#pragma once

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

class ClientSession final {
public:
  ClientSession(std::string endpoint_url,
                std::chrono::milliseconds request_timeout);
  ~ClientSession();

  ClientSession(const ClientSession &) = delete;
  ClientSession &operator=(const ClientSession &) = delete;

  bool connect(std::string *error);
  std::vector<RemoteOperationDescription>
  discoverOperations(const std::string &component_name, std::string *error);
  std::vector<RemoteValueDescription>
  discoverValues(const std::string &component_name,
                 RemoteValueCategory category, std::string *error);
  bool readValue(const ::opcua::NodeId &node_id, ::opcua::Variant *value);
  bool writeValue(const ::opcua::NodeId &node_id,
                  const ::opcua::Variant &value);
  RemoteCallResult call(const ::opcua::NodeId &object_id,
                        const ::opcua::NodeId &method_id,
                        const std::vector<::opcua::Variant> &inputs);

  ProxyConnectionState state() const noexcept;
  const std::string &endpointUrl() const noexcept;
  std::string lastError() const;

private:
  const std::string endpoint_url_;
  const std::chrono::milliseconds request_timeout_;
  mutable std::mutex mutex_;
  std::unique_ptr<::opcua::Client> client_;
  std::uint16_t namespace_index_{0U};
  std::atomic<ProxyConnectionState> state_{ProxyConnectionState::disconnected};
  std::string last_error_;
};

} // namespace RTT::opcua::detail
