#pragma once

#include <rtt/TaskContext.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace RTT::opcua::detail {

struct ComponentState {
  ComponentState(RTT::TaskContext &value,
                 std::shared_ptr<std::atomic_bool> model_closed)
      : component(&value), component_name(value.getName()),
        admission_closed(std::move(model_closed)) {}

  mutable std::mutex mutex;
  std::condition_variable condition;
  RTT::TaskContext *component;
  const std::string component_name;
  const std::shared_ptr<std::atomic_bool> admission_closed;
  bool is_active{true};
  std::size_t users{0U};
};

class ComponentLease final {
public:
  explicit ComponentLease(const std::shared_ptr<ComponentState> &state)
      : state_(state) {
    if (!state_) {
      return;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->admission_closed->load() || !state_->is_active ||
        state_->component == nullptr) {
      state_.reset();
      return;
    }
    ++state_->users;
    component_ = state_->component;
  }

  ~ComponentLease() { release(); }

  ComponentLease(const ComponentLease &) = delete;
  ComponentLease &operator=(const ComponentLease &) = delete;

  ComponentLease(ComponentLease &&other) noexcept
      : state_(std::move(other.state_)),
        component_(std::exchange(other.component_, nullptr)) {}

  ComponentLease &operator=(ComponentLease &&other) noexcept {
    if (this != &other) {
      release();
      state_ = std::move(other.state_);
      component_ = std::exchange(other.component_, nullptr);
    }
    return *this;
  }

  explicit operator bool() const noexcept { return component_ != nullptr; }

  RTT::TaskContext *get() const noexcept { return component_; }

private:
  void release() noexcept {
    if (!state_) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      --state_->users;
    }
    state_->condition.notify_all();
    state_.reset();
    component_ = nullptr;
  }

  std::shared_ptr<ComponentState> state_;
  RTT::TaskContext *component_{nullptr};
};

inline void deactivate(const std::shared_ptr<ComponentState> &state) noexcept {
  if (!state) {
    return;
  }
  std::unique_lock<std::mutex> lock(state->mutex);
  state->is_active = false;
  state->condition.wait(lock, [&state] { return state->users == 0U; });
  state->component = nullptr;
}

inline bool isActive(const std::shared_ptr<ComponentState> &state) noexcept {
  if (!state) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  return !state->admission_closed->load() && state->is_active &&
         state->component != nullptr;
}

} // namespace RTT::opcua::detail
