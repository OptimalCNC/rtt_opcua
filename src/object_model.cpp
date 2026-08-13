#include <rtt/opcua/object_model.hpp>

#include "component_state.hpp"
#include "operation_dispatcher.hpp"
#include "port_bridge.hpp"

#include <rtt/opcua/endpoint_type_registry.hpp>
#include <rtt/opcua/node_id.hpp>

#include <open62541pp/plugin/nodestore.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/nodemanagement.hpp>
#include <open62541pp/services/view.hpp>
#include <open62541pp/ua/nodeids.hpp>
#include <open62541/server.h>

#include <rtt/Logger.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/base/PropertyBase.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace RTT::opcua {
namespace detail {
namespace {

constexpr std::size_t kMaximumServiceDepth = 32U;

std::string appendNodeSegment(std::string path, std::string_view segment) {
  path += '/';
  path += escapeNodeIdSegment(segment);
  return path;
}

::opcua::NodeId nodeId(std::uint16_t namespace_index, const std::string &path) {
  return ::opcua::NodeId(namespace_index, path);
}

std::size_t pathDepth(const std::string &path) {
  return static_cast<std::size_t>(std::count(path.begin(), path.end(), '/'));
}

void assignError(std::string *output, std::string value) {
  if (output != nullptr) {
    *output = std::move(value);
  }
}

void appendError(std::string *output, std::string value) {
  if (output == nullptr || value.empty()) {
    return;
  }
  if (!output->empty()) {
    *output += "; ";
  }
  *output += std::move(value);
}

std::string pointerFingerprint(const void *pointer) {
  std::ostringstream stream;
  stream << pointer;
  return stream.str();
}

std::string statusName(::opcua::StatusCode status) {
  return std::string(status.name());
}

bool componentNodeCreated(
    const ::opcua::Result<::opcua::NodeId> &result, const std::string &path,
    bool *created, std::string *error) {
  if (result) {
    *created = true;
    return true;
  }
  assignError(error, "failed to create OPC UA component node '" + path +
                         "': " + statusName(result.code()));
  return false;
}

bool sharedRootEnsured(const ::opcua::Result<::opcua::NodeId> &result,
                       const std::string &path, std::string *error) {
  if (result || result.code() == UA_STATUSCODE_BADNODEIDEXISTS) {
    return true;
  }
  assignError(error, "failed to ensure OPC UA root node '" + path + "': " +
                         statusName(result.code()));
  return false;
}

std::string taskStateName(RTT::base::TaskCore::TaskState state) {
  switch (state) {
  case RTT::base::TaskCore::Init:
    return "Init";
  case RTT::base::TaskCore::PreOperational:
    return "PreOperational";
  case RTT::base::TaskCore::FatalError:
    return "FatalError";
  case RTT::base::TaskCore::Exception:
    return "Exception";
  case RTT::base::TaskCore::Stopped:
    return "Stopped";
  case RTT::base::TaskCore::Running:
    return "Running";
  case RTT::base::TaskCore::RunTimeError:
    return "RunTimeError";
  }
  return "Unknown";
}

::opcua::Bitmask<::opcua::AccessLevel> readOnlyAccess() {
  return ::opcua::AccessLevel::CurrentRead;
}

::opcua::Bitmask<::opcua::AccessLevel> readWriteAccess() {
  return ::opcua::AccessLevel::CurrentRead | ::opcua::AccessLevel::CurrentWrite;
}

class GuardedValueDataSource final : public ::opcua::DataSourceBase {
public:
  GuardedValueDataSource(
      std::weak_ptr<ComponentState> state,
      RTT::base::DataSourceBase::shared_ptr source,
      std::shared_ptr<const EndpointTypeRegistry> type_registry,
      const TypeCodec *codec, bool writable)
      : state_(std::move(state)), source_(std::move(source)),
        type_registry_(std::move(type_registry)), codec_(codec),
        writable_(writable) {}

