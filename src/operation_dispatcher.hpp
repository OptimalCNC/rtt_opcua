#pragma once

#include <open62541pp/services/nodemanagement.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace RTT {
class OperationInterfacePart;
}

namespace RTT::opcua::detail {

struct ComponentState;

struct OperationSchema {
  bool supported{false};
  std::string fingerprint;
  std::vector<::opcua::Argument> inputs;
  std::vector<::opcua::Argument> outputs;
};

class OperationDispatcher final {
public:
  explicit OperationDispatcher(std::chrono::milliseconds timeout);
  ~OperationDispatcher();

  OperationDispatcher(const OperationDispatcher &) = delete;
  OperationDispatcher &operator=(const OperationDispatcher &) = delete;

  OperationSchema describe(RTT::OperationInterfacePart &operation) const;
  ::opcua::StatusCode invoke(const std::shared_ptr<ComponentState> &state,
                             RTT::OperationInterfacePart &operation,
                             ::opcua::Span<const ::opcua::Variant> inputs,
                             ::opcua::Span<::opcua::Variant> outputs) noexcept;

  void reapPending() noexcept;
  void drainPending() noexcept;
  std::size_t pendingCount() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace RTT::opcua::detail
