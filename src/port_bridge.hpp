#pragma once

#include <open62541pp/types.hpp>

#include <memory>
#include <string>

namespace RTT::base {
class InputPortInterface;
class OutputPortInterface;
class PortInterface;
}

namespace RTT::opcua {
class EndpointTypeRegistry;
}

namespace RTT::opcua::detail {

class PortBridge final {
public:
  static std::shared_ptr<PortBridge>
  create(RTT::base::InputPortInterface &port,
         std::shared_ptr<const EndpointTypeRegistry> type_registry,
         std::string *error = nullptr);

  static std::shared_ptr<PortBridge>
  observe(RTT::base::OutputPortInterface &port, std::string *error = nullptr);

  ~PortBridge();

  PortBridge(const PortBridge &) = delete;
  PortBridge &operator=(const PortBridge &) = delete;

  ::opcua::StatusCode write(const ::opcua::Variant &value) noexcept;

private:
  PortBridge(std::shared_ptr<const EndpointTypeRegistry> type_registry,
             std::unique_ptr<RTT::base::PortInterface> peer);

  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  std::unique_ptr<RTT::base::PortInterface> peer_;
};

} // namespace RTT::opcua::detail