  ::opcua::StatusCode read(::opcua::Session &, const ::opcua::NodeId &,
                           const ::opcua::NumericRange *range,
                           ::opcua::DataValue &value, bool) override {
    if (range != nullptr) {
      value.setStatus(UA_STATUSCODE_BADINDEXRANGEINVALID);
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    const auto state = state_.lock();
    ComponentLease lease(state);
    if (!lease) {
      value.setStatus(UA_STATUSCODE_BADNOTCONNECTED);
      return UA_STATUSCODE_BADNOTCONNECTED;
    }

    ::opcua::Variant encoded;
    if (codec_ == nullptr || !codec_->toVariant(source_, &encoded)) {
      value.setStatus(UA_STATUSCODE_BADTYPEMISMATCH);
      return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    value.setValue(std::move(encoded));
    return UA_STATUSCODE_GOOD;
  }

  ::opcua::StatusCode write(::opcua::Session &, const ::opcua::NodeId &,
                            const ::opcua::NumericRange *range,
                            const ::opcua::DataValue &value) override {
    if (range != nullptr) {
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    if (!writable_) {
      return UA_STATUSCODE_BADNOTWRITABLE;
    }
    if (!value.hasValue()) {
      return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    const auto state = state_.lock();
    ComponentLease lease(state);
    if (!lease) {
      return UA_STATUSCODE_BADNOTCONNECTED;
    }
    return codec_ != nullptr && codec_->assignVariant(value.value(), source_)
               ? ::opcua::StatusCode(UA_STATUSCODE_GOOD)
               : ::opcua::StatusCode(UA_STATUSCODE_BADTYPEMISMATCH);
  }

private:
  std::weak_ptr<ComponentState> state_;
  RTT::base::DataSourceBase::shared_ptr source_;
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  const TypeCodec *codec_;
  bool writable_;
};

class PortValueDataSource final : public ::opcua::DataSourceBase {
public:
  PortValueDataSource(std::weak_ptr<ComponentState> state,
                      RTT::base::OutputPortInterface *port,
                      std::shared_ptr<const EndpointTypeRegistry> type_registry,
                      const TypeCodec *codec)
      : state_(std::move(state)), port_(port),
        type_registry_(std::move(type_registry)), codec_(codec) {}

  ::opcua::StatusCode read(::opcua::Session &, const ::opcua::NodeId &,
                           const ::opcua::NumericRange *range,
                           ::opcua::DataValue &value, bool) override {
    if (range != nullptr) {
      value.setStatus(UA_STATUSCODE_BADINDEXRANGEINVALID);
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    ComponentLease lease(state_.lock());
    if (!lease || port_ == nullptr) {
      value.setStatus(UA_STATUSCODE_BADNOTCONNECTED);
      return UA_STATUSCODE_BADNOTCONNECTED;
    }

    ::opcua::Variant encoded;
    switch (codec_ == nullptr ? PortValueStatus::error
                              : codec_->portValue(port_, &encoded)) {
    case PortValueStatus::value:
      value.setValue(std::move(encoded));
      return UA_STATUSCODE_GOOD;
    case PortValueStatus::waiting_for_initial_data:
      value.setStatus(UA_STATUSCODE_BADWAITINGFORINITIALDATA);
      return UA_STATUSCODE_BADWAITINGFORINITIALDATA;
    case PortValueStatus::error:
      value.setStatus(UA_STATUSCODE_BADTYPEMISMATCH);
      return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    value.setStatus(UA_STATUSCODE_BADUNEXPECTEDERROR);
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  }

  ::opcua::StatusCode write(::opcua::Session &, const ::opcua::NodeId &,
                            const ::opcua::NumericRange *,
                            const ::opcua::DataValue &) override {
    return UA_STATUSCODE_BADNOTWRITABLE;
  }

private:
  std::weak_ptr<ComponentState> state_;
  RTT::base::OutputPortInterface *port_;
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  const TypeCodec *codec_;
};

class LifecycleDataSource final : public ::opcua::DataSourceBase {
public:
  explicit LifecycleDataSource(std::weak_ptr<ComponentState> state)
      : state_(std::move(state)) {}

  ::opcua::StatusCode read(::opcua::Session &, const ::opcua::NodeId &,
                           const ::opcua::NumericRange *range,
                           ::opcua::DataValue &value, bool) override {
    if (range != nullptr) {
      value.setStatus(UA_STATUSCODE_BADINDEXRANGEINVALID);
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    const auto state = state_.lock();
    ComponentLease lease(state);
    if (!lease) {
      value.setStatus(UA_STATUSCODE_BADNOTCONNECTED);
      return UA_STATUSCODE_BADNOTCONNECTED;
    }
    value.setValue(
        ::opcua::Variant(taskStateName(lease.get()->getTaskState())));
    return UA_STATUSCODE_GOOD;
  }

  ::opcua::StatusCode write(::opcua::Session &, const ::opcua::NodeId &,
                            const ::opcua::NumericRange *,
                            const ::opcua::DataValue &) override {
    return UA_STATUSCODE_BADNOTWRITABLE;
  }

private:
  std::weak_ptr<ComponentState> state_;
};

enum class NodeKind { object, variable, method };

struct CreatedNode {
  ::opcua::NodeId id;
  bool recursive_root{false};
};

struct NodeSpec {
  NodeKind kind;
  std::string path;
  std::string parent_path;
  std::string browse_name;
  std::string fingerprint;
  bool expects_input_arguments{false};
  bool expects_output_arguments{false};
  std::vector<::opcua::Argument> expected_input_arguments;
  std::vector<::opcua::Argument> expected_output_arguments;
  std::function<bool(::opcua::Server &, std::uint16_t, bool *, std::string *)>
      create;
};

using NodeMap = std::map<std::string, NodeSpec>;

bool createNode(::opcua::Server &native, std::uint16_t namespace_index,
                const NodeSpec &spec, bool *created,
                std::string *error) noexcept {
  *created = false;
  try {
    return spec.create(native, namespace_index, created, error);
  } catch (const std::exception &exception) {
    assignError(error, "OPC UA node creator threw for '" + spec.path +
                           "': " + exception.what());
  } catch (...) {
    assignError(error, "OPC UA node creator threw for '" + spec.path + "'");
  }
  return false;
}

bool isMethodArgument(const ::opcua::ReferenceDescription &reference) {
  const auto browse_name = reference.browseName();
  return reference.nodeClass() == ::opcua::NodeClass::Variable &&
         browse_name.namespaceIndex() == 0U &&
         (browse_name.name() == "InputArguments" ||
          browse_name.name() == "OutputArguments");
}

bool argumentsMatch(const ::opcua::Argument &actual,
                    const ::opcua::Argument &expected) {
  return actual.name() == expected.name() &&
         actual.dataType() == expected.dataType() &&
         actual.valueRank() == expected.valueRank() &&
         std::ranges::equal(actual.arrayDimensions(),
                            expected.arrayDimensions());
}

bool validateMethodArguments(::opcua::Server &native, const ::opcua::NodeId &id,
                             const std::vector<::opcua::Argument> &expected,
                             const std::string &path, std::string *error) {
  const auto data_type = ::opcua::services::readDataType(native, id);
  const auto value_rank = ::opcua::services::readValueRank(native, id);
  const auto value = ::opcua::services::readValue(native, id);
  if (!data_type ||
      data_type.value() != ::opcua::NodeId(::opcua::DataTypeId::Argument) ||
      !value_rank || value_rank.value() != ::opcua::ValueRank::OneDimension ||
      !value) {
    assignError(error,
                "invalid OPC UA method argument schema for '" + path + "'");
    return false;
  }
  try {
    const auto actual = value.value().to<std::vector<::opcua::Argument>>();
    if (actual.size() != expected.size() ||
        !std::ranges::equal(actual, expected, argumentsMatch)) {
      assignError(error, "unexpected OPC UA method argument schema for '" +
                             path + "'");
      return false;
    }
  } catch (const std::exception &exception) {
    assignError(error, "invalid OPC UA method argument schema for '" + path +
                           "': " + exception.what());
    return false;
  }
  return true;
}

bool recordMethodArgumentNodes(::opcua::Server &native,
                               std::uint16_t namespace_index,
                               const NodeSpec &spec,
                               std::vector<CreatedNode> &ledger,
                               std::string *error) {
  const ::opcua::NodeId primary_id = nodeId(namespace_index, spec.path);
  const ::opcua::BrowseDescription browse(
      primary_id, ::opcua::BrowseDirection::Forward,
      ::opcua::ReferenceTypeId::HasProperty, true,
      ::opcua::NodeClass::Variable, ::opcua::BrowseResultMask::All);
  const auto references = ::opcua::services::browseAll(native, browse);
  if (!references) {
    assignError(error, "failed to inspect OPC UA method arguments for '" +
                           spec.path + "': " + statusName(references.code()));
    return false;
  }

  std::map<std::string, ::opcua::NodeId, std::less<>> arguments;
  bool unexpected_property = false;
  for (const auto &reference : references.value()) {
    if (!reference.nodeId().isLocal() || !isMethodArgument(reference)) {
      unexpected_property = true;
      continue;
    }
    const ::opcua::NodeId argument_id = reference.nodeId().nodeId();
    ledger.push_back(CreatedNode{argument_id, false});
  }
  if (unexpected_property) {
    assignError(error, "unexpected OPC UA method property while recording '" +
                           spec.path + "'");
    return false;
  }

  for (const auto &reference : references.value()) {
    const std::string name(reference.browseName().name());
    const ::opcua::NodeId argument_id = reference.nodeId().nodeId();
    const std::vector<::opcua::Argument> *expected = nullptr;
    if (name == "InputArguments" && spec.expects_input_arguments) {
      expected = &spec.expected_input_arguments;
    } else if (name == "OutputArguments" && spec.expects_output_arguments) {
      expected = &spec.expected_output_arguments;
    }
    if (expected == nullptr ||
        !arguments.emplace(name, argument_id).second ||
        !validateMethodArguments(native, argument_id, *expected, spec.path,
                                 error)) {
      if (error != nullptr && error->empty()) {
        assignError(error,
                    "unexpected OPC UA method property while recording '" +
                        spec.path + "'");
      }
      return false;
    }
  }

  const std::size_t expected_count =
      static_cast<std::size_t>(spec.expects_input_arguments) +
      static_cast<std::size_t>(spec.expects_output_arguments);
  if (arguments.size() != expected_count) {
    assignError(error,
                "missing OPC UA method argument property while recording '" +
                    spec.path + "'");
    return false;
  }
  return true;
}

struct ComponentSnapshot {
  NodeMap nodes;
  std::vector<UnsupportedResource> unsupported;
  std::string fingerprint;
};

std::string appendDiagnosticSegment(std::string_view path,
                                    std::string_view segment) {
  if (path.empty()) {
    return std::string(segment);
  }
  return std::string(path) + '.' + std::string(segment);
}

std::string typeName(const RTT::types::TypeInfo *type) {
  return type == nullptr ? "<unknown>" : type->getTypeName();
}

std::string unsupportedValueReason(const RTT::types::TypeInfo *type,
                                   const TypeCodec *codec) {
  if (type == nullptr) {
    return "has no RTT type information";
  }
  if (codec == nullptr) {
    return "has no registered OPC UA protocol";
  }
  return "does not support OPC UA values";
}

void appendUnsupported(std::vector<UnsupportedResource> &unsupported,
                       const std::string &component, std::string path,
                       std::string kind, const RTT::types::TypeInfo *type,
                       const TypeCodec *codec) {
  unsupported.push_back(
      UnsupportedResource{component, std::move(path), std::move(kind),
                          typeName(type), unsupportedValueReason(type, codec)});
}

void appendUnsupported(std::vector<UnsupportedResource> &unsupported,
                       const std::string &component, std::string path,
                       std::string kind, std::string type_name,
                       std::string reason) {
  unsupported.push_back(UnsupportedResource{
      component, std::move(path), std::move(kind),
      type_name.empty() ? "<unknown>" : std::move(type_name),
      reason.empty() ? "is not supported by OPC UA" : std::move(reason)});
}

bool addObjectNode(::opcua::Server &server, std::uint16_t namespace_index,
                   const std::string &parent_path, const std::string &path,
                   const std::string &browse_name,
                   const std::string &description, bool *created,
                   std::string *error) {
  ::opcua::ObjectAttributes attributes;
  attributes.setDisplayName(::opcua::LocalizedText("en-US", browse_name));
  if (!description.empty()) {
    attributes.setDescription(::opcua::LocalizedText("en-US", description));
  }
  const auto result = ::opcua::services::addObject(
      server, nodeId(namespace_index, parent_path),
      nodeId(namespace_index, path), browse_name, attributes,
      ::opcua::ObjectTypeId::BaseObjectType,
      ::opcua::ReferenceTypeId::HasComponent);
  return componentNodeCreated(result, path, created, error);
}

NodeSpec objectSpec(std::string path, std::string parent_path,
                    std::string browse_name, std::string description = {}) {
  NodeSpec spec;
  spec.kind = NodeKind::object;
  spec.path = std::move(path);
  spec.parent_path = std::move(parent_path);
  spec.browse_name = std::move(browse_name);
  spec.fingerprint =
      "object|" + spec.parent_path + "|" + spec.browse_name + "|" + description;
  spec.create = [path = spec.path, parent = spec.parent_path,
                 name = spec.browse_name, description = std::move(description)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    return addObjectNode(server, namespace_index, parent, path, name,
                         description, created, error);
  };
  return spec;
}

bool addStaticStringVariable(
    ::opcua::Server &server, std::uint16_t namespace_index,
    const std::string &parent_path, const std::string &path,
    const std::string &browse_name, const std::string &description,
    const std::string &value, bool property, bool *created,
    std::string *error) {
  ::opcua::VariableAttributes attributes;
  attributes.setDisplayName(::opcua::LocalizedText("en-US", browse_name));
  if (!description.empty()) {
    attributes.setDescription(::opcua::LocalizedText("en-US", description));
  }
  attributes.setValue(::opcua::Variant(value));
  attributes.setDataType(::opcua::DataTypeId::String);
  attributes.setValueRank(::opcua::ValueRank::Scalar);
  attributes.setAccessLevel(readOnlyAccess());
  attributes.setUserAccessLevel(readOnlyAccess());
  const auto result = ::opcua::services::addVariable(
      server, nodeId(namespace_index, parent_path),
      nodeId(namespace_index, path), browse_name, attributes,
      ::opcua::VariableTypeId::BaseDataVariableType,
      property ? ::opcua::ReferenceTypeId::HasProperty
               : ::opcua::ReferenceTypeId::HasComponent);
  return componentNodeCreated(result, path, created, error);
}

NodeSpec staticStringSpec(std::string path, std::string parent_path,
                          std::string browse_name, std::string description,
                          std::string value, bool property = false) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.path = std::move(path);
  spec.parent_path = std::move(parent_path);
  spec.browse_name = std::move(browse_name);
  spec.fingerprint = "string|" + spec.parent_path + "|" + spec.browse_name +
                     "|" + description + "|" + value +
                     (property ? "|property" : "|component");
  spec.create = [path = spec.path, parent = spec.parent_path,
                 name = spec.browse_name, description = std::move(description),
                 value = std::move(value),
                 property](::opcua::Server &server,
                           std::uint16_t namespace_index, bool *created,
                           std::string *error) {
    return addStaticStringVariable(server, namespace_index, parent, path, name,
                                   description, value, property, created, error);
  };
  return spec;
}

template <typename T>
NodeSpec staticArrayPropertySpec(std::string path, std::string parent_path,
                                 std::string browse_name,
                                 std::string description, std::vector<T> values,
                                 ::opcua::NodeId data_type) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.path = std::move(path);
  spec.parent_path = std::move(parent_path);
  spec.browse_name = std::move(browse_name);
  std::ostringstream fingerprint;
  fingerprint << "array-property|" << spec.parent_path << '|'
              << spec.browse_name << '|' << description;
  for (const auto &value : values) {
    fingerprint << '|' << value;
  }
  spec.fingerprint = fingerprint.str();
  spec.create = [path = spec.path, parent = spec.parent_path,
                 name = spec.browse_name, description = std::move(description),
                 values = std::move(values), data_type = std::move(data_type)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", name));
    attributes.setDescription(::opcua::LocalizedText("en-US", description));
    ::opcua::Variant encoded(values);
    if (!encoded.isType(data_type)) {
      assignError(error,
                  "RTT metadata Variant type mismatch for '" + path + "'");
      return false;
    }
    attributes.setValue(std::move(encoded));
    attributes.setDataType(data_type);
    attributes.setValueRank(::opcua::ValueRank::OneDimension);
    attributes.setArrayDimensions({0U});
    attributes.setAccessLevel(readOnlyAccess());
    attributes.setUserAccessLevel(readOnlyAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        name, attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasProperty);
    return componentNodeCreated(result, path, created, error);
  };
  return spec;
}

NodeSpec lifecycleSpec(const std::string &component_path,
                       const std::shared_ptr<ComponentState> &state) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = component_path;
  spec.path = appendNodeSegment(component_path, "lifecycleState");
  spec.browse_name = "lifecycleState";
  spec.fingerprint = "lifecycle|" + state->component_name;
  spec.create = [path = spec.path, parent = spec.parent_path,
                 weak_state = std::weak_ptr<ComponentState>(state)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    const auto current_state = weak_state.lock();
    ComponentLease lease(current_state);
    if (!lease) {
      assignError(
          error,
          "component became unavailable while publishing lifecycle state");
      return false;
    }
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(
        ::opcua::LocalizedText("en-US", "lifecycleState"));
    attributes.setDescription(::opcua::LocalizedText(
        "en-US", "Current RTT component lifecycle state."));
    attributes.setValue(
        ::opcua::Variant(taskStateName(lease.get()->getTaskState())));
    attributes.setDataType(::opcua::DataTypeId::String);
    attributes.setValueRank(::opcua::ValueRank::Scalar);
    attributes.setAccessLevel(readOnlyAccess());
    attributes.setUserAccessLevel(readOnlyAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        "lifecycleState", attributes,
        ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!componentNodeCreated(result, path, created, error)) {
      return false;
    }
    ::opcua::setVariableNodeValueBackend(
        server, nodeId(namespace_index, path),
        std::make_unique<LifecycleDataSource>(weak_state));
    return true;
  };
  return spec;
}

NodeSpec
dataSourceSpec(const std::string &parent_path, const std::string &name,
               const std::string &description,
               const RTT::base::DataSourceBase::shared_ptr &source,
               const std::shared_ptr<ComponentState> &state,
               std::shared_ptr<const EndpointTypeRegistry> type_registry) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = parent_path;
  spec.path = appendNodeSegment(parent_path, name);
  spec.browse_name = name;
  const TypeCodec *codec =
      type_registry ? type_registry->codecForDataSource(source) : nullptr;
  const bool writable = source->isAssignable();
  spec.fingerprint = "rtt-value|" + pointerFingerprint(source.get()) + "|" +
                     (writable ? "rw|" : "ro|") + description;
  spec.create = [path = spec.path, parent = spec.parent_path, name, description,
                 source, type_registry = std::move(type_registry), codec,
                 writable, weak_state = std::weak_ptr<ComponentState>(state)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    ::opcua::Variant value;
    const auto current_state = weak_state.lock();
    ComponentLease lease(current_state);
    if (!lease || codec == nullptr || !codec->toVariant(source, &value)) {
      assignError(error,
                  "failed to encode RTT value for OPC UA node '" + path + "'");
      return false;
    }

    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", name));
    if (!description.empty()) {
      attributes.setDescription(::opcua::LocalizedText("en-US", description));
    }
    attributes.setValue(std::move(value));
    attributes.setDataType(codec->dataTypeNodeId());
    attributes.setValueRank(codec->valueRank());
    if (codec->valueRank() == ::opcua::ValueRank::OneDimension) {
      attributes.setArrayDimensions({0U});
    }
    attributes.setAccessLevel(writable ? readWriteAccess() : readOnlyAccess());
    attributes.setUserAccessLevel(writable ? readWriteAccess()
                                           : readOnlyAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        name, attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!componentNodeCreated(result, path, created, error)) {
      return false;
    }
    ::opcua::setVariableNodeValueBackend(
        server, nodeId(namespace_index, path),
        std::make_unique<GuardedValueDataSource>(
            weak_state, source, type_registry, codec, writable));
    return true;
  };
  return spec;
}

NodeSpec
portValueSpec(const std::string &port_path,
              RTT::base::OutputPortInterface &port,
              const std::shared_ptr<ComponentState> &state,
              std::shared_ptr<const EndpointTypeRegistry> type_registry) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = port_path;
  spec.path = appendNodeSegment(port_path, "value");
  spec.browse_name = "value";
  spec.fingerprint = "port-value|" + pointerFingerprint(&port);
  spec.create = [path = spec.path, parent = spec.parent_path, port = &port,
                 weak_state = std::weak_ptr<ComponentState>(state),
                 type_registry = std::move(type_registry)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    ComponentLease lease(weak_state.lock());
    if (!lease || port == nullptr || port->getTypeInfo() == nullptr) {
      assignError(
          error,
          "RTT output port became unavailable while creating OPC UA value");
      return false;
    }
    const TypeCodec *codec =
        type_registry ? type_registry->codecForTypeInfo(port->getTypeInfo())
                      : nullptr;
    if (codec == nullptr || !codec->hasValue()) {
      assignError(error, "RTT output port type has no OPC UA protocol");
      return false;
    }

    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", "value"));
    attributes.setDescription(
        ::opcua::LocalizedText("en-US", "Current RTT output-port value."));
    attributes.setDataType(codec->dataTypeNodeId());
    attributes.setValueRank(codec->valueRank());
    if (codec->valueRank() == ::opcua::ValueRank::OneDimension) {
      attributes.setArrayDimensions({0U});
    }
    attributes.setAccessLevel(readOnlyAccess());
    attributes.setUserAccessLevel(readOnlyAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        "value", attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!componentNodeCreated(result, path, created, error)) {
      return false;
    }
    ::opcua::setVariableNodeValueBackend(
        server, nodeId(namespace_index, path),
        std::make_unique<PortValueDataSource>(weak_state, port, type_registry,
                                              codec));
    return true;
  };
  return spec;
}

NodeSpec operationSpec(const std::string &parent_path, const std::string &name,
                       const std::string &description,
                       const RTT::Service::shared_ptr &service,
                       RTT::OperationInterfacePart &operation,
                       const std::shared_ptr<ComponentState> &state,
                       const std::shared_ptr<OperationDispatcher> &dispatcher,
                       OperationSchema schema) {
  NodeSpec spec;
  spec.kind = NodeKind::method;
  spec.parent_path = parent_path;
  spec.path = appendNodeSegment(parent_path, name);
  spec.browse_name = name;
  spec.fingerprint = "method|" + spec.parent_path + "|" + spec.browse_name +
                     "|" + description + "|" + schema.fingerprint;
  spec.expects_input_arguments = !schema.inputs.empty();
  spec.expects_output_arguments = !schema.outputs.empty();
  spec.expected_input_arguments = schema.inputs;
  spec.expected_output_arguments = schema.outputs;
  spec.create = [path = spec.path, parent = spec.parent_path, name, description,
                 service, operation = &operation,
                 weak_state = std::weak_ptr<ComponentState>(state), dispatcher,
                 schema = std::move(schema)](::opcua::Server &server,
                                             std::uint16_t namespace_index,
                                             bool *created,
                                             std::string *error) {
    ::opcua::MethodAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", name));
    if (!description.empty()) {
      attributes.setDescription(::opcua::LocalizedText("en-US", description));
    }
    attributes.setExecutable(true);
    attributes.setUserExecutable(true);

    ::opcua::services::MethodCallback callback =
        std::function<::opcua::StatusCode(
            ::opcua::Session &, ::opcua::Span<const ::opcua::Variant>,
            ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
            const ::opcua::NodeId &)>(
            [service, operation, weak_state,
             dispatcher](::opcua::Session &,
                         ::opcua::Span<const ::opcua::Variant> inputs,
                         ::opcua::Span<::opcua::Variant> outputs,
                         const ::opcua::NodeId &, const ::opcua::NodeId &) {
              static_cast<void>(service);
              const auto current_state = weak_state.lock();
              if (!current_state || operation == nullptr) {
                return ::opcua::StatusCode(UA_STATUSCODE_BADNOTCONNECTED);
              }
              return dispatcher->invoke(current_state, *operation, inputs,
                                        outputs);
            });

    const auto result = ::opcua::services::addMethod(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        name, std::move(callback), schema.inputs, schema.outputs, attributes,
        ::opcua::ReferenceTypeId::HasComponent);
    return componentNodeCreated(result, path, created, error);
  };
  return spec;
}

enum class PortMethodKind { read, write };

std::pair<std::vector<::opcua::Argument>, std::vector<::opcua::Argument>>
portMethodArguments(const TypeCodec &sample_codec,
                    const TypeCodec &status_codec, bool reads) {
  std::vector<::opcua::Argument> inputs;
  std::vector<::opcua::Argument> outputs;
  const ::opcua::Argument status_argument(
      "status", ::opcua::LocalizedText("en-US", "RTT port flow status."),
      status_codec.dataTypeNodeId(), status_codec.valueRank());
  const ::opcua::Argument value_argument(
      "value", ::opcua::LocalizedText("en-US", "RTT port sample."),
      sample_codec.dataTypeNodeId(), sample_codec.valueRank());
  if (reads) {
    outputs.push_back(status_argument);
    outputs.push_back(value_argument);
  } else {
    inputs.push_back(value_argument);
    outputs.push_back(status_argument);
  }
  return {std::move(inputs), std::move(outputs)};
}

NodeSpec
portMethodSpec(const std::string &port_path, RTT::base::PortInterface &port,
               const std::shared_ptr<ComponentState> &state,
               std::shared_ptr<const EndpointTypeRegistry> type_registry,
               std::size_t buffer_size, PortMethodKind method_kind) {
  const bool reads = method_kind == PortMethodKind::read;
  const std::string method_name = reads ? "read" : "write";
  NodeSpec spec;
  spec.kind = NodeKind::method;
  spec.parent_path = port_path;
  spec.path = appendNodeSegment(port_path, method_name);
  spec.browse_name = method_name;
  spec.fingerprint = "port-method|" + pointerFingerprint(&port) + "|" +
                     method_name + "|" + std::to_string(buffer_size);
  spec.expects_input_arguments = !reads;
  spec.expects_output_arguments = true;
  const TypeCodec *expected_codec =
      type_registry && port.getTypeInfo()
          ? type_registry->codecForTypeInfo(port.getTypeInfo())
          : nullptr;
  const TypeCodec *expected_status_codec =
      type_registry
          ? type_registry->codecForTypeName(reads ? "FlowStatus"
                                                  : "WriteStatus")
          : nullptr;
  if (expected_codec != nullptr && expected_status_codec != nullptr) {
    auto [inputs, outputs] =
        portMethodArguments(*expected_codec, *expected_status_codec, reads);
    spec.expected_input_arguments = std::move(inputs);
    spec.expected_output_arguments = std::move(outputs);
  }
  const auto bridge_slot = std::make_shared<std::shared_ptr<PortBridge>>();
  spec.create = [path = spec.path, parent = spec.parent_path, method_name,
                 port = &port,
                 weak_state = std::weak_ptr<ComponentState>(state), buffer_size,
                 type_registry = std::move(type_registry), reads, bridge_slot,
                 inputs = spec.expected_input_arguments,
                 outputs = spec.expected_output_arguments](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    const auto current_state = weak_state.lock();
    ComponentLease lease(current_state);
    if (!lease || port == nullptr || port->getTypeInfo() == nullptr) {
      assignError(error,
                  "RTT port became unavailable while creating OPC UA method");
      return false;
    }
    const TypeCodec *codec =
        type_registry ? type_registry->codecForTypeInfo(port->getTypeInfo())
                      : nullptr;
    const TypeCodec *status_codec =
        type_registry
            ? type_registry->codecForTypeName(reads ? "FlowStatus"
                                                    : "WriteStatus")
            : nullptr;
    if (codec == nullptr || !codec->hasValue() || status_codec == nullptr ||
        !status_codec->hasValue()) {
      assignError(error, "RTT port type has no OPC UA protocol");
      return false;
    }

    std::string bridge_error;
    const auto bridge =
        PortBridge::create(*port, type_registry, buffer_size, &bridge_error);
    if (!bridge) {
      assignError(error, std::move(bridge_error));
      return false;
    }

    ::opcua::MethodAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", method_name));
    attributes.setDescription(::opcua::LocalizedText(
        "en-US", reads ? "Read the next RTT output-port sample."
                       : "Write a sample to the RTT input port."));
    attributes.setExecutable(true);
    attributes.setUserExecutable(true);

    ::opcua::services::MethodCallback callback =
        std::function<::opcua::StatusCode(
            ::opcua::Session &, ::opcua::Span<const ::opcua::Variant>,
            ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
            const ::opcua::NodeId &)>(
            [weak_bridge = std::weak_ptr<PortBridge>(bridge), weak_state,
             reads](::opcua::Session &,
                    ::opcua::Span<const ::opcua::Variant> method_inputs,
                    ::opcua::Span<::opcua::Variant> method_outputs,
                    const ::opcua::NodeId &, const ::opcua::NodeId &) {
              ComponentLease callback_lease(weak_state.lock());
              const auto current_bridge = weak_bridge.lock();
              if (!callback_lease || !current_bridge) {
                return ::opcua::StatusCode(UA_STATUSCODE_BADNOTCONNECTED);
              }
              return reads
                         ? current_bridge->read(method_outputs)
                         : current_bridge->write(method_inputs, method_outputs);
            });

    const auto result = ::opcua::services::addMethod(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        method_name, std::move(callback), inputs, outputs, attributes,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!componentNodeCreated(result, path, created, error)) {
      return false;
    }
    *bridge_slot = bridge;
    return true;
  };
  return spec;
}

void insertNode(NodeMap &nodes, NodeSpec spec) {
  nodes.insert_or_assign(spec.path, std::move(spec));
}

void appendOperationNodes(
    NodeMap &nodes, std::vector<UnsupportedResource> &unsupported,
    const RTT::Service::shared_ptr &service, const std::string &owner_path,
    const std::string &diagnostic_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<OperationDispatcher> &dispatcher) {
  const std::string operations_path =
      appendNodeSegment(owner_path, "operations");
  for (const std::string &name : service->getOperationNames()) {
    RTT::OperationInterfacePart *operation = service->getOperation(name);
    if (operation == nullptr) {
      continue;
    }
    OperationSchema schema = dispatcher->describe(*operation);
    if (!schema.supported) {
      appendUnsupported(unsupported, state->component_name,
                        appendDiagnosticSegment(diagnostic_path, name),
                        "operation", std::move(schema.unsupported_type_name),
                        std::move(schema.unsupported_reason));
      continue;
    }
    const std::string operation_path = appendNodeSegment(operations_path, name);
    if (!schema.input_type_names.empty()) {
      insertNode(nodes,
                 staticArrayPropertySpec(
                     appendNodeSegment(operation_path, "rttInputTypes"),
                     operation_path, "rttInputTypes",
                     "Canonical RTT input argument types.",
                     schema.input_type_names, ::opcua::DataTypeId::String));
    }
    if (!schema.output_type_names.empty()) {
      insertNode(nodes,
                 staticArrayPropertySpec(
                     appendNodeSegment(operation_path, "rttOutputTypes"),
                     operation_path, "rttOutputTypes",
                     "Canonical RTT output argument types.",
                     schema.output_type_names, ::opcua::DataTypeId::String));
      insertNode(
          nodes,
          staticArrayPropertySpec(
              appendNodeSegment(operation_path, "rttOutputSources"),
              operation_path, "rttOutputSources",
              "Return value (-1) or mutable input index for each output.",
              schema.output_sources, ::opcua::DataTypeId::Int32));
    }
    insertNode(nodes,
               operationSpec(operations_path, name, operation->description(),
                             service, *operation, state, dispatcher,
                             std::move(schema)));
  }
}

void appendResourceFolders(NodeMap &nodes, const std::string &owner_path,
                           const std::string &description) {
  for (const auto &[segment, browse_name] :
       std::initializer_list<std::pair<std::string_view, std::string_view>>{
           {"operations", "Operations"},
           {"properties", "Properties"},
           {"attributes", "Attributes"},
           {"ports", "Ports"},
           {"services", "Services"}}) {
    insertNode(nodes,
               objectSpec(appendNodeSegment(owner_path, segment), owner_path,
                          std::string(browse_name), description));
  }
}

void appendConfigurationNodes(
    NodeMap &nodes, std::vector<UnsupportedResource> &unsupported,
    const RTT::Service::shared_ptr &service, const std::string &owner_path,
    const std::string &diagnostic_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry) {
  const std::string properties_path =
      appendNodeSegment(owner_path, "properties");
  for (const std::string &name : service->properties()->getPropertyNames()) {
    RTT::base::PropertyBase *property = service->getProperty(name);
    if (property == nullptr) {
      continue;
    }
    const auto source = property->getDataSource();
    const RTT::types::TypeInfo *type =
        source == nullptr ? nullptr : source->getTypeInfo();
    const TypeCodec *codec =
        source == nullptr ? nullptr : type_registry->codecForDataSource(source);
    if (codec == nullptr || !codec->hasValue()) {
      appendUnsupported(unsupported, state->component_name,
                        appendDiagnosticSegment(diagnostic_path, name),
                        "property", type, codec);
      continue;
    }
    insertNode(nodes,
               dataSourceSpec(properties_path, name, property->getDescription(),
                              source, state, type_registry));
    const std::string property_path = appendNodeSegment(properties_path, name);
    insertNode(nodes, staticStringSpec(
                          appendNodeSegment(property_path, "rttType"),
                          property_path, "rttType", "Canonical RTT value type.",
                          source->getTypeInfo()->getTypeName(), true));
  }

  const std::string attributes_path =
      appendNodeSegment(owner_path, "attributes");
  for (const std::string &name : service->getAttributeNames()) {
    RTT::base::AttributeBase *attribute = service->getValue(name);
    if (attribute == nullptr) {
      continue;
    }
    const auto source = attribute->getDataSource();
    const RTT::types::TypeInfo *type =
        source == nullptr ? nullptr : source->getTypeInfo();
    const TypeCodec *codec =
        source == nullptr ? nullptr : type_registry->codecForDataSource(source);
    if (codec == nullptr || !codec->hasValue()) {
      appendUnsupported(unsupported, state->component_name,
                        appendDiagnosticSegment(diagnostic_path, name),
                        "attribute", type, codec);
      continue;
    }
    insertNode(nodes, dataSourceSpec(attributes_path, name, {}, source, state,
                                     type_registry));
    const std::string attribute_path = appendNodeSegment(attributes_path, name);
    insertNode(nodes,
               staticStringSpec(appendNodeSegment(attribute_path, "rttType"),
                                attribute_path, "rttType",
                                "Canonical RTT value type.",
                                source->getTypeInfo()->getTypeName(), true));
  }
}

void appendPortNodes(
    NodeMap &nodes, std::vector<UnsupportedResource> &unsupported,
    const RTT::Service::shared_ptr &service, const std::string &owner_path,
    const std::string &diagnostic_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry,
    std::size_t buffer_size) {
  const std::string ports_path = appendNodeSegment(owner_path, "ports");
  for (const std::string &name : service->getPortNames()) {
    RTT::base::PortInterface *port = service->getPort(name);
    if (port == nullptr) {
      continue;
    }
    const RTT::types::TypeInfo *type = port->getTypeInfo();
    const TypeCodec *codec =
        type == nullptr ? nullptr : type_registry->codecForTypeInfo(type);
    const bool is_input =
        dynamic_cast<RTT::base::InputPortInterface *>(port) != nullptr;
    const bool is_output =
        dynamic_cast<RTT::base::OutputPortInterface *>(port) != nullptr;
    if (codec == nullptr || !codec->hasValue()) {
      appendUnsupported(unsupported, state->component_name,
                        appendDiagnosticSegment(diagnostic_path, name),
                        is_input ? "input port"
                                 : (is_output ? "output port" : "port"),
                        type, codec);
      continue;
    }
    const std::string port_path = appendNodeSegment(ports_path, name);
    insertNode(nodes,
               objectSpec(port_path, ports_path, name, port->getDescription()));
    insertNode(nodes,
               staticStringSpec(appendNodeSegment(port_path, "type"), port_path,
                                "type", "Canonical RTT port type.",
                                port->getTypeInfo()->getTypeName()));

    std::string direction = "unknown";
    if (is_input) {
      direction = "input";
    } else if (is_output) {
      direction = "output";
    }
    insertNode(nodes,
               staticStringSpec(appendNodeSegment(port_path, "direction"),
                                port_path, "direction", "RTT port direction.",
                                std::move(direction)));
    insertNode(nodes, staticStringSpec(
                          appendNodeSegment(port_path, "description"),
                          port_path, "description", "RTT port description.",
                          port->getDescription()));

    if (is_input) {
      insertNode(nodes, portMethodSpec(port_path, *port, state, type_registry,
                                       buffer_size, PortMethodKind::write));
    } else if (is_output) {
      auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(port);
      if (output != nullptr && output->keepsLastWrittenValue()) {
        insertNode(nodes,
                   portValueSpec(port_path, *output, state, type_registry));
      }
      insertNode(nodes, portMethodSpec(port_path, *port, state, type_registry,
                                       buffer_size, PortMethodKind::read));
    }
  }
}

void appendServiceContents(
    NodeMap &nodes, std::vector<UnsupportedResource> &unsupported,
    const RTT::Service::shared_ptr &service, const std::string &service_path,
    const std::string &diagnostic_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<OperationDispatcher> &dispatcher,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry,
    std::size_t port_buffer_size, std::set<const RTT::Service *> &ancestry,
    std::size_t depth) {
  if (!service || depth > kMaximumServiceDepth ||
      ancestry.contains(service.get())) {
    return;
  }
  ancestry.insert(service.get());

  appendResourceFolders(nodes, service_path, service->doc());
  appendConfigurationNodes(nodes, unsupported, service, service_path,
                           diagnostic_path, state, type_registry);
  appendOperationNodes(nodes, unsupported, service, service_path,
                       diagnostic_path, state, dispatcher);
  appendPortNodes(nodes, unsupported, service, service_path, diagnostic_path,
                  state, type_registry, port_buffer_size);

  const std::string services_path = appendNodeSegment(service_path, "services");
  for (const std::string &name : service->getProviderNames()) {
    if (name == "this" || service->getPort(name) != nullptr) {
      continue;
    }
    RTT::Service::shared_ptr child = service->getService(name);
    if (!child) {
      continue;
    }
    const std::string child_path = appendNodeSegment(services_path, name);
    insertNode(nodes,
               objectSpec(child_path, services_path, name, child->doc()));
    appendServiceContents(nodes, unsupported, child, child_path,
                          appendDiagnosticSegment(diagnostic_path, name), state,
                          dispatcher, type_registry, port_buffer_size, ancestry,
                          depth + 1U);
  }

  ancestry.erase(service.get());
}

ComponentSnapshot snapshotComponent(
    const std::shared_ptr<ComponentState> &state, RTT::TaskContext &component,
    const std::shared_ptr<OperationDispatcher> &dispatcher,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry,
    std::size_t port_buffer_size) {
  ComponentSnapshot snapshot;
  const std::string components_path = appendNodeSegment("rtt", "components");
  const std::string component_path =
      appendNodeSegment(components_path, state->component_name);
  insertNode(snapshot.nodes,
             objectSpec(component_path, components_path, state->component_name,
                        component.provides()->doc()));
  insertNode(snapshot.nodes, lifecycleSpec(component_path, state));

  std::set<const RTT::Service *> ancestry;
  appendServiceContents(snapshot.nodes, snapshot.unsupported,
                        component.provides(), component_path, {}, state,
                        dispatcher, type_registry, port_buffer_size, ancestry,
                        0U);
  std::sort(snapshot.unsupported.begin(), snapshot.unsupported.end());
  snapshot.unsupported.erase(
      std::unique(snapshot.unsupported.begin(), snapshot.unsupported.end()),
      snapshot.unsupported.end());
  std::ostringstream fingerprint;
  for (const auto &[path, spec] : snapshot.nodes) {
    fingerprint << path.size() << ':' << path << spec.fingerprint.size() << ':'
                << spec.fingerprint;
  }
  snapshot.fingerprint = fingerprint.str();
  return snapshot;
}

bool collectDescendantNodeIds(::opcua::Server &native,
                              const ::opcua::NodeId &root,
                              std::set<::opcua::NodeId> &descendants,
                              std::string *error) {
  std::vector<::opcua::NodeId> pending{root};
  while (!pending.empty()) {
    const ::opcua::NodeId current = std::move(pending.back());
    pending.pop_back();
    const ::opcua::BrowseDescription browse(
        current, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HierarchicalReferences, true,
        ::opcua::NodeClass::Unspecified, ::opcua::BrowseResultMask::All);
    const auto references = ::opcua::services::browseAll(native, browse);
    if (!references) {
      appendError(error, "failed to inspect an OPC UA rollback subtree: " +
                             statusName(references.code()));
      return false;
    }
    for (const auto &reference : references.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }
      ::opcua::NodeId child = reference.nodeId().nodeId();
      if (descendants.insert(child).second) {
        pending.push_back(std::move(child));
      }
    }
  }
  descendants.erase(root);
  return true;
}

struct ForeignHierarchicalReference {
  ::opcua::NodeId source;
  ::opcua::NodeId target;
  ::opcua::NodeId type;
};

bool detachForeignHierarchicalReferences(
    ::opcua::Server &native, const std::set<::opcua::NodeId> &created_ids,
    std::string *error) {
  std::vector<ForeignHierarchicalReference> foreign_references;
  for (const ::opcua::NodeId &source : created_ids) {
    const ::opcua::BrowseDescription browse(
        source, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HierarchicalReferences, true,
        ::opcua::NodeClass::Unspecified, ::opcua::BrowseResultMask::All);
    const auto references = ::opcua::services::browseAll(native, browse);
    if (!references) {
      appendError(error, "failed to inspect OPC UA rollback references: " +
                             statusName(references.code()));
      return false;
    }
    for (const auto &reference : references.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }
      const ::opcua::NodeId target = reference.nodeId().nodeId();
      if (created_ids.contains(target)) {
        continue;
      }
      foreign_references.push_back(ForeignHierarchicalReference{
          source, target, reference.referenceTypeId()});
    }
  }

