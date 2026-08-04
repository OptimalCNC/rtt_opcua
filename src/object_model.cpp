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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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

bool isDescendantPath(std::string_view path, std::string_view ancestor) {
  return path.size() > ancestor.size() && path.starts_with(ancestor) &&
         path[ancestor.size()] == '/';
}

void assignError(std::string *output, std::string value) {
  if (output != nullptr) {
    *output = std::move(value);
  }
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
    std::string *error) {
  if (result) {
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

struct OwnedNodeIdentity {
  ::opcua::NodeId id;
  ::opcua::NodeClass node_class;
  void *context{nullptr};
  bool marker_context{false};
  bool primary{false};
};

struct NodeOwnership {
  std::vector<OwnedNodeIdentity> identities;
};

struct NodeSpec {
  NodeKind kind;
  std::string path;
  std::string parent_path;
  std::string browse_name;
  std::string fingerprint;
  std::shared_ptr<NodeOwnership> ownership{std::make_shared<NodeOwnership>()};
  std::function<bool(::opcua::Server &, std::uint16_t, std::string *)> create;
};

using NodeMap = std::map<std::string, NodeSpec>;

::opcua::NodeClass nodeClass(NodeKind kind) {
  switch (kind) {
  case NodeKind::object:
    return ::opcua::NodeClass::Object;
  case NodeKind::variable:
    return ::opcua::NodeClass::Variable;
  case NodeKind::method:
    return ::opcua::NodeClass::Method;
  }
  return ::opcua::NodeClass::Unspecified;
}

bool isMethodArgument(const ::opcua::ReferenceDescription &reference) {
  const auto browse_name = reference.browseName();
  return reference.nodeClass() == ::opcua::NodeClass::Variable &&
         browse_name.namespaceIndex() == 0U &&
         (browse_name.name() == "InputArguments" ||
          browse_name.name() == "OutputArguments");
}

bool captureIdentity(::opcua::Server &native, const ::opcua::NodeId &id,
                     ::opcua::NodeClass expected_class,
                     const std::shared_ptr<NodeOwnership> &ownership,
                     bool primary, std::string *error) {
  const auto actual_class = ::opcua::services::readNodeClass(native, id);
  if (!actual_class || actual_class.value() != expected_class) {
    assignError(error, "failed to capture OPC UA node ownership");
    return false;
  }

  void *context = nullptr;
  const ::opcua::StatusCode context_status(
      UA_Server_getNodeContext(native.handle(), id, &context));
  if (context_status.isBad()) {
    assignError(error, "failed to read OPC UA node ownership context: " +
                           statusName(context_status));
    return false;
  }

  const bool marker_context = context == nullptr;
  if (marker_context) {
    context = ownership.get();
    const ::opcua::StatusCode marker_status(
        UA_Server_setNodeContext(native.handle(), id, context));
    if (marker_status.isBad()) {
      assignError(error, "failed to mark OPC UA node ownership: " +
                             statusName(marker_status));
      return false;
    }
  }
  ownership->identities.push_back(
      OwnedNodeIdentity{id, expected_class, context, marker_context, primary});
  return true;
}

bool captureOwnership(::opcua::Server &native, std::uint16_t namespace_index,
                      const NodeSpec &spec, std::string *error) {
  spec.ownership->identities.clear();
  const ::opcua::NodeId primary_id = nodeId(namespace_index, spec.path);
  if (!captureIdentity(native, primary_id, nodeClass(spec.kind), spec.ownership,
                       true, error)) {
    return false;
  }
  if (spec.kind != NodeKind::method) {
    return true;
  }

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
  for (const auto &reference : references.value()) {
    if (!reference.nodeId().isLocal() || !isMethodArgument(reference)) {
      assignError(error, "unexpected OPC UA method property while capturing '" +
                             spec.path + "'");
      return false;
    }
    if (!captureIdentity(native, reference.nodeId().nodeId(),
                         ::opcua::NodeClass::Variable, spec.ownership, false,
                         error)) {
      return false;
    }
  }
  return true;
}

enum class IdentityState { missing, owned, foreign };

IdentityState identityState(::opcua::Server &native,
                            const OwnedNodeIdentity &identity) {
  const auto actual_class =
      ::opcua::services::readNodeClass(native, identity.id);
  if (!actual_class) {
    return actual_class.code() == UA_STATUSCODE_BADNODEIDUNKNOWN
               ? IdentityState::missing
               : IdentityState::foreign;
  }
  if (actual_class.value() != identity.node_class) {
    return IdentityState::foreign;
  }
  void *context = nullptr;
  if (UA_Server_getNodeContext(native.handle(), identity.id, &context) !=
      UA_STATUSCODE_GOOD) {
    return IdentityState::foreign;
  }
  return context == identity.context ? IdentityState::owned
                                     : IdentityState::foreign;
}

void releaseOwnershipMarkers(::opcua::Server &native,
                             const NodeMap &nodes) noexcept {
  for (const auto &[path, spec] : nodes) {
    static_cast<void>(path);
    for (const OwnedNodeIdentity &identity : spec.ownership->identities) {
      if (!identity.marker_context ||
          identityState(native, identity) != IdentityState::owned) {
        continue;
      }
      static_cast<void>(
          UA_Server_setNodeContext(native.handle(), identity.id, nullptr));
    }
  }
}

bool removalSubtreesAreOwned(::opcua::Server &native,
                             std::uint16_t namespace_index,
                             const NodeMap &previous,
                             const std::vector<std::string> &roots,
                             std::string *error) {
  std::map<::opcua::NodeId, const OwnedNodeIdentity *> owned_nodes;
  for (const auto &[path, spec] : previous) {
    static_cast<void>(path);
    for (const OwnedNodeIdentity &identity : spec.ownership->identities) {
      owned_nodes.emplace(identity.id, &identity);
    }
  }

  struct PendingNode {
    ::opcua::NodeId id;
    std::string root_path;
  };
  std::vector<PendingNode> pending;
  for (const std::string &path : roots) {
    const auto spec = previous.find(path);
    if (spec == previous.end()) {
      continue;
    }
    const ::opcua::NodeId root_id = nodeId(namespace_index, path);
    const auto owned = owned_nodes.find(root_id);
    if (owned == owned_nodes.end()) {
      assignError(error, "missing OPC UA ownership proof for '" + path + "'");
      return false;
    }
    const IdentityState state = identityState(native, *owned->second);
    if (state == IdentityState::missing) {
      continue;
    }
    if (state == IdentityState::foreign) {
      assignError(error, "OPC UA ownership changed for node '" + path + "'");
      return false;
    }
    pending.push_back(PendingNode{root_id, path});
  }

  std::set<::opcua::NodeId> visited;
  while (!pending.empty()) {
    PendingNode current = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(current.id).second) {
      continue;
    }

    const ::opcua::BrowseDescription browse(
        current.id, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HierarchicalReferences, true,
        ::opcua::NodeClass::Unspecified, ::opcua::BrowseResultMask::All);
    const auto references = ::opcua::services::browseAll(native, browse);
    if (!references) {
      assignError(error, "failed to inspect OPC UA replacement subtree '" +
                             current.root_path + "': " +
                             statusName(references.code()));
      return false;
    }

    for (const auto &reference : references.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }
      const ::opcua::NodeId child = reference.nodeId().nodeId();
      const auto owned = owned_nodes.find(child);
      if (owned != owned_nodes.end()) {
        if (identityState(native, *owned->second) != IdentityState::owned) {
          assignError(error, "OPC UA ownership changed below node '" +
                                 current.root_path + "'");
          return false;
        }
        pending.push_back(PendingNode{child, current.root_path});
        continue;
      }

      assignError(error,
                  "refusing to replace OPC UA node '" + current.root_path +
                      "' because it contains unowned child '" +
                      std::string(reference.browseName().name()) + "'");
      return false;
    }
  }
  return true;
}

