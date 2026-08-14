#include "port_bridge.hpp"

#include <rtt/opcua/endpoint_type_registry.hpp>

#include <rtt/ConnPolicy.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <utility>

namespace RTT::opcua::detail {
namespace {

void assignError(std::string *output, std::string value) {
  if (output != nullptr) {
    *output = std::move(value);
  }
}

} // namespace

PortBridge::PortBridge(
    std::shared_ptr<const EndpointTypeRegistry> type_registry,
    std::unique_ptr<RTT::base::PortInterface> peer)
    : type_registry_(std::move(type_registry)), peer_(std::move(peer)) {}

PortBridge::~PortBridge() {
  if (peer_) {
    try {
      peer_->disconnect();
    } catch (...) {
      // Destruction must not propagate a transport cleanup failure.
    }
  }
}

std::shared_ptr<PortBridge>
PortBridge::create(RTT::base::InputPortInterface &port,
                   std::shared_ptr<const EndpointTypeRegistry> type_registry,
                   std::string *error) {
  if (!type_registry || port.getTypeInfo() == nullptr ||
      type_registry->codecForTypeInfo(port.getTypeInfo()) == nullptr) {
    assignError(error, "OPC UA port type is not transportable");
    return {};
  }

  std::unique_ptr<RTT::base::PortInterface> peer(port.antiClone());
  if (!peer) {
    assignError(error, "failed to create an RTT anti-port");
    return {};
  }

  auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(peer.get());
  const bool connected = output != nullptr && output->createConnection(port);
  if (!connected) {
    assignError(error, "failed to connect the RTT OPC UA anti-port");
    return {};
  }

  assignError(error, {});
  return std::shared_ptr<PortBridge>(
      new PortBridge(std::move(type_registry), std::move(peer)));
}

std::shared_ptr<PortBridge>
PortBridge::observe(RTT::base::OutputPortInterface &port, std::string *error) {
  std::unique_ptr<RTT::base::PortInterface> peer(port.antiClone());
  if (!peer) {
    assignError(error, "failed to create an RTT anti-port");
    return {};
  }

  auto *input = dynamic_cast<RTT::base::InputPortInterface *>(peer.get());
  const RTT::ConnPolicy policy =
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false);
  if (input == nullptr || !port.createConnection(*input, policy)) {
    assignError(error, "failed to connect the RTT OPC UA anti-port");
    return {};
  }

  assignError(error, {});
  return std::shared_ptr<PortBridge>(new PortBridge({}, std::move(peer)));
}

::opcua::StatusCode
PortBridge::write(const ::opcua::Variant &encoded) noexcept {
  auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(peer_.get());
  if (output == nullptr || output->getTypeInfo() == nullptr) {
    return UA_STATUSCODE_BADNOTCONNECTED;
  }

  try {
    const TypeCodec *codec =
        type_registry_->codecForTypeInfo(output->getTypeInfo());
    const auto value = codec == nullptr
                           ? RTT::base::DataSourceBase::shared_ptr{}
                           : codec->makeDataSource(encoded);
    if (!value) {
      return UA_STATUSCODE_BADTYPEMISMATCH;
    }
    switch (output->write(value)) {
    case RTT::WriteSuccess:
      return UA_STATUSCODE_GOOD;
    case RTT::NotConnected:
      return UA_STATUSCODE_BADNOTCONNECTED;
    case RTT::WriteFailure:
      return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  } catch (...) {
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  }
}

} // namespace RTT::opcua::detail