  for (const ForeignHierarchicalReference &reference : foreign_references) {
    const ::opcua::StatusCode result = ::opcua::services::deleteReference(
        native, reference.source, reference.target, reference.type, true, true);
    if (result.isBad()) {
      appendError(error,
                  "failed to detach a foreign OPC UA rollback reference: " +
                      statusName(result));
      return false;
    }
  }
  return true;
}

bool rollbackCreatedNodes(::opcua::Server &native,
                          const std::vector<CreatedNode> &ledger,
                          std::string *error) {
  bool complete = true;
  std::set<::opcua::NodeId> created_ids;
  for (const CreatedNode &created : ledger) {
    created_ids.insert(created.id);
  }
  if (!detachForeignHierarchicalReferences(native, created_ids, error)) {
    return false;
  }
  std::set<::opcua::NodeId> recursively_removed;
  for (auto created = ledger.rbegin(); created != ledger.rend(); ++created) {
    std::set<::opcua::NodeId> descendants;
    if (created->recursive_root &&
        !collectDescendantNodeIds(native, created->id, descendants, error)) {
      complete = false;
    }

    for (const auto &descendant : descendants) {
      if (!created_ids.contains(descendant)) {
        continue;
      }
      const ::opcua::StatusCode descendant_result =
          ::opcua::services::deleteNode(native, descendant, true);
      if (descendant_result.isBad() &&
          descendant_result != UA_STATUSCODE_BADNODEIDUNKNOWN) {
        appendError(error,
                    "failed to delete an OPC UA rollback descendant: " +
                        statusName(descendant_result));
        complete = false;
        continue;
      }
      recursively_removed.insert(descendant);
    }

    const ::opcua::StatusCode result =
        ::opcua::services::deleteNode(native, created->id, true);
    if (result == UA_STATUSCODE_BADNODEIDUNKNOWN) {
      if (!recursively_removed.contains(created->id)) {
        appendError(error,
                    "an OPC UA rollback node disappeared before deletion");
        complete = false;
      }
      continue;
    }
    if (result.isBad()) {
      appendError(error, "failed to delete an OPC UA rollback node: " +
                             statusName(result));
      complete = false;
      continue;
    }
  }
  return complete;
}

} // namespace

struct PublishedComponent {
  PublishedComponent(RTT::TaskContext &original,
                     std::shared_ptr<ComponentState> callback_state,
                     NodeMap snapshot_nodes, std::string fingerprint)
      : component(&original), state(std::move(callback_state)),
        nodes(std::move(snapshot_nodes)),
        snapshot_fingerprint(std::move(fingerprint)) {}