struct ComponentSnapshot {
  NodeMap nodes;
  std::vector<UnsupportedResource> unsupported;
};

struct DiagnosticEvent {
  bool recovered{false};
  UnsupportedResource resource;
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
                   const std::string &description, std::string *error) {
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
  return componentNodeCreated(result, path, error);
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
                    std::string *error) {
    return addObjectNode(server, namespace_index, parent, path, name,
                         description, error);
  };
  return spec;
}

bool addStaticStringVariable(
    ::opcua::Server &server, std::uint16_t namespace_index,
    const std::string &parent_path, const std::string &path,
    const std::string &browse_name, const std::string &description,
    const std::string &value, bool property, std::string *error) {
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
  return componentNodeCreated(result, path, error);
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
                           std::uint16_t namespace_index, std::string *error) {
    return addStaticStringVariable(server, namespace_index, parent, path, name,
                                   description, value, property, error);
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
                    std::string *error) {
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
    return componentNodeCreated(result, path, error);
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
                    std::string *error) {
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
    if (!componentNodeCreated(result, path, error)) {
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
                    std::string *error) {
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
    if (!componentNodeCreated(result, path, error)) {
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
  spec.create = [path = spec.path, parent = spec.parent_path, name, description,
                 service, operation = &operation,
                 weak_state = std::weak_ptr<ComponentState>(state), dispatcher,
                 schema = std::move(schema)](::opcua::Server &server,
                                             std::uint16_t namespace_index,
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
    return componentNodeCreated(result, path, error);
  };
  return spec;
}

enum class PortMethodKind { read, write };

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
  const auto bridge_slot = std::make_shared<std::shared_ptr<PortBridge>>();
  spec.create = [path = spec.path, parent = spec.parent_path, method_name,
                 port = &port,
                 weak_state = std::weak_ptr<ComponentState>(state), buffer_size,
                 type_registry = std::move(type_registry), reads, bridge_slot](
                    ::opcua::Server &server, std::uint16_t namespace_index,
                    std::string *error) {
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
    if (codec == nullptr || !codec->hasValue()) {
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

    std::vector<::opcua::Argument> inputs;
    std::vector<::opcua::Argument> outputs;
    const ::opcua::Argument status_argument(
        "status", ::opcua::LocalizedText("en-US", "RTT port flow status."),
        ::opcua::DataTypeId::String, ::opcua::ValueRank::Scalar);
    const ::opcua::Argument value_argument(
        "value", ::opcua::LocalizedText("en-US", "RTT port sample."),
        codec->dataTypeNodeId(), codec->valueRank());
    if (reads) {
      outputs.push_back(status_argument);
      outputs.push_back(value_argument);
    } else {
      inputs.push_back(value_argument);
      outputs.push_back(status_argument);
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
    if (!componentNodeCreated(result, path, error)) {
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
  return snapshot;
}

bool deleteOwnedNodes(::opcua::Server &server, std::uint16_t namespace_index,
                      std::vector<const NodeSpec *> specs,
                      std::string *error) {
  std::sort(specs.begin(), specs.end(),
            [](const NodeSpec *left, const NodeSpec *right) {
              const std::size_t left_depth = pathDepth(left->path);
              const std::size_t right_depth = pathDepth(right->path);
              return left_depth == right_depth ? left->path < right->path
                                               : left_depth > right_depth;
            });

  std::set<::opcua::NodeId> handled;
  const auto remove_identity = [&](const OwnedNodeIdentity &identity) {
    if (!handled.insert(identity.id).second) {
      return true;
    }
    const IdentityState state = identityState(server, identity);
    if (state == IdentityState::missing) {
      return true;
    }
    if (state == IdentityState::foreign) {
      assignError(error, "refusing to delete OPC UA node after ownership changed");
      return false;
    }
    const ::opcua::StatusCode result =
        ::opcua::services::deleteNode(server, identity.id, true);
    if (result.isBad() && result != UA_STATUSCODE_BADNODEIDUNKNOWN) {
      assignError(error, "failed to delete owned OPC UA node: " +
                             statusName(result));
      return false;
    }
    return true;
  };

  for (const NodeSpec *spec : specs) {
    for (const OwnedNodeIdentity &identity : spec->ownership->identities) {
      if (!identity.primary && !remove_identity(identity)) {
        return false;
      }
    }
  }
  for (const NodeSpec *spec : specs) {
    const auto primary = std::ranges::find_if(
        spec->ownership->identities,
        [](const OwnedNodeIdentity &identity) { return identity.primary; });
    if (primary == spec->ownership->identities.end()) {
      if (::opcua::services::readNodeClass(
              server, nodeId(namespace_index, spec->path))) {
        assignError(error, "missing ownership proof for OPC UA node '" +
                               spec->path + "'");
        return false;
      }
      continue;
    }
    if (!remove_identity(*primary)) {
      return false;
    }
  }
  return true;
}

void appendRollbackError(std::string *error, const std::string &rollback_error,
                         bool restored) {
  if (error == nullptr) {
    return;
  }
  *error += restored ? "; rollback succeeded"
                     : "; rollback failed: " + rollback_error;
}

} // namespace

class ObjectModelImpl final
    : public std::enable_shared_from_this<ObjectModelImpl> {
public:
  ObjectModelImpl(Server &model_server, ObjectModelOptions model_options)
      : server(model_server), options(std::move(model_options)),
        type_registry(model_server.typeRegistry()),
        dispatcher(std::make_shared<OperationDispatcher>(
            type_registry, options.operation_timeout)) {}

  ~ObjectModelImpl() { shutdown(); }

  void startWorker() {
    worker = std::jthread([weak = weak_from_this()](std::stop_token stop) {
      while (!stop.stop_requested()) {
        const auto self = weak.lock();
        if (!self) {
          return;
        }
        std::unique_lock<std::mutex> lock(self->worker_mutex);
        self->worker_condition.wait_for(
            lock, self->options.reconcile_interval, [&stop, &self] {
              return stop.stop_requested() || self->shutdown_started.load();
            });
        lock.unlock();
        if (stop.stop_requested() || self->shutdown_started.load()) {
          return;
        }
        self->dispatcher->reapPending();
        self->reconcile(nullptr);
      }
    });
  }

  std::shared_ptr<ComponentState>
  registerComponent(RTT::TaskContext &component, std::string *error,
                    std::vector<UnsupportedResource> *unsupported) {
    if (!server.isRunning()) {
      assignError(
          error,
          "OPC UA server must be running before components are registered");
      return {};
    }
    if (!type_registry) {
      assignError(error, "OPC UA server type registry is unavailable");
      return {};
    }
    if (options.reconcile_interval <= std::chrono::milliseconds::zero()) {
      assignError(error, "object model reconcile interval must be positive");
      return {};
    }
    if (options.operation_timeout <= std::chrono::milliseconds::zero()) {
      assignError(error, "object model operation timeout must be positive");
      return {};
    }
    if (options.port_buffer_size == 0U ||
        options.port_buffer_size >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      assignError(error, "object model port buffer size is out of range");
      return {};
    }

    auto state = std::make_shared<ComponentState>(component);
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      if (shutdown_started.load()) {
        assignError(error, "OPC UA object model is shutting down");
        return {};
      }
      if (components.contains(state->component_name)) {
        assignError(error, "an RTT component named '" + state->component_name +
                               "' is already registered");
        return {};
      }
    }

    ComponentSnapshot snapshot =
        snapshotComponent(state, component, dispatcher, type_registry,
                          options.port_buffer_size);
    const bool rejected = !snapshot.unsupported.empty();
    bool published_snapshot = false;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      if (shutdown_started.load()) {
        assignError(error, "OPC UA object model is shutting down");
        return {};
      }
      if (components.contains(state->component_name)) {
        assignError(error, "an RTT component named '" + state->component_name +
                               "' is already registered");
        return {};
      }

      if (rejected) {
        std::lock_guard<std::mutex> model_lock(model_mutex);
        failed_publications[state->component_name] = snapshot.unsupported;
      } else {
        components.emplace(state->component_name, state);
        published_snapshot = publishSnapshot(state, snapshot, error);
        if (!published_snapshot) {
          components.erase(state->component_name);
        }
      }
    }

    if (rejected) {
      if (unsupported != nullptr) {
        *unsupported = snapshot.unsupported;
      }
      assignError(error, "strict OPC UA publication rejected component '" +
                             state->component_name + "'");
      std::vector<DiagnosticEvent> events;
      events.reserve(snapshot.unsupported.size());
      for (const UnsupportedResource &resource : snapshot.unsupported) {
        events.push_back(DiagnosticEvent{false, resource});
      }
      emitDiagnosticEvents(events);
      return {};
    }

    if (!published_snapshot) {
      deactivate(state);
      return {};
    }

    {
      std::lock_guard<std::mutex> lock(model_mutex);
      failed_publications.erase(state->component_name);
    }
    worker_condition.notify_all();
    return state;
  }

  void
  unregisterComponent(const std::shared_ptr<ComponentState> &state) noexcept {
    if (!state) {
      return;
    }
    bool removed = false;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      const auto found = components.find(state->component_name);
      if (found != components.end() && found->second == state) {
        components.erase(found);
        removed = true;
      }
    }
    deactivate(state);
    if (!removed) {
      return;
    }

    const auto self = shared_from_this();
    const auto changed = std::make_shared<bool>(false);
    const bool invoked = server.invoke(
        [self, state, changed](::opcua::Server &native) {
          std::lock_guard<std::mutex> lock(self->model_mutex);
          *changed =
              self->removePublishedComponent(native, state.get(), nullptr);
          if (*changed) {
            self->advanceRevision(native);
          }
        },
        std::chrono::seconds(5));
    if (!invoked) {
      std::lock_guard<std::mutex> lock(model_mutex);
      published.erase(state.get());
      unsupported.erase(state->component_name);
      failed_publications.erase(state->component_name);
    }
  }

  bool reconcile(std::string *error) {
    if (!server.isRunning()) {
      assignError(error, "OPC UA server is not running");
      return false;
    }

    std::vector<std::shared_ptr<ComponentState>> current;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      current.reserve(components.size());
      for (const auto &[name, state] : components) {
        static_cast<void>(name);
        current.push_back(state);
      }
    }

    const auto self = shared_from_this();
    const auto reconcile_error = std::make_shared<std::string>();
    const auto diagnostic_events =
        std::make_shared<std::vector<DiagnosticEvent>>();
    std::string server_error;
    const bool success = server.invoke(
        [self, current, reconcile_error,
         diagnostic_events](::opcua::Server &native) {
          std::lock_guard<std::mutex> lock(self->model_mutex);
          const auto namespace_index = self->server.namespaceIndex();
          if (!namespace_index || !self->ensureRoots(native, *namespace_index,
                                                     reconcile_error.get())) {
            return;
          }

          for (const auto &state : current) {
            ComponentLease lease(state);
            if (!lease) {
              continue;
            }
            const ComponentSnapshot expected = snapshotComponent(
                state, *lease.get(), self->dispatcher, self->type_registry,
                self->options.port_buffer_size);
            self->updateDiagnostics(state->component_name, expected.unsupported,
                                    *diagnostic_events);
            if (!expected.unsupported.empty()) {
              continue;
            }
            bool component_changed = false;
            if (!self->reconcileComponent(native, *namespace_index, state.get(),
                                          expected.nodes, &component_changed,
                                          reconcile_error.get())) {
              return;
            }
            if (component_changed) {
              self->advanceRevision(native);
            }
          }
        },
        std::chrono::seconds(5), &server_error);

    if (success) {
      emitDiagnosticEvents(*diagnostic_events);
    }

    std::string failure =
        server_error.empty() ? *reconcile_error : server_error;
    if (!success || !failure.empty()) {
      if (failure.empty()) {
        failure = "failed to reconcile the OPC UA object model";
      }
      setLastError(failure);
      assignError(error, failure);
      return false;
    }
    setLastError({});
    assignError(error, {});
    return true;
  }

  std::uint64_t currentRevision() const noexcept { return revision.load(); }

  std::size_t componentCount() const noexcept {
    std::lock_guard<std::mutex> lock(registry_mutex);
    return components.size();
  }

  std::size_t pendingOperationCount() const noexcept {
    return dispatcher->pendingCount();
  }

  std::vector<UnsupportedResource>
  unsupportedResources(std::string_view component) const {
    return diagnosticsFor(component);
  }

  std::string lastError() const {
    std::lock_guard<std::mutex> lock(error_mutex);
    return last_error;
  }

  void shutdown() noexcept {
    bool expected = false;
    if (!shutdown_started.compare_exchange_strong(expected, true)) {
      return;
    }
    worker.request_stop();
    worker_condition.notify_all();
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
      worker.join();
    }
    dispatcher->drainPending();

    std::vector<std::shared_ptr<ComponentState>> states;
    {
      std::lock_guard<std::mutex> lock(registry_mutex);
      for (const auto &[name, state] : components) {
        static_cast<void>(name);
        states.push_back(state);
      }
      components.clear();
    }
    for (const auto &state : states) {
      deactivate(state);
    }

    if (server.isRunning()) {
      const auto self = weak_from_this().lock();
      server.invoke(
          [self](::opcua::Server &native) {
            if (!self) {
              return;
            }
            std::lock_guard<std::mutex> lock(self->model_mutex);
            const auto namespace_index = self->server.namespaceIndex();
            if (!namespace_index) {
              return;
            }
            for (auto &[state, nodes] : self->published) {
              static_cast<void>(state);
              std::vector<std::string> paths;
              std::vector<const NodeSpec *> specs;
              paths.reserve(nodes.size());
              specs.reserve(nodes.size());
              for (const auto &[path, spec] : nodes) {
                paths.push_back(path);
                specs.push_back(&spec);
              }
              if (!removalSubtreesAreOwned(native, *namespace_index, nodes,
                                           paths, nullptr) ||
                  !deleteOwnedNodes(native, *namespace_index, std::move(specs),
                                    nullptr)) {
                releaseOwnershipMarkers(native, nodes);
              }
            }
            self->published.clear();
            self->unsupported.clear();
            self->failed_publications.clear();
          },
          std::chrono::seconds(5));
    }
    std::lock_guard<std::mutex> lock(model_mutex);
    published.clear();
    unsupported.clear();
    failed_publications.clear();
  }

