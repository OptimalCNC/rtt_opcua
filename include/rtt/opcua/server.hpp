#pragma once

#include <rtt/opcua/server_options.hpp>

#include <open62541pp/server.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace RTT::opcua {

class EndpointTypeRegistry;

enum class ServerState {
  stopped,
  starting,
  running,
  stopping,
  failed,
};

class Server final {
public:
  using Task = std::function<void(::opcua::Server &)>;

  explicit Server(ServerOptions options = {});
  ~Server();

  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  Server(Server &&) = delete;
  Server &operator=(Server &&) = delete;

  bool start(std::string *error = nullptr);
  void stop() noexcept;

  ServerState state() const noexcept;
  bool isRunning() const noexcept;
  const ServerOptions &options() const noexcept;
  std::string endpointUrl() const;
  std::optional<std::uint16_t> namespaceIndex() const noexcept;
  std::optional<std::uint16_t>
  namespaceIndex(std::string_view namespace_uri) const noexcept;
  std::shared_ptr<const EndpointTypeRegistry> typeRegistry() const;
  std::string lastError() const;

  bool post(Task task);
  // The timeout cancels only a queued task. Once execution starts, invoke()
  // waits for the callback's result so synchronous captures remain valid.
  bool invoke(Task task,
              std::chrono::milliseconds timeout = std::chrono::seconds(5),
              std::string *error = nullptr);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace RTT::opcua
