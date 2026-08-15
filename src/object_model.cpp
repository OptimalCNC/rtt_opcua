#include <rtt/opcua/object_model.hpp>

#include "component_state.hpp"
#include "operation_dispatcher.hpp"
#include "port_bridge.hpp"
#include "publication_selector.hpp"

#include <rtt/opcua/endpoint_type_registry.hpp>
#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/port_direction.hpp>

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
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
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

constexpr std::array<std::pair<std::string_view, std::string_view>, 5U>
    kResourceCategories{{
        {"operations", "Operations"},
        {"properties", "Properties"},
        {"attributes", "Attributes"},
        {"ports", "Ports"},
        {"services", "Services"},
    }};

constexpr std::array<std::pair<std::string_view, std::string_view>, 8U>
    kMandatoryOperations{{
        {"getTaskState", "TaskState"},
        {"getTargetState", "TaskState"},
        {"isConfigured", "Bool"},
        {"isActive", "Bool"},
        {"isRunning", "Bool"},
        {"inFatalError", "Bool"},
        {"inException", "Bool"},
        {"inRunTimeError", "Bool"},
}};

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

class InputPortValueDataSource final : public ::opcua::DataSourceBase {
public:
  InputPortValueDataSource(std::weak_ptr<ComponentState> state,
                           std::shared_ptr<PortBridge> bridge)
      : state_(std::move(state)), bridge_(std::move(bridge)) {}