  PublishedComponent(PublishedComponent &&) noexcept = default;
  PublishedComponent &operator=(PublishedComponent &&) = delete;
  PublishedComponent(const PublishedComponent &) = delete;
  PublishedComponent &operator=(const PublishedComponent &) = delete;

  RTT::TaskContext *const component;
  std::shared_ptr<ComponentState> state;
  NodeMap nodes;
  std::string snapshot_fingerprint;
};

static_assert(std::is_nothrow_move_constructible_v<PublishedComponent>);

struct AbandonedResources {
  std::shared_ptr<ComponentState> closed_state;
  NodeMap nodes;
  std::string snapshot_fingerprint;
};

static_assert(std::is_nothrow_move_constructible_v<AbandonedResources>);

class ObjectModelImpl final {
public:
  ObjectModelImpl(Server &model_server, ObjectModelOptions model_options)
      : server(model_server), options(std::move(model_options)),
        type_registry(model_server.typeRegistry()),
        dispatcher(std::make_shared<OperationDispatcher>(
            type_registry, options.operation_timeout)) {}

  ~ObjectModelImpl() { shutdown(); }

  bool publishComponent(
      RTT::TaskContext &component, std::string *error,
      std::vector<UnsupportedResource> *unsupported_output) {
    std::unique_lock<std::mutex> lock(command_mutex);
    if (unsupported_output != nullptr) {
      unsupported_output->clear();
    }

    const std::string component_name = component.getName();
    const auto existing = components.find(component_name);
    if (existing != components.end()) {
      if (existing->second.component == &component) {
        failed_publications.erase(component_name);
        setSuccess(error);
        return true;
      }
      return setFailure(
          "RTT component name '" + component_name +
              "' is already published by a different RTT component instance",
          error);
    }

    if (shutdown_started.load()) {
      return setFailure("OPC UA object model is shutting down", error);
    }
    if (!server.isRunning()) {
      return setFailure(
          "OPC UA server must be running before components are published",
          error);
    }
    if (!type_registry) {
      return setFailure("OPC UA server type registry is unavailable", error);
    }
    if (options.operation_timeout <= std::chrono::milliseconds::zero()) {
      return setFailure("object model operation timeout must be positive",
                        error);
    }
    if (options.port_buffer_size == 0U ||
        options.port_buffer_size >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return setFailure("object model port buffer size is out of range", error);
    }

    auto state = std::make_shared<ComponentState>(component);
    ComponentSnapshot snapshot =
        snapshotComponent(state, component, dispatcher, type_registry,
                          options.port_buffer_size);
    if (!snapshot.unsupported.empty()) {
      failed_publications.insert_or_assign(component_name,
                                           snapshot.unsupported);
      if (unsupported_output != nullptr) {
        *unsupported_output = snapshot.unsupported;
      }
      deactivate(state);
      const std::string failure =
          "strict OPC UA publication rejected component '" + component_name +
          "'";
      last_error = failure;
      assignError(error, failure);
      const auto diagnostics = snapshot.unsupported;
      lock.unlock();
      emitDiagnostics(diagnostics);
      return false;
    }

    PublishedComponent candidate(component, state, std::move(snapshot.nodes),
                                 std::move(snapshot.fingerprint));
    std::vector<CreatedNode> ledger;
    std::string transaction_error;
    std::string server_error;
    bool committed = false;
    bool rollback_complete = true;
    const bool invoked = server.invoke(
        [this, &candidate, &ledger, &transaction_error, &committed,
         &rollback_complete](::opcua::Server &native) {
          try {
            const auto namespace_index = server.namespaceIndex();
            if (!namespace_index ||
                !ensureRoots(native, *namespace_index, &transaction_error) ||
                !createCandidate(native, *namespace_index, candidate, ledger,
                                 &transaction_error)) {
              closeAndRollback(native, candidate.state, ledger,
                               rollback_complete, &transaction_error);
              return;
            }

            const std::string name = candidate.state->component_name;
            const auto [published, inserted] =
                components.try_emplace(name, std::move(candidate));
            static_cast<void>(published);
            if (!inserted) {
              assignError(&transaction_error,
                          "RTT component publication changed during commit");
              closeAndRollback(native, candidate.state, ledger,
                               rollback_complete, &transaction_error);
              return;
            }
            committed = true;
            advanceRevision(native);
          } catch (const std::exception &exception) {
            if (committed) {
              return;
            }
            if (transaction_error.empty()) {
              transaction_error =
                  "OPC UA component publication threw: " +
                  std::string(exception.what());
            }
            closeAndRollback(native, candidate.state, ledger,
                             rollback_complete, &transaction_error);
          } catch (...) {
            if (committed) {
              return;
            }
            if (transaction_error.empty()) {
              transaction_error = "OPC UA component publication threw";
            }
            closeAndRollback(native, candidate.state, ledger,
                             rollback_complete, &transaction_error);
          }
        },
        std::chrono::seconds(5), &server_error);

    if (!invoked || !committed) {
      deactivate(state);
      if (!rollback_complete) {
        abandoned.push_back(
            AbandonedResources{state, std::move(candidate.nodes),
                               std::move(candidate.snapshot_fingerprint)});
      }
      std::string failure =
          server_error.empty() ? std::move(transaction_error)
                               : std::move(server_error);
      if (failure.empty()) {
        failure = "failed to publish the OPC UA component";
      }
      return setFailure(std::move(failure), error);
    }

    failed_publications.erase(component_name);
    setSuccess(error);
    return true;
  }

