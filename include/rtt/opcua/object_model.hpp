#pragma once

#include <rtt/opcua/server.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace RTT {
class TaskContext;
}

namespace RTT::opcua {

namespace detail {
class ObjectModelImpl;
struct ComponentState;
} // namespace detail

struct ObjectModelOptions {
  std::chrono::milliseconds reconcile_interval{std::chrono::milliseconds(100)};
  std::chrono::milliseconds operation_timeout{std::chrono::seconds(5)};
};

class ComponentRegistration final {
public:
  ComponentRegistration() = default;
  ~ComponentRegistration();

  ComponentRegistration(const ComponentRegistration &) = delete;
  ComponentRegistration &operator=(const ComponentRegistration &) = delete;
  ComponentRegistration(ComponentRegistration &&other) noexcept;
  ComponentRegistration &operator=(ComponentRegistration &&other) noexcept;

  bool active() const noexcept;
  explicit operator bool() const noexcept;
  std::string name() const;
  void reset() noexcept;

private:
  friend class ObjectModel;

  ComponentRegistration(std::weak_ptr<detail::ObjectModelImpl> model,
                        std::shared_ptr<detail::ComponentState> state);

  std::weak_ptr<detail::ObjectModelImpl> model_;
  std::shared_ptr<detail::ComponentState> state_;
};

class ObjectModel final {
public:
  explicit ObjectModel(Server &server, ObjectModelOptions options = {});
  ~ObjectModel();

  ObjectModel(const ObjectModel &) = delete;
  ObjectModel &operator=(const ObjectModel &) = delete;
  ObjectModel(ObjectModel &&) = delete;
  ObjectModel &operator=(ObjectModel &&) = delete;

  std::optional<ComponentRegistration>
  registerComponent(RTT::TaskContext &component, std::string *error = nullptr);
  bool reconcile(std::string *error = nullptr);

  std::uint64_t revision() const noexcept;
  std::size_t componentCount() const noexcept;
  std::string lastError() const;

private:
  std::shared_ptr<detail::ObjectModelImpl> impl_;
};

} // namespace RTT::opcua
