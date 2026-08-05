#pragma once

#include <rtt/opcua/server.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RTT {
class TaskContext;
}

namespace RTT::opcua {

namespace detail {
class ObjectModelImpl;
} // namespace detail

struct UnsupportedResource {
  std::string component;
  std::string path;
  std::string kind;
  std::string type_name;
  std::string reason;

  std::string message() const;
  auto operator<=>(const UnsupportedResource &) const = default;
};

struct ObjectModelOptions {
  std::chrono::milliseconds operation_timeout{std::chrono::seconds(5)};
  std::size_t port_buffer_size{64U};
  std::function<void(const std::string &)> warning_sink;
};

class ObjectModel final {
public:
  explicit ObjectModel(Server &server, ObjectModelOptions options = {});
  ~ObjectModel();

  ObjectModel(const ObjectModel &) = delete;
  ObjectModel &operator=(const ObjectModel &) = delete;
  ObjectModel(ObjectModel &&) = delete;
  ObjectModel &operator=(ObjectModel &&) = delete;

  bool publishComponent(
      RTT::TaskContext &component, std::string *error = nullptr,
      std::vector<UnsupportedResource> *unsupported = nullptr);

  std::uint64_t revision() const noexcept;
  std::size_t componentCount() const noexcept;
  std::size_t pendingOperationCount() const noexcept;
  std::vector<UnsupportedResource>
  unsupportedResources(std::string_view component) const;
  std::string lastError() const;

private:
  std::shared_ptr<detail::ObjectModelImpl> impl_;
};

} // namespace RTT::opcua