  ::opcua::StatusCode read(::opcua::Session &, const ::opcua::NodeId &,
                           const ::opcua::NumericRange *range,
                           ::opcua::DataValue &value, bool) override {
    if (range != nullptr) {
      value.setStatus(UA_STATUSCODE_BADINDEXRANGEINVALID);
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    ComponentLease lease(state_.lock());
    if (!lease || !bridge_) {
      value.setStatus(UA_STATUSCODE_BADNOTCONNECTED);
      return UA_STATUSCODE_BADNOTCONNECTED;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!has_last_value_) {
      value.setStatus(UA_STATUSCODE_BADWAITINGFORINITIALDATA);
      return UA_STATUSCODE_BADWAITINGFORINITIALDATA;
    }
    value.setValue(last_value_);
    return UA_STATUSCODE_GOOD;
  }

  ::opcua::StatusCode write(::opcua::Session &, const ::opcua::NodeId &,
                            const ::opcua::NumericRange *range,
                            const ::opcua::DataValue &value) override {
    if (range != nullptr) {
      return UA_STATUSCODE_BADINDEXRANGEINVALID;
    }
    if (!value.hasValue()) {
      return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    ComponentLease lease(state_.lock());
    if (!lease || !bridge_) {
      return UA_STATUSCODE_BADNOTCONNECTED;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    const ::opcua::StatusCode status = bridge_->write(value.value());
    if (status.isGood()) {
      last_value_ = value.value();
      has_last_value_ = true;
    }
    return status;
  }

private:
  std::weak_ptr<ComponentState> state_;
  std::shared_ptr<PortBridge> bridge_;
  std::mutex mutex_;
  ::opcua::Variant last_value_;
  bool has_last_value_{false};
};

class OutputPortValueDataSource final : public ::opcua::DataSourceBase {
public:
  OutputPortValueDataSource(
      std::weak_ptr<ComponentState> state, RTT::base::OutputPortInterface *port,
      std::shared_ptr<const EndpointTypeRegistry> type_registry,
      const TypeCodec *codec, std::shared_ptr<PortBridge> observer)
      : state_(std::move(state)), port_(port),
        type_registry_(std::move(type_registry)), codec_(codec),
        observer_(std::move(observer)) {}

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
  std::shared_ptr<PortBridge> observer_;
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
  std::set<std::string, std::less<>> effective_resources;
  std::vector<UnsupportedResource> unsupported;
  std::vector<PublicationDiagnostic> diagnostics;
  std::string component;
  std::string fingerprint;
};

enum class ResourceKind { operation, property, attribute, port, service };

struct ResourceRecord {
  ResourceKind kind;
  std::string path;
  std::string owner_path;
  std::string legacy_path;
  std::string name;
  RTT::Service::shared_ptr owner;
  RTT::Service::shared_ptr service;
  std::string discovery_error;
};

struct ComponentInventory {
  std::map<std::string, ResourceRecord, std::less<>> resources;
  std::vector<PublicationDiagnostic> fatal_diagnostics;
};

enum class PublicationMode { full, selected };

struct PublicationPlan {
  PublicationMode mode;
  std::set<std::string, std::less<>> effective_resources;
  std::vector<PublicationDiagnostic> diagnostics;
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

void appendMappingFailure(ComponentSnapshot &snapshot,
                          const ResourceRecord &record,
                          UnsupportedResource unsupported) {
  snapshot.diagnostics.push_back(PublicationDiagnostic{
      PublicationDiagnosticKind::unsupported_resource, unsupported.component,
      {}, record.path,
      unsupported.kind + " uses RTT type '" + unsupported.type_name +
          "' which " + unsupported.reason});
  snapshot.unsupported.push_back(std::move(unsupported));
}

std::string resourceNodePath(std::string_view component_path,
                             std::string_view resource_path) {
  return std::string(component_path) + '/' + std::string(resource_path);
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

NodeSpec portDirectionSpec(const std::string &port_path,
                           PortDirection direction) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = port_path;
  spec.path = appendNodeSegment(port_path, "direction");
  spec.browse_name = "direction";
  const auto value = static_cast<std::int32_t>(direction);
  spec.fingerprint = "port-direction|" + spec.parent_path + "|" +
                     spec.browse_name + "|" + std::to_string(value);
  spec.create = [path = spec.path, parent = spec.parent_path,
                 value](::opcua::Server &server,
                        std::uint16_t namespace_index, bool *created,
                        std::string *error) {
    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(
        ::opcua::LocalizedText("en-US", "direction"));
    attributes.setDescription(
        ::opcua::LocalizedText("en-US", "RTT port direction."));
    attributes.setValue(::opcua::Variant(value));
    attributes.setDataType(::opcua::DataTypeId::Int32);
    attributes.setValueRank(::opcua::ValueRank::Scalar);
    attributes.setAccessLevel(readOnlyAccess());
    attributes.setUserAccessLevel(readOnlyAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent),
        nodeId(namespace_index, path), "direction", attributes,
        ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    return componentNodeCreated(result, path, created, error);
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
inputPortValueSpec(const std::string &port_path,
                   RTT::base::InputPortInterface &port,
                   const std::shared_ptr<ComponentState> &state,
                   std::shared_ptr<const EndpointTypeRegistry> type_registry) {
  NodeSpec spec;
  spec.kind = NodeKind::variable;
  spec.parent_path = port_path;
  spec.path = appendNodeSegment(port_path, "value");
  spec.browse_name = "value";
  spec.fingerprint = "input-port-value|" + pointerFingerprint(&port);
  spec.create = [path = spec.path, parent = spec.parent_path, port = &port,
                 weak_state = std::weak_ptr<ComponentState>(state),
                 type_registry = std::move(type_registry)](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    bool *created, std::string *error) {
    ComponentLease lease(weak_state.lock());
    if (!lease || port == nullptr || port->getTypeInfo() == nullptr) {
      assignError(
          error,
          "RTT input port became unavailable while creating OPC UA value");
      return false;
    }
    const TypeCodec *codec =
        type_registry ? type_registry->codecForTypeInfo(port->getTypeInfo())
                      : nullptr;
    if (codec == nullptr || !codec->hasValue()) {
      assignError(error, "RTT input port type has no OPC UA protocol");
      return false;
    }

    std::string bridge_error;
    const auto bridge = PortBridge::create(*port, type_registry, &bridge_error);
    if (!bridge) {
      assignError(error, std::move(bridge_error));
      return false;
    }

    ::opcua::VariableAttributes attributes;
    attributes.setDisplayName(::opcua::LocalizedText("en-US", "value"));
    attributes.setDescription(::opcua::LocalizedText(
        "en-US", "Write one sample to the RTT input port."));
    attributes.setDataType(codec->dataTypeNodeId());
    attributes.setValueRank(codec->valueRank());
    if (codec->valueRank() == ::opcua::ValueRank::OneDimension) {
      attributes.setArrayDimensions({0U});
    }
    attributes.setAccessLevel(readWriteAccess());
    attributes.setUserAccessLevel(readWriteAccess());
    const auto result = ::opcua::services::addVariable(
        server, nodeId(namespace_index, parent), nodeId(namespace_index, path),
        "value", attributes, ::opcua::VariableTypeId::BaseDataVariableType,
        ::opcua::ReferenceTypeId::HasComponent);
    if (!componentNodeCreated(result, path, created, error)) {
      return false;
    }
    ::opcua::setVariableNodeValueBackend(
        server, nodeId(namespace_index, path),
        std::make_unique<InputPortValueDataSource>(weak_state, bridge));
    return true;
  };
  return spec;
}

NodeSpec
outputPortValueSpec(const std::string &port_path,
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

    std::string observer_error;
    const auto observer = PortBridge::observe(*port, &observer_error);
    if (!observer) {
      assignError(error, std::move(observer_error));
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
        std::make_unique<OutputPortValueDataSource>(
            weak_state, port, type_registry, codec, observer));
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

void insertNode(NodeMap &nodes, NodeSpec spec) {
  nodes.insert_or_assign(spec.path, std::move(spec));
}

std::string resourceCategory(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::operation:
    return "operations";
  case ResourceKind::property:
    return "properties";
  case ResourceKind::attribute:
    return "attributes";
  case ResourceKind::port:
    return "ports";
  case ResourceKind::service:
    return "services";
  }
  return {};
}

std::string resourceKindName(ResourceKind kind) {
  switch (kind) {
  case ResourceKind::operation:
    return "operation";
  case ResourceKind::property:
    return "property";
  case ResourceKind::attribute:
    return "attribute";
  case ResourceKind::port:
    return "port";
  case ResourceKind::service:
    return "service";
  }
  return "resource";
}

std::string resourcePath(std::string_view owner_path, ResourceKind kind,
                         std::string_view name) {
  std::string path(owner_path);
  if (!path.empty()) {
    path += '/';
  }
  path += resourceCategory(kind);
  return appendNodeSegment(std::move(path), name);
}

void addInventoryRecord(ComponentInventory &inventory,
                        const std::string &component,
                        ResourceRecord record) {
  const std::string path = record.path;
  if (inventory.resources.emplace(path, std::move(record)).second) {
    return;
  }
  inventory.fatal_diagnostics.push_back(PublicationDiagnostic{
      PublicationDiagnosticKind::inventory_failure, component, {}, {},
      "duplicate canonical resource path '" + path + "'"});
}

void discoverService(ComponentInventory &inventory,
                     const std::string &component,
                     const RTT::Service::shared_ptr &service,
                     const std::string &owner_path,
                     const std::string &legacy_path,
                     std::set<const RTT::Service *> &ancestry,
                     std::size_t depth) {
  ancestry.insert(service.get());

  for (const std::string &name : service->getOperationNames()) {
    addInventoryRecord(
        inventory, component,
        ResourceRecord{
            ResourceKind::operation,
            resourcePath(owner_path, ResourceKind::operation, name), owner_path,
            appendDiagnosticSegment(legacy_path, name), name, service, {},
            service->getOperation(name) == nullptr
                ? "RTT operation is unavailable during inventory discovery"
                : std::string{}});
  }
  for (const std::string &name : service->properties()->getPropertyNames()) {
    addInventoryRecord(
        inventory, component,
        ResourceRecord{
            ResourceKind::property,
            resourcePath(owner_path, ResourceKind::property, name), owner_path,
            appendDiagnosticSegment(legacy_path, name), name, service, {},
            service->getProperty(name) == nullptr
                ? "RTT property is unavailable during inventory discovery"
                : std::string{}});
  }
  for (const std::string &name : service->getAttributeNames()) {
    addInventoryRecord(
        inventory, component,
        ResourceRecord{
            ResourceKind::attribute,
            resourcePath(owner_path, ResourceKind::attribute, name), owner_path,
            appendDiagnosticSegment(legacy_path, name), name, service, {},
            service->getValue(name) == nullptr
                ? "RTT attribute is unavailable during inventory discovery"
                : std::string{}});
  }
  for (const std::string &name : service->getPortNames()) {
    addInventoryRecord(
        inventory, component,
        ResourceRecord{
            ResourceKind::port,
            resourcePath(owner_path, ResourceKind::port, name), owner_path,
            appendDiagnosticSegment(legacy_path, name), name, service, {},
            service->getPort(name) == nullptr
                ? "RTT port is unavailable during inventory discovery"
                : std::string{}});
  }

  for (const std::string &name : service->getProviderNames()) {
    if (name == "this") {
      continue;
    }
    RTT::Service::shared_ptr child = service->getService(name);
    const std::string child_path =
        resourcePath(owner_path, ResourceKind::service, name);
    const std::string child_legacy_path =
        appendDiagnosticSegment(legacy_path, name);
    std::string discovery_error;
    if (!child) {
      discovery_error =
          "RTT service is unavailable during inventory discovery";
    } else if (ancestry.contains(child.get())) {
      discovery_error = "forms a cycle in the RTT service graph";
    } else if (depth >= kMaximumServiceDepth) {
      discovery_error = "exceeds the maximum supported service depth of " +
                        std::to_string(kMaximumServiceDepth);
    }
    addInventoryRecord(
        inventory, component,
        ResourceRecord{ResourceKind::service, child_path, owner_path,
                       child_legacy_path, name, service, child,
                       discovery_error});
    if (child && discovery_error.empty()) {
      discoverService(inventory, component, child, child_path,
                      child_legacy_path, ancestry, depth + 1U);
    }
  }

  ancestry.erase(service.get());
}

ComponentInventory discoverComponent(RTT::TaskContext &component) {
  ComponentInventory inventory;
  const RTT::Service::shared_ptr root = component.provides();
  if (!root) {
    inventory.fatal_diagnostics.push_back(PublicationDiagnostic{
        PublicationDiagnosticKind::inventory_failure, component.getName(), {},
        {}, "RTT component root service is unavailable"});
    return inventory;
  }
  std::set<const RTT::Service *> ancestry;
  discoverService(inventory, component.getName(), root, {}, {}, ancestry, 0U);
  std::sort(inventory.fatal_diagnostics.begin(),
            inventory.fatal_diagnostics.end());
  inventory.fatal_diagnostics.erase(
      std::unique(inventory.fatal_diagnostics.begin(),
                  inventory.fatal_diagnostics.end()),
      inventory.fatal_diagnostics.end());
  return inventory;
}

void addServiceAncestors(
    std::string_view owner_path,
    std::set<std::string, std::less<>> &effective_resources) {
  std::size_t begin = 0U;
  while (begin < owner_path.size()) {
    const std::size_t category_end = owner_path.find('/', begin);
    if (category_end == std::string_view::npos ||
        owner_path.substr(begin, category_end - begin) != "services") {
      return;
    }
    const std::size_t name_end = owner_path.find('/', category_end + 1U);
    const std::size_t ancestor_end =
        name_end == std::string_view::npos ? owner_path.size() : name_end;
    effective_resources.emplace(owner_path.substr(0U, ancestor_end));
    if (name_end == std::string_view::npos) {
      return;
    }
    begin = name_end + 1U;
  }
}

PublicationPlan buildPublicationPlan(
    PublicationMode mode, const ComponentInventory &inventory,
    const std::string &component, const std::vector<std::string> &selectors) {
  PublicationPlan plan{mode, {}, inventory.fatal_diagnostics};
  if (mode == PublicationMode::full) {
    for (const auto &[path, record] : inventory.resources) {
      static_cast<void>(record);
      plan.effective_resources.insert(path);
    }
  } else if (selectors.empty()) {
    plan.diagnostics.push_back(PublicationDiagnostic{
        PublicationDiagnosticKind::malformed_selector, component, {}, {},
        "at least one publication selector is required"});
  } else {
    std::vector<std::string> resource_paths;
    resource_paths.reserve(inventory.resources.size());
    for (const auto &[path, record] : inventory.resources) {
      static_cast<void>(record);
      resource_paths.push_back(path);
    }
    const SelectorMatch match =
        matchPublicationSelectors(selectors, resource_paths);
    plan.effective_resources = match.resource_paths;
    for (const SelectorIssue &issue : match.issues) {
      plan.diagnostics.push_back(PublicationDiagnostic{
          issue.kind == SelectorIssueKind::malformed
              ? PublicationDiagnosticKind::malformed_selector
              : PublicationDiagnosticKind::unmatched_selector,
          component, issue.selector, {}, issue.reason});
    }
    for (const std::string &path : match.resource_paths) {
      const auto record = inventory.resources.find(path);
      if (record != inventory.resources.end()) {
        addServiceAncestors(record->second.owner_path,
                            plan.effective_resources);
      }
    }
  }

  for (const auto &[name, output_type] : kMandatoryOperations) {
    static_cast<void>(output_type);
    const std::string path = resourcePath({}, ResourceKind::operation, name);
    if (inventory.resources.contains(path)) {
      plan.effective_resources.insert(path);
      continue;
    }
    plan.diagnostics.push_back(PublicationDiagnostic{
        PublicationDiagnosticKind::mandatory_resource, component, {},
        path, "mandatory RTT proxy operation is unavailable"});
  }
  return plan;
}

std::string_view mandatoryOutputType(const ResourceRecord &record) {
  if (record.kind != ResourceKind::operation || !record.owner_path.empty()) {
    return {};
  }
  const auto found = std::ranges::find_if(
      kMandatoryOperations,
      [&record](const auto &mandatory) { return mandatory.first == record.name; });
  return found == kMandatoryOperations.end() ? std::string_view{}
                                             : found->second;
}

void appendMandatorySchemaFailure(ComponentSnapshot &snapshot,
                                  const ResourceRecord &record,
                                  std::string reason) {
  snapshot.diagnostics.push_back(PublicationDiagnostic{
      PublicationDiagnosticKind::mandatory_resource, snapshot.component, {},
      record.path, std::move(reason)});
}

void appendDiscoveryFailure(ComponentSnapshot &snapshot,
                            const ResourceRecord &record,
                            const std::string &component,
                            std::string reason) {
  appendMappingFailure(
      snapshot, record,
      UnsupportedResource{component, record.legacy_path,
                          resourceKindName(record.kind),
                          record.kind == ResourceKind::service ? "RTT::Service"
                                                               : "<unknown>",
                          std::move(reason)});
}

void appendCategoryNode(ComponentSnapshot &snapshot,
                        const ResourceRecord &record,
                        const std::string &component_path) {
  const std::string owner_node_path =
      record.owner_path.empty()
          ? component_path
          : resourceNodePath(component_path, record.owner_path);
  const std::string category = resourceCategory(record.kind);
  const auto found = std::ranges::find_if(
      kResourceCategories,
      [&category](const auto &entry) { return entry.first == category; });
  insertNode(snapshot.nodes,
             objectSpec(appendNodeSegment(owner_node_path, category),
                        owner_node_path,
                        found == kResourceCategories.end()
                            ? category
                            : std::string(found->second),
                        record.owner ? record.owner->doc() : std::string{}));
}

void appendOperationBundle(
    ComponentSnapshot &snapshot, const ResourceRecord &record,
    const std::string &component_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<OperationDispatcher> &dispatcher) {
  const std::string_view mandatory_type = mandatoryOutputType(record);
  if (!record.discovery_error.empty()) {
    if (!mandatory_type.empty()) {
      appendMandatorySchemaFailure(
          snapshot, record, "mandatory RTT proxy operation is unavailable");
      return;
    }
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           record.discovery_error);
    return;
  }
  RTT::OperationInterfacePart *operation =
      record.owner ? record.owner->getOperation(record.name) : nullptr;
  if (operation == nullptr) {
    if (!mandatory_type.empty()) {
      appendMandatorySchemaFailure(
          snapshot, record, "mandatory RTT proxy operation is unavailable");
      return;
    }
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           "RTT operation became unavailable during mapping");
    return;
  }
  OperationSchema schema = dispatcher->describe(*operation);
  if (!schema.supported) {
    if (!mandatory_type.empty()) {
      appendMandatorySchemaFailure(
          snapshot, record,
          "mandatory RTT proxy operation has an unsupported schema");
      return;
    }
    std::vector<UnsupportedResource> unsupported;
    appendUnsupported(unsupported, state->component_name, record.legacy_path,
                      "operation", std::move(schema.unsupported_type_name),
                      std::move(schema.unsupported_reason));
    appendMappingFailure(snapshot, record, std::move(unsupported.front()));
    return;
  }
  if (!mandatory_type.empty() &&
      (!schema.inputs.empty() || !schema.input_type_names.empty() ||
       schema.outputs.size() != 1U || schema.output_type_names.size() != 1U ||
       schema.output_type_names.front() != mandatory_type ||
       schema.output_sources != std::vector<std::int32_t>{-1})) {
    appendMandatorySchemaFailure(
        snapshot, record,
        "mandatory RTT proxy operation has an incompatible input/output "
        "schema");
    return;
  }

  appendCategoryNode(snapshot, record, component_path);
  const std::string operations_path = resourceNodePath(
      component_path,
      record.owner_path.empty() ? "operations"
                                : record.owner_path + "/operations");
  const std::string operation_path =
      resourceNodePath(component_path, record.path);
  if (!schema.input_type_names.empty()) {
    insertNode(snapshot.nodes,
               staticArrayPropertySpec(
                   appendNodeSegment(operation_path, "rttInputTypes"),
                   operation_path, "rttInputTypes",
                   "Canonical RTT input argument types.", schema.input_type_names,
                   ::opcua::DataTypeId::String));
  }
  if (!schema.output_type_names.empty()) {
    insertNode(snapshot.nodes,
               staticArrayPropertySpec(
                   appendNodeSegment(operation_path, "rttOutputTypes"),
                   operation_path, "rttOutputTypes",
                   "Canonical RTT output argument types.",
                   schema.output_type_names, ::opcua::DataTypeId::String));
    insertNode(snapshot.nodes,
               staticArrayPropertySpec(
                   appendNodeSegment(operation_path, "rttOutputSources"),
                   operation_path, "rttOutputSources",
                   "Return value (-1) or mutable input index for each output.",
                   schema.output_sources, ::opcua::DataTypeId::Int32));
  }
  insertNode(snapshot.nodes,
             operationSpec(operations_path, record.name,
                           operation->description(), record.owner, *operation,
                           state, dispatcher, std::move(schema)));
}

void appendConfigurationBundle(
    ComponentSnapshot &snapshot, const ResourceRecord &record,
    const std::string &component_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry) {
  if (!record.discovery_error.empty()) {
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           record.discovery_error);
    return;
  }
  RTT::base::PropertyBase *property = nullptr;
  RTT::base::AttributeBase *attribute = nullptr;
  RTT::base::DataSourceBase::shared_ptr source;
  std::string description;
  if (record.kind == ResourceKind::property) {
    property = record.owner ? record.owner->getProperty(record.name) : nullptr;
    if (property != nullptr) {
      source = property->getDataSource();
      description = property->getDescription();
    }
  } else {
    attribute = record.owner ? record.owner->getValue(record.name) : nullptr;
    if (attribute != nullptr) {
      source = attribute->getDataSource();
    }
  }
  if ((record.kind == ResourceKind::property && property == nullptr) ||
      (record.kind == ResourceKind::attribute && attribute == nullptr)) {
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           "RTT " + resourceKindName(record.kind) +
                               " became unavailable during mapping");
    return;
  }
  const RTT::types::TypeInfo *type =
      source == nullptr ? nullptr : source->getTypeInfo();
  const TypeCodec *codec =
      source == nullptr ? nullptr : type_registry->codecForDataSource(source);
  if (codec == nullptr || !codec->hasValue()) {
    std::vector<UnsupportedResource> unsupported;
    appendUnsupported(unsupported, state->component_name, record.legacy_path,
                      resourceKindName(record.kind), type, codec);
    appendMappingFailure(snapshot, record, std::move(unsupported.front()));
    return;
  }

  appendCategoryNode(snapshot, record, component_path);
  const std::string category_path = resourceNodePath(
      component_path,
      record.owner_path.empty()
          ? resourceCategory(record.kind)
          : record.owner_path + "/" + resourceCategory(record.kind));
  insertNode(snapshot.nodes, dataSourceSpec(category_path, record.name,
                                            description, source, state,
                                            type_registry));
  const std::string value_path = resourceNodePath(component_path, record.path);
  insertNode(snapshot.nodes,
             staticStringSpec(appendNodeSegment(value_path, "rttType"),
                              value_path, "rttType",
                              "Canonical RTT value type.",
                              source->getTypeInfo()->getTypeName(), true));
}

void appendPortBundle(
    ComponentSnapshot &snapshot, const ResourceRecord &record,
    const std::string &component_path,
    const std::shared_ptr<ComponentState> &state,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry) {
  if (!record.discovery_error.empty()) {
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           record.discovery_error);
    return;
  }
  RTT::base::PortInterface *port =
      record.owner ? record.owner->getPort(record.name) : nullptr;
  if (port == nullptr) {
    appendDiscoveryFailure(snapshot, record, state->component_name,
                           "RTT port became unavailable during mapping");
    return;
  }
  const RTT::types::TypeInfo *type = port->getTypeInfo();
  const TypeCodec *codec =
      type == nullptr ? nullptr : type_registry->codecForTypeInfo(type);
  auto *input = dynamic_cast<RTT::base::InputPortInterface *>(port);
  auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(port);
  const bool is_input = input != nullptr;
  const bool is_output = output != nullptr;
  if (is_input == is_output) {
    appendMappingFailure(
        snapshot, record,
        UnsupportedResource{
            state->component_name, record.legacy_path, "port", typeName(type),
            is_input ? "matches both RTT input and output interfaces"
                     : "matches neither RTT input nor output interface"});
    return;
  }
  if (codec == nullptr || !codec->hasValue()) {
    std::vector<UnsupportedResource> unsupported;
    appendUnsupported(unsupported, state->component_name, record.legacy_path,
                      is_input ? "input port" : "output port", type, codec);
    appendMappingFailure(snapshot, record, std::move(unsupported.front()));
    return;
  }

  appendCategoryNode(snapshot, record, component_path);
  const std::string ports_path = resourceNodePath(
      component_path,
      record.owner_path.empty() ? "ports" : record.owner_path + "/ports");
  const std::string port_path = resourceNodePath(component_path, record.path);
  insertNode(snapshot.nodes,
             objectSpec(port_path, ports_path, record.name,
                        port->getDescription()));
  insertNode(snapshot.nodes,
             staticStringSpec(appendNodeSegment(port_path, "type"), port_path,
                              "type", "Canonical RTT port type.",
                              port->getTypeInfo()->getTypeName()));
  insertNode(snapshot.nodes,
             portDirectionSpec(port_path, is_input ? PortDirection::input
                                                   : PortDirection::output));
  insertNode(snapshot.nodes,
             staticStringSpec(appendNodeSegment(port_path, "description"),
                              port_path, "description", "RTT port description.",
                              port->getDescription()));
  if (input != nullptr) {
    insertNode(snapshot.nodes,
               inputPortValueSpec(port_path, *input, state, type_registry));
  } else if (output->keepsLastWrittenValue()) {
    insertNode(snapshot.nodes,
               outputPortValueSpec(port_path, *output, state, type_registry));
  }
}

void appendServiceBundle(ComponentSnapshot &snapshot,
                         const ResourceRecord &record,
                         const std::string &component_path) {
  if (!record.discovery_error.empty() || !record.owner || !record.service) {
    appendDiscoveryFailure(
        snapshot, record, snapshot.component,
        !record.discovery_error.empty()
            ? record.discovery_error
            : "RTT service became unavailable during mapping");
    return;
  }
  appendCategoryNode(snapshot, record, component_path);
  const std::string services_path = resourceNodePath(
      component_path,
      record.owner_path.empty() ? "services"
                                : record.owner_path + "/services");
  insertNode(snapshot.nodes,
             objectSpec(resourceNodePath(component_path, record.path),
                        services_path, record.name, record.service->doc()));
}

ComponentSnapshot snapshotComponent(
    const std::shared_ptr<ComponentState> &state, RTT::TaskContext &component,
    const std::shared_ptr<OperationDispatcher> &dispatcher,
    const std::shared_ptr<const EndpointTypeRegistry> &type_registry,
    PublicationMode mode, const std::vector<std::string> &selectors) {
  const ComponentInventory inventory = discoverComponent(component);
  const PublicationPlan plan =
      buildPublicationPlan(mode, inventory, state->component_name, selectors);
  ComponentSnapshot snapshot;
  snapshot.component = state->component_name;
  snapshot.effective_resources = plan.effective_resources;
  snapshot.diagnostics = plan.diagnostics;

  const std::string components_path = appendNodeSegment("rtt", "components");
  const std::string component_path =
      appendNodeSegment(components_path, state->component_name);
  insertNode(snapshot.nodes,
             objectSpec(component_path, components_path, state->component_name,
                        component.provides()->doc()));

  for (const std::string &path : plan.effective_resources) {
    const auto found = inventory.resources.find(path);
    if (found == inventory.resources.end()) {
      continue;
    }
    const ResourceRecord &record = found->second;
    switch (record.kind) {
    case ResourceKind::operation:
      appendOperationBundle(snapshot, record, component_path, state, dispatcher);
      break;
    case ResourceKind::property:
    case ResourceKind::attribute:
      appendConfigurationBundle(snapshot, record, component_path, state,
                                type_registry);
      break;
    case ResourceKind::port:
      appendPortBundle(snapshot, record, component_path, state, type_registry);
      break;
    case ResourceKind::service:
      appendServiceBundle(snapshot, record, component_path);
      break;
    }
  }
  std::sort(snapshot.unsupported.begin(), snapshot.unsupported.end());
  snapshot.unsupported.erase(
      std::unique(snapshot.unsupported.begin(), snapshot.unsupported.end()),
      snapshot.unsupported.end());
  std::sort(snapshot.diagnostics.begin(), snapshot.diagnostics.end());
  snapshot.diagnostics.erase(
      std::unique(snapshot.diagnostics.begin(), snapshot.diagnostics.end()),
      snapshot.diagnostics.end());
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
                     PublicationMode publication_mode,
                     std::set<std::string, std::less<>> resources,
                     std::shared_ptr<ComponentState> callback_state,
                     NodeMap snapshot_nodes, std::string fingerprint)
      : component(&original), mode(publication_mode),
        effective_resources(std::move(resources)),
        state(std::move(callback_state)),
        nodes(std::move(snapshot_nodes)),
        snapshot_fingerprint(std::move(fingerprint)) {}