private:
  bool publishSnapshot(const std::shared_ptr<ComponentState> &state,
                       const ComponentSnapshot &snapshot, std::string *error) {
    bool component_changed = false;
    bool published_snapshot = false;
    const bool invoked = server.invoke(
        [this, &state, &snapshot, &component_changed, &published_snapshot,
         error](::opcua::Server &native) {
          std::lock_guard<std::mutex> lock(model_mutex);
          const auto index = server.namespaceIndex();
          if (!index || !ensureRoots(native, *index, error) ||
              !reconcileComponent(native, *index, state.get(), snapshot.nodes,
                                  &component_changed, error)) {
            return;
          }
          if (component_changed) {
            advanceRevision(native);
          }
          published_snapshot = true;
        });
    if (!invoked && (error == nullptr || error->empty())) {
      assignError(error, "failed to invoke initial OPC UA component publication");
    }
    return invoked && published_snapshot;
  }

  std::vector<UnsupportedResource>
  diagnosticsFor(std::string_view component_name) const {
    std::lock_guard<std::mutex> lock(model_mutex);
    const auto active = unsupported.find(component_name);
    if (active != unsupported.end()) {
      return active->second;
    }
    const auto rejected = failed_publications.find(component_name);
    return rejected == failed_publications.end()
               ? std::vector<UnsupportedResource>{}
               : rejected->second;
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
                       "Registered RTT components.") ||
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

  bool reconcileComponent(::opcua::Server &native,
                          std::uint16_t namespace_index,
                          const ComponentState *state, const NodeMap &expected,
                          bool *changed, std::string *error) {
    const auto current = published.find(state);
    const NodeMap previous =
        current == published.end() ? NodeMap{} : current->second;
    NodeMap committed = expected;
    for (const auto &[path, existing] : previous) {
      const auto candidate = committed.find(path);
      if (candidate != committed.end() &&
          candidate->second.fingerprint == existing.fingerprint) {
        candidate->second = existing;
      }
    }

    std::set<std::string> old_remove_set;
    for (const auto &[path, existing] : previous) {
      const auto replacement = expected.find(path);
      if (replacement == expected.end() ||
          replacement->second.fingerprint != existing.fingerprint) {
        old_remove_set.insert(path);
      }
    }
    const std::vector<std::string> direct_remove_paths(old_remove_set.begin(),
                                                       old_remove_set.end());
    for (const auto &[path, spec] : previous) {
      static_cast<void>(spec);
      if (std::ranges::any_of(direct_remove_paths, [&](const auto &ancestor) {
            return isDescendantPath(path, ancestor);
          })) {
        old_remove_set.insert(path);
      }
    }
    std::vector<std::string> old_remove_paths(old_remove_set.begin(),
                                              old_remove_set.end());
    std::sort(old_remove_paths.begin(), old_remove_paths.end(),
              [](const auto &left, const auto &right) {
                const std::size_t left_depth = pathDepth(left);
                const std::size_t right_depth = pathDepth(right);
                return left_depth == right_depth ? left < right
                                                 : left_depth > right_depth;
              });

    std::vector<const NodeSpec *> candidate_specs;
    for (const auto &[path, spec] : committed) {
      const auto existing = previous.find(path);
      if (existing == previous.end() ||
          existing->second.fingerprint != spec.fingerprint ||
          old_remove_set.contains(path)) {
        candidate_specs.push_back(&spec);
      }
    }
    std::sort(candidate_specs.begin(), candidate_specs.end(),
              [](const NodeSpec *left, const NodeSpec *right) {
                const std::size_t left_depth = pathDepth(left->path);
                const std::size_t right_depth = pathDepth(right->path);
                return left_depth == right_depth ? left->path < right->path
                                                 : left_depth < right_depth;
              });

    std::vector<const NodeSpec *> rollback_specs;
    rollback_specs.reserve(old_remove_paths.size());
    for (const auto &[path, spec] : previous) {
      if (old_remove_set.contains(path)) {
        rollback_specs.push_back(&spec);
      }
    }
    std::sort(rollback_specs.begin(), rollback_specs.end(),
              [](const NodeSpec *left, const NodeSpec *right) {
                const std::size_t left_depth = pathDepth(left->path);
                const std::size_t right_depth = pathDepth(right->path);
                return left_depth == right_depth ? left->path < right->path
                                                 : left_depth < right_depth;
              });

    if (!removalSubtreesAreOwned(native, namespace_index, previous,
                                 old_remove_paths, error)) {
      return false;
    }
    for (const NodeSpec *spec : candidate_specs) {
      if (!previous.contains(spec->path) &&
          ::opcua::services::readNodeClass(
              native, nodeId(namespace_index, spec->path))) {
        assignError(error, "OPC UA candidate node collision at '" +
                               spec->path + "' before reconciliation");
        return false;
      }
    }

    const auto restore_old = [&](std::string *rollback_error) {
      std::string cleanup_error;
      const bool removed = deleteOwnedNodes(native, namespace_index,
                                            rollback_specs, &cleanup_error);
      bool restored = removed;
      std::string creation_error;
      if (!removed) {
        assignError(rollback_error, std::move(cleanup_error));
        return false;
      }
      for (const NodeSpec *spec : rollback_specs) {
        if (!spec->create(native, namespace_index, &creation_error) ||
            !captureOwnership(native, namespace_index, *spec,
                              &creation_error)) {
          restored = false;
          break;
        }
      }
      if (!cleanup_error.empty() && !creation_error.empty()) {
        cleanup_error += "; ";
      }
      cleanup_error += creation_error;
      assignError(rollback_error, std::move(cleanup_error));
      return restored;
    };

    if (!deleteOwnedNodes(native, namespace_index, rollback_specs, error)) {
      std::string rollback_error;
      const bool restored = restore_old(&rollback_error);
      appendRollbackError(error, rollback_error, restored);
      return false;
    }

    std::vector<const NodeSpec *> created_candidate_specs;
    for (const NodeSpec *spec : candidate_specs) {
      const bool existed_before = static_cast<bool>(
          ::opcua::services::readNodeClass(
              native, nodeId(namespace_index, spec->path)));
      const bool created = spec->create(native, namespace_index, error);
      const bool captured =
          created && captureOwnership(native, namespace_index, *spec, error);
      if (!created || !captured) {
        std::vector<const NodeSpec *> cleanup_specs = created_candidate_specs;
        std::string ownership_error;
        if (!existed_before &&
            ::opcua::services::readNodeClass(
                native, nodeId(namespace_index, spec->path)) &&
            (!spec->ownership->identities.empty() ||
             captureOwnership(native, namespace_index, *spec,
                              &ownership_error))) {
          cleanup_specs.push_back(spec);
        }
        std::string cleanup_error;
        const bool removed = deleteOwnedNodes(native, namespace_index,
                                              std::move(cleanup_specs),
                                              &cleanup_error);
        if (!ownership_error.empty()) {
          if (!cleanup_error.empty()) {
            cleanup_error += "; ";
          }
          cleanup_error += ownership_error;
        }
        std::string restore_error;
        const bool old_restored = restore_old(&restore_error);
        std::string rollback_error = cleanup_error;
        if (!restore_error.empty()) {
          if (!rollback_error.empty()) {
            rollback_error += "; ";
          }
          rollback_error += restore_error;
        }
        const bool restored = removed && old_restored;
        appendRollbackError(error, rollback_error, restored);
        return false;
      }
      created_candidate_specs.push_back(spec);
    }

    published.insert_or_assign(state, std::move(committed));
    *changed = !old_remove_paths.empty() || !candidate_specs.empty();
    return true;
  }

  bool updateDiagnostics(const std::string &component,
                         const std::vector<UnsupportedResource> &expected,
                         std::vector<DiagnosticEvent> &events) {
    std::vector<UnsupportedResource> &current = unsupported[component];
    std::vector<UnsupportedResource> additions;
    std::vector<UnsupportedResource> removals;
    std::set_difference(expected.begin(), expected.end(), current.begin(),
                        current.end(), std::back_inserter(additions));
    std::set_difference(current.begin(), current.end(), expected.begin(),
                        expected.end(), std::back_inserter(removals));
    for (auto &resource : additions) {
      events.push_back(DiagnosticEvent{false, std::move(resource)});
    }
    for (auto &resource : removals) {
      events.push_back(DiagnosticEvent{true, std::move(resource)});
    }
    if (additions.empty() && removals.empty()) {
      return false;
    }
    current = expected;
    return true;
  }

  void emitDiagnosticEvents(
      const std::vector<DiagnosticEvent> &events) const noexcept {
    for (const DiagnosticEvent &event : events) {
      const UnsupportedResource &resource = event.resource;
      const std::string message =
          event.recovered
              ? "OPC UA: component '" + resource.component +
                    "' now publishes " + resource.kind + " '" + resource.path +
                    "' with RTT type '" + resource.type_name + "'."
              : resource.message();
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
      RTT::Logger::log().logf(event.recovered ? RTT::Logger::Info
                                              : RTT::Logger::Warning,
                              "ObjectModel", "%s", message.c_str());
    }
  }

  bool removePublishedComponent(::opcua::Server &native,
                                const ComponentState *state,
                                std::string *error) {
    const bool active_diagnostics_removed =
        unsupported.erase(state->component_name) != 0U;
    const bool failed_diagnostics_removed =
        failed_publications.erase(state->component_name) != 0U;
    const bool diagnostics_removed =
        active_diagnostics_removed || failed_diagnostics_removed;
    const auto found = published.find(state);
    if (found == published.end()) {
      return diagnostics_removed;
    }
    const auto namespace_index = server.namespaceIndex();
    if (!namespace_index) {
      published.erase(found);
      return true;
    }
    std::vector<std::string> paths;
    std::vector<const NodeSpec *> specs;
    paths.reserve(found->second.size());
    specs.reserve(found->second.size());
    for (const auto &[path, spec] : found->second) {
      paths.push_back(path);
      specs.push_back(&spec);
    }
    const bool owned = removalSubtreesAreOwned(
        native, *namespace_index, found->second, paths, error);
    const bool deleted =
        owned && deleteOwnedNodes(native, *namespace_index, std::move(specs),
                                  error);
    if (!deleted) {
      releaseOwnershipMarkers(native, found->second);
    }
    published.erase(found);
    return deleted || diagnostics_removed;
  }

  void advanceRevision(::opcua::Server &native) {
    const std::uint64_t next = revision.fetch_add(1U) + 1U;
    const auto namespace_index = server.namespaceIndex();
    if (!namespace_index) {
      return;
    }
    ::opcua::services::writeValue(
        native,
        nodeId(
            *namespace_index,
            appendNodeSegment(appendNodeSegment("rtt", "model"), "revision")),
        ::opcua::Variant(next));
  }

  void setLastError(std::string value) {
    std::lock_guard<std::mutex> lock(error_mutex);
    last_error = std::move(value);
  }

  Server &server;
  ObjectModelOptions options;
  std::shared_ptr<const EndpointTypeRegistry> type_registry;
  std::shared_ptr<OperationDispatcher> dispatcher;
  mutable std::mutex registry_mutex;
  std::map<std::string, std::shared_ptr<ComponentState>> components;
  mutable std::mutex model_mutex;
  std::map<const ComponentState *, NodeMap> published;
  std::map<std::string, std::vector<UnsupportedResource>, std::less<>>
      unsupported;
  std::map<std::string, std::vector<UnsupportedResource>, std::less<>>
      failed_publications;
  bool roots_ready{false};
  std::atomic<std::uint64_t> revision{0U};
  mutable std::mutex error_mutex;
  std::string last_error;
  std::atomic_bool shutdown_started{false};
  std::mutex worker_mutex;
  std::condition_variable worker_condition;
  std::jthread worker;
};

} // namespace detail