  std::uint64_t currentRevision() const noexcept { return revision.load(); }

  std::size_t componentCount() const noexcept {
    std::lock_guard<std::mutex> lock(command_mutex);
    return components.size();
  }

  std::size_t pendingOperationCount() const noexcept {
    return dispatcher->pendingCount();
  }

  std::vector<UnsupportedResource>
  unsupportedResources(std::string_view component_name) const {
    std::lock_guard<std::mutex> lock(command_mutex);
    const auto found = failed_publications.find(component_name);
    return found == failed_publications.end()
               ? std::vector<UnsupportedResource>{}
               : found->second;
  }

  std::string lastError() const {
    std::lock_guard<std::mutex> lock(command_mutex);
    return last_error;
  }

  void shutdown() noexcept {
    std::unique_lock<std::mutex> lock(command_mutex);
    bool expected = false;
    if (!shutdown_started.compare_exchange_strong(expected, true)) {
      return;
    }

    dispatcher->drainPending();
    for (auto &[name, publication] : components) {
      static_cast<void>(name);
      deactivate(publication.state);
    }
    for (auto &resources : abandoned) {
      deactivate(resources.closed_state);
    }
    components.clear();
    abandoned.clear();
    failed_publications.clear();
  }

private:
  bool setFailure(std::string failure, std::string *error) {
    last_error = failure;
    assignError(error, std::move(failure));
    return false;
  }

