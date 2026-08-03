#pragma once

#include <open62541pp/types.hpp>

#include <cstddef>
#include <memory>
#include <string>

namespace RTT::base {
class PortInterface;
}

namespace RTT::opcua {
class EndpointTypeRegistry;
}

namespace RTT::opcua::detail {

class PortBridge final {
public:
  static std::shared_ptr<PortBridge>
  create(RTT::base::PortInterface &port,
         std::shared_ptr<const EndpointTypeRegistry> type_registry,
         std::size_t buffer_size, std::string *error = nullptr);

  ~PortBridge();

  PortBridge(const PortBridge &) = delete;
  PortBridge &operator=(const PortBridge &) = delete;

  ::opcua::StatusCode read(::opcua::Span<::opcua::Variant> outputs) noexcept;
  ::opcua::StatusCode write(::opcua::Span<const ::opcua::Variant> inputs,
                            ::opcua::Span<::opcua::Variant> outputs) noexcept;

private:
  PortBridge(std::shared_ptr<const EndpointTypeRegistry> type_registry,
             std::unique_ptr<RTT::base::PortInterface> peer);

  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  std::unique_ptr<RTT::base::PortInterface> peer_;
};

} // namespace RTT::opcua::detail