  PublishedComponent(PublishedComponent &&) noexcept = default;
  PublishedComponent &operator=(PublishedComponent &&) = delete;
  PublishedComponent(const PublishedComponent &) = delete;
  PublishedComponent &operator=(const PublishedComponent &) = delete;

  RTT::TaskContext *const component;
  PublicationMode mode;
  std::set<std::string, std::less<>> effective_resources;
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
    return publishComponentInternal(component, PublicationMode::full, {}, error,
                                    unsupported_output, nullptr);
  }

  bool publishComponentSelected(
      RTT::TaskContext &component, const std::vector<std::string> &selectors,
      std::string *error,
      std::vector<PublicationDiagnostic> *diagnostic_output) {
    return publishComponentInternal(component, PublicationMode::selected,
                                    selectors, error, nullptr,
                                    diagnostic_output);
  }

private:
  bool publishComponentInternal(
      RTT::TaskContext &component, PublicationMode mode,
      const std::vector<std::string> &selectors, std::string *error,
      std::vector<UnsupportedResource> *unsupported_output,
      std::vector<PublicationDiagnostic> *diagnostic_output) {
    std::unique_lock<std::mutex> lock(command_mutex);
    if (unsupported_output != nullptr) {
      unsupported_output->clear();
    }
    if (diagnostic_output != nullptr) {
      diagnostic_output->clear();
    }

    const std::string component_name = component.getName();
    const auto existing = components.find(component_name);
    if (existing != components.end()) {
      if (existing->second.component != &component) {
        return rejectPublicationConflict(
            component_name,
            "already published by a different RTT component instance", error,
            diagnostic_output, lock);
      }
      if (existing->second.mode != mode) {
        return rejectPublicationConflict(
            component_name,
            "publication mode differs from the first successful publication",
            error, diagnostic_output, lock);
      }
      if (mode == PublicationMode::full) {
        failed_publications.erase(component_name);
        publication_diagnostics.erase(component_name);
        setSuccess(error);
        return true;
      }

      const ComponentInventory inventory = discoverComponent(component);
      const PublicationPlan plan = buildPublicationPlan(
          mode, inventory, component_name, selectors);
      if (existing->second.effective_resources != plan.effective_resources) {
        return rejectPublicationConflict(
            component_name,
            "selection resolves to a different effective resource set",
            error, diagnostic_output, lock);
      }
      if (!plan.diagnostics.empty()) {
        failed_publications.erase(component_name);
        publication_diagnostics.insert_or_assign(component_name,
                                                 plan.diagnostics);
        if (diagnostic_output != nullptr) {
          *diagnostic_output = plan.diagnostics;
        }
        const std::string failure =
            "selective OPC UA publication rejected component '" +
            component_name + "' with " +
            std::to_string(plan.diagnostics.size()) + " diagnostic(s)";
        last_error = failure;
        assignError(error, failure);
        const auto diagnostics = plan.diagnostics;
        lock.unlock();
        emitDiagnostics(diagnostics);
        return false;
      }

      failed_publications.erase(component_name);
      publication_diagnostics.erase(component_name);
      setSuccess(error);
      return true;
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
    auto state = std::make_shared<ComponentState>(component);
    ComponentSnapshot snapshot = snapshotComponent(
        state, component, dispatcher, type_registry, mode, selectors);
    if (!snapshot.diagnostics.empty()) {
      failed_publications.insert_or_assign(component_name,
                                           snapshot.unsupported);
      publication_diagnostics.insert_or_assign(component_name,
                                               snapshot.diagnostics);
      if (unsupported_output != nullptr) {
        *unsupported_output = snapshot.unsupported;
      }
      if (diagnostic_output != nullptr) {
        *diagnostic_output = snapshot.diagnostics;
      }
      deactivate(state);
      const std::string failure =
          mode == PublicationMode::full
              ? "strict OPC UA publication rejected component '" +
                    component_name + "'"
              : "selective OPC UA publication rejected component '" +
                    component_name + "' with " +
                    std::to_string(snapshot.diagnostics.size()) +
                    " diagnostic(s)";
      last_error = failure;
      assignError(error, failure);
      const auto diagnostics = snapshot.diagnostics;
      lock.unlock();
      emitDiagnostics(diagnostics);
      return false;
    }

    PublishedComponent candidate(
        component, mode, std::move(snapshot.effective_resources), state,
        std::move(snapshot.nodes), std::move(snapshot.fingerprint));
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
    publication_diagnostics.erase(component_name);
    setSuccess(error);
    return true;
  }

public:
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

  std::vector<PublicationDiagnostic>
  publicationDiagnostics(std::string_view component_name) const {
    std::lock_guard<std::mutex> lock(command_mutex);
    const auto found = publication_diagnostics.find(component_name);
    return found == publication_diagnostics.end()
               ? std::vector<PublicationDiagnostic>{}
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
    publication_diagnostics.clear();
  }

private:
  bool rejectPublicationConflict(
      const std::string &component_name, std::string reason,
      std::string *error,
      std::vector<PublicationDiagnostic> *diagnostic_output,
      std::unique_lock<std::mutex> &lock) {
    std::vector<PublicationDiagnostic> diagnostics{
        PublicationDiagnostic{PublicationDiagnosticKind::publication_conflict,
                              component_name, {}, {}, std::move(reason)}};
    publication_diagnostics.insert_or_assign(component_name, diagnostics);
    if (diagnostic_output != nullptr) {
      *diagnostic_output = diagnostics;
    }
    const std::string failure = "RTT component publication conflict for '" +
                                component_name + "': " +
                                diagnostics.front().reason;
    last_error = failure;
    assignError(error, failure);
    lock.unlock();
    emitDiagnostics(diagnostics);
    return false;
  }

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
      const std::vector<PublicationDiagnostic> &diagnostics) const noexcept {
    for (const PublicationDiagnostic &diagnostic : diagnostics) {
      const std::string message = diagnostic.message();
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
  std::map<std::string, std::vector<PublicationDiagnostic>, std::less<>>
      publication_diagnostics;
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

std::string PublicationDiagnostic::message() const {
  std::string stable_reason = reason;
  while (stable_reason.ends_with('.')) {
    stable_reason.pop_back();
  }
  switch (kind) {
  case PublicationDiagnosticKind::malformed_selector:
    return "OPC UA publication: component '" + component +
           "' rejected selector '" + selector + "': " + stable_reason + ".";
  case PublicationDiagnosticKind::unmatched_selector:
    return "OPC UA publication: component '" + component + "' selector '" +
           selector + "' matched no RTT resource.";
  case PublicationDiagnosticKind::inventory_failure:
    return "OPC UA publication: component '" + component +
           "' inventory failed: " + stable_reason + ".";
  case PublicationDiagnosticKind::unsupported_resource:
    return "OPC UA publication: component '" + component +
           "' rejected resource '" + resource_path + "': " + stable_reason +
           ".";
  case PublicationDiagnosticKind::mandatory_resource:
    return "OPC UA publication: component '" + component +
           "' requires resource '" + resource_path + "': " + stable_reason +
           ".";
  case PublicationDiagnosticKind::publication_conflict:
    return "OPC UA publication: component '" + component +
           "' conflicts with its existing publication: " + stable_reason +
           ".";
  }
  return {};
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

bool ObjectModel::publishComponentSelected(
    RTT::TaskContext &component, const std::vector<std::string> &selectors,
    std::string *error, std::vector<PublicationDiagnostic> *diagnostics) {
  return impl_->publishComponentSelected(component, selectors, error,
                                         diagnostics);
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

std::vector<PublicationDiagnostic>
ObjectModel::publicationDiagnostics(std::string_view component) const {
  return impl_->publicationDiagnostics(component);
}

std::string ObjectModel::lastError() const { return impl_->lastError(); }

} // namespace RTT::opcua