  void setSuccess(std::string *error) {
    last_error.clear();
    assignError(error, {});
  }

  void emitDiagnostics(
      const std::vector<UnsupportedResource> &diagnostics) const noexcept {
    for (const UnsupportedResource &resource : diagnostics) {
      const std::string message = resource.message();
      if (options.warning_sink) {
        try {
          options.warning_sink(message);
        } catch (...) {
          RTT::Logger::log().logf(
              RTT::Logger::Error, "ObjectModel",
              "OPC UA diagnostic callback threw an exception");
        }
        continue;
      }
      RTT::Logger::log().logf(RTT::Logger::Warning, "ObjectModel", "%s",
                              message.c_str());
    }
  }

  bool createCandidate(::opcua::Server &native,
                       std::uint16_t namespace_index,
                       const PublishedComponent &candidate,
                       std::vector<CreatedNode> &ledger,
                       std::string *error) {
    std::vector<const NodeSpec *> specs;
    specs.reserve(candidate.nodes.size());
    for (const auto &[path, spec] : candidate.nodes) {
      static_cast<void>(path);
      specs.push_back(&spec);
    }
    std::sort(specs.begin(), specs.end(),
              [](const NodeSpec *left, const NodeSpec *right) {
                const std::size_t left_depth = pathDepth(left->path);
                const std::size_t right_depth = pathDepth(right->path);
                return left_depth == right_depth ? left->path < right->path
                                                 : left_depth < right_depth;
              });

    const std::string component_root = appendNodeSegment(
        appendNodeSegment("rtt", "components"),
        candidate.state->component_name);
    if (candidate.nodes.size() > ledger.max_size() / 3U) {
      assignError(error, "OPC UA component snapshot is too large to publish");
      return false;
    }
    ledger.reserve(candidate.nodes.size() * 3U);
    for (const NodeSpec *spec : specs) {
      CreatedNode primary{nodeId(namespace_index, spec->path),
                          spec->path == component_root ||
                              spec->kind == NodeKind::method};
      bool created = false;
      const bool ready =
          createNode(native, namespace_index, *spec, &created, error);
      if (created) {
        ledger.push_back(std::move(primary));
      }
      if (created && spec->kind == NodeKind::method &&
          !recordMethodArgumentNodes(native, namespace_index, *spec, ledger,
                                     error)) {
        return false;
      }
      if (!ready) {
        return false;
      }
      if (!created) {
        assignError(error, "OPC UA node creator did not report ownership for '" +
                               spec->path + "'");
        return false;
      }
    }
    return true;
  }

