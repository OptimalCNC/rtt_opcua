#pragma once

#include <rtt/opcua/endpoint_type_registry.hpp>
#include <rtt/opcua/task_context_proxy.hpp>

#include <open62541pp/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace opcua {
class Client;
}

namespace RTT::opcua::detail {

struct RemoteOperationDescription {
  std::string name;
  std::string description;
  ::opcua::NodeId object_id;
  ::opcua::NodeId method_id;
  std::vector<std::string> input_types;
  std::vector<std::string> input_names;
  std::vector<std::string> input_descriptions;
  std::vector<std::string> output_types;
  std::vector<std::int32_t> output_sources;
};

struct RemoteCallResult {
  bool success{false};
  std::vector<::opcua::Variant> outputs;
  std::string error;
};

enum class RemoteValueCategory { properties, attributes };

struct RemoteValueDescription {
  std::string name;
  std::string description;
  std::string type_name;
  ::opcua::NodeId node_id;
  bool writable{false};
};

using RemoteServicePath = std::vector<std::string>;

struct RemoteServiceDescription {
  std::string name;
  std::string description;
};

enum class RemotePortDirection { input, output };

struct RemotePortDescription {
  std::string name;
  std::string description;
  std::string type_name;
  RemoteServicePath service_path;
  RemotePortDirection direction{RemotePortDirection::input};
  ::opcua::NodeId object_id;
  ::opcua::NodeId method_id;
};

class ClientSession final {
public:
  ClientSession(std::string endpoint_url,
                std::chrono::milliseconds request_timeout);
  ~ClientSession();

  ClientSession(const ClientSession &) = delete;
  ClientSession &operator=(const ClientSession &) = delete;

  bool connect(std::string *error);
  std::vector<RemoteOperationDescription>
  discoverOperations(const std::string &component_name,
                     const RemoteServicePath &service_path, std::string *error);
  std::vector<RemoteValueDescription>
  discoverValues(const std::string &component_name,
                 const RemoteServicePath &service_path,
                 RemoteValueCategory category, std::string *error);
  std::vector<RemoteServiceDescription>
  discoverServices(const std::string &component_name,
                   const RemoteServicePath &service_path, std::string *error);
  std::vector<RemotePortDescription>
  discoverPorts(const std::string &component_name,
                const RemoteServicePath &service_path, std::string *error);
  bool readServiceDescription(const std::string &component_name,
                              const RemoteServicePath &service_path,
                              std::string *description, std::string *error);
  bool readLifecycleState(const std::string &component_name,
                          std::string *lifecycle_state, std::string *error);
  bool readValue(const ::opcua::NodeId &node_id, ::opcua::Variant *value);
  bool writeValue(const ::opcua::NodeId &node_id,
                  const ::opcua::Variant &value);
  RemoteCallResult call(const ::opcua::NodeId &object_id,
                        const ::opcua::NodeId &method_id,
                        const std::vector<::opcua::Variant> &inputs);
  RemoteCallResult callPort(const ::opcua::NodeId &object_id,
                            const ::opcua::NodeId &method_id,
                            const std::vector<::opcua::Variant> &inputs);
  RemoteCallResult callOperation(const std::string &component_name,
                                 const RemoteServicePath &service_path,
                                 const std::string &operation_name,
                                 const std::vector<::opcua::Variant> &inputs);
  void setInterfaceAccessEnabled(bool enabled);

  ProxyConnectionState state() const noexcept;
  const std::string &endpointUrl() const noexcept;
  std::shared_ptr<const EndpointTypeRegistry> typeRegistry() const;
  std::string lastError() const;

private:
  void invalidateInterfaceLocked() noexcept;
  RemoteCallResult callLocked(const ::opcua::NodeId &object_id,
                              const ::opcua::NodeId &method_id,
                              const std::vector<::opcua::Variant> &inputs,
                              bool require_interface_access,
                              bool tolerate_not_connected);

  const std::string endpoint_url_;
  const std::chrono::milliseconds request_timeout_;
  mutable std::mutex mutex_;
  std::shared_ptr<EndpointTypeRegistry> type_registry_;
  std::unique_ptr<::opcua::Client> client_;
  std::uint16_t namespace_index_{0U};
  std::atomic<ProxyConnectionState> state_{ProxyConnectionState::disconnected};
  std::atomic<bool> interface_access_enabled_{false};
  std::string last_error_;
};

} // namespace RTT::opcua::detail