std::string UnsupportedResource::message() const {
  return "OPC UA: component '" + component + "' skipped " + kind + " '" + path +
         "' because RTT type '" + type_name + "' " + reason + ".";
}

ComponentRegistration::ComponentRegistration(
    std::weak_ptr<detail::ObjectModelImpl> model,
    std::shared_ptr<detail::ComponentState> state)
    : model_(std::move(model)), state_(std::move(state)) {}

ComponentRegistration::~ComponentRegistration() { reset(); }

ComponentRegistration::ComponentRegistration(
    ComponentRegistration &&other) noexcept
    : model_(std::move(other.model_)), state_(std::move(other.state_)) {}

ComponentRegistration &
ComponentRegistration::operator=(ComponentRegistration &&other) noexcept {
  if (this != &other) {
    reset();
    model_ = std::move(other.model_);
    state_ = std::move(other.state_);
  }
  return *this;
}

bool ComponentRegistration::active() const noexcept {
  return detail::isActive(state_);
}

ComponentRegistration::operator bool() const noexcept { return active(); }

std::string ComponentRegistration::name() const {
  return state_ ? state_->component_name : std::string{};
}

void ComponentRegistration::reset() noexcept {
  if (!state_) {
    return;
  }
  if (const auto model = model_.lock()) {
    model->unregisterComponent(state_);
  } else {
    detail::deactivate(state_);
  }
  state_.reset();
  model_.reset();
}

ObjectModel::ObjectModel(Server &server, ObjectModelOptions options)
    : impl_(std::make_shared<detail::ObjectModelImpl>(server,
                                                      std::move(options))) {
  impl_->startWorker();
}

ObjectModel::~ObjectModel() { impl_->shutdown(); }

std::optional<ComponentRegistration>
ObjectModel::registerComponent(RTT::TaskContext &component,
                               std::string *error,
                               std::vector<UnsupportedResource> *unsupported) {
  const auto state = impl_->registerComponent(component, error, unsupported);
  if (!state) {
    return std::nullopt;
  }
  return ComponentRegistration(impl_, state);
}

bool ObjectModel::reconcile(std::string *error) {
  return impl_->reconcile(error);
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