  static void closeAndRollback(::opcua::Server &native,
                               const std::shared_ptr<ComponentState> &state,
                               const std::vector<CreatedNode> &ledger,
                               bool &rollback_complete,
                               std::string *error) noexcept {
    deactivate(state);
    try {
      std::string rollback_error;
      rollback_complete =
          rollbackCreatedNodes(native, ledger, &rollback_error);
      if (!rollback_complete) {
        appendError(error, "rollback failed: " + rollback_error);
      }
    } catch (const std::exception &exception) {
      rollback_complete = false;
      try {
        appendError(error, "rollback threw: " + std::string(exception.what()));
      } catch (...) {
      }
    } catch (...) {
      rollback_complete = false;
      try {
        appendError(error, "rollback threw");
      } catch (...) {
      }
    }
  }

  bool ensureRoots(::opcua::Server &native, std::uint16_t namespace_index,
                   std::string *error) {
    if (roots_ready) {
      return true;
    }

    ::opcua::ObjectAttributes root_attributes;
    root_attributes.setDisplayName(::opcua::LocalizedText("en-US", "RTT"));
    root_attributes.setDescription(
        ::opcua::LocalizedText("en-US", "Orocos RTT remote object model."));
    const auto root_result = ::opcua::services::addObject(
        native, ::opcua::ObjectId::ObjectsFolder,
        nodeId(namespace_index, "rtt"), "RTT", root_attributes,
        ::opcua::ObjectTypeId::BaseObjectType,
        ::opcua::ReferenceTypeId::Organizes);
    if (!sharedRootEnsured(root_result, "rtt", error)) {
      return false;
    }

    const auto ensure_object = [&](const std::string &path,
                                   const std::string &browse_name,
                                   const std::string &description) {
      ::opcua::ObjectAttributes attributes;
      attributes.setDisplayName(
          ::opcua::LocalizedText("en-US", browse_name));
      attributes.setDescription(
          ::opcua::LocalizedText("en-US", description));
      const auto result = ::opcua::services::addObject(
          native, nodeId(namespace_index, "rtt"),
          nodeId(namespace_index, path), browse_name, attributes,
          ::opcua::ObjectTypeId::BaseObjectType,
          ::opcua::ReferenceTypeId::HasComponent);
      return sharedRootEnsured(result, path, error);
    };
    const std::string components_path = appendNodeSegment("rtt", "components");
    const std::string model_path = appendNodeSegment("rtt", "model");
    if (!ensure_object(components_path, "Components",
                       "Published RTT components.") ||
        !ensure_object(model_path, "Model", "RTT model metadata.")) {
      return false;
    }

    ::opcua::VariableAttributes revision_attributes;
    revision_attributes.setDisplayName(
        ::opcua::LocalizedText("en-US", "revision"));
    revision_attributes.setDescription(::opcua::LocalizedText(
        "en-US", "Monotonic RTT model schema revision."));
    revision_attributes.setValue(::opcua::Variant(revision.load()));
    revision_attributes.setDataType(::opcua::DataTypeId::UInt64);
    revision_attributes.setValueRank(::opcua::ValueRank::Scalar);
    revision_attributes.setAccessLevel(readOnlyAccess());
    revision_attributes.setUserAccessLevel(readOnlyAccess());
    const std::string revision_path =
        appendNodeSegment(model_path, "revision");
    const auto revision_result = ::opcua::services::addVariable(
        native, nodeId(namespace_index, model_path),
        nodeId(namespace_index, revision_path), "revision", revision_attributes,
        ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!sharedRootEnsured(revision_result, revision_path, error)) {
      return false;
    }
    roots_ready = true;
    return true;
  }

  void advanceRevision(::opcua::Server &native) noexcept {
    try {
      const std::uint64_t next = revision.fetch_add(1U) + 1U;
      const auto namespace_index = server.namespaceIndex();
      if (!namespace_index) {
        return;
      }
      static_cast<void>(::opcua::services::writeValue(
          native,
          nodeId(*namespace_index,
                 appendNodeSegment(appendNodeSegment("rtt", "model"),
                                   "revision")),
          ::opcua::Variant(next)));
    } catch (...) {
      // The publication is already committed; the local revision remains
      // authoritative if updating its OPC UA mirror cannot allocate.
    }
  }

  Server &server;
  const ObjectModelOptions options;
  const std::shared_ptr<const EndpointTypeRegistry> type_registry;
  const std::shared_ptr<OperationDispatcher> dispatcher;
  mutable std::mutex command_mutex;
  std::map<std::string, PublishedComponent, std::less<>> components;
  std::vector<AbandonedResources> abandoned;
  std::map<std::string, std::vector<UnsupportedResource>, std::less<>>
      failed_publications;
  bool roots_ready{false};
  std::atomic<std::uint64_t> revision{0U};
  std::string last_error;
  std::atomic_bool shutdown_started{false};
};

} // namespace detail

std::string UnsupportedResource::message() const {
  return "OPC UA: component '" + component + "' rejected " + kind + " '" +
         path + "' because RTT type '" + type_name + "' " + reason + ".";
}

ObjectModel::ObjectModel(Server &server, ObjectModelOptions options)
    : impl_(std::make_shared<detail::ObjectModelImpl>(server,
                                                      std::move(options))) {}

ObjectModel::~ObjectModel() { impl_->shutdown(); }

bool ObjectModel::publishComponent(
    RTT::TaskContext &component, std::string *error,
    std::vector<UnsupportedResource> *unsupported) {
  return impl_->publishComponent(component, error, unsupported);
}

std::uint64_t ObjectModel::revision() const noexcept {
  return impl_->currentRevision();
}

std::size_t ObjectModel::componentCount() const noexcept {
  return impl_->componentCount();
}

std::size_t ObjectModel::pendingOperationCount() const noexcept {
  return impl_->pendingOperationCount();
}

std::vector<UnsupportedResource>
ObjectModel::unsupportedResources(std::string_view component) const {
  return impl_->unsupportedResources(component);
}

std::string ObjectModel::lastError() const { return impl_->lastError(); }

} // namespace RTT::opcua
