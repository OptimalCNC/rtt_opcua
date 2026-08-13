#include "port_bridge.hpp"

#include <rtt/opcua/endpoint_type_registry.hpp>

#include <rtt/ConnPolicy.hpp>
#include <rtt/FlowStatus.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <limits>
#include <string_view>
#include <utility>

namespace RTT::opcua::detail {
namespace {

void assignError(std::string *output, std::string value) {
  if (output != nullptr) {
    *output = std::move(value);
  }
}

template <typename Status>
bool encodeStatus(const EndpointTypeRegistry &type_registry,
                  std::string_view type_name, Status status,
                  ::opcua::Variant *value) {
  const TypeCodec *codec = type_registry.codecForTypeName(type_name);
  if (codec == nullptr || value == nullptr) {
    return false;
  }
  typename RTT::internal::ValueDataSource<Status>::shared_ptr source =
      new RTT::internal::ValueDataSource<Status>(status);
  return codec->toVariant(source, value);
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
PortBridge::create(RTT::base::PortInterface &port,
                   std::shared_ptr<const EndpointTypeRegistry> type_registry,
                   std::size_t buffer_size, std::string *error) {
  if (buffer_size == 0U ||
      buffer_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    assignError(error, "OPC UA port buffer size is out of range");
    return {};
  }
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

  bool connected = false;
  if (auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(&port)) {
    auto *input = dynamic_cast<RTT::base::InputPortInterface *>(peer.get());
    const RTT::ConnPolicy policy = RTT::ConnPolicy::circularBuffer(
        static_cast<int>(buffer_size), RTT::ConnPolicy::LOCK_FREE);
    connected = input != nullptr && output->createConnection(*input, policy);
  } else if (auto *input =
                 dynamic_cast<RTT::base::InputPortInterface *>(&port)) {
    auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(peer.get());
    connected = output != nullptr && output->createConnection(*input);
  }
  if (!connected) {
    assignError(error, "failed to connect the RTT OPC UA anti-port");
    return {};
  }

  assignError(error, {});
  return std::shared_ptr<PortBridge>(
      new PortBridge(std::move(type_registry), std::move(peer)));
}

::opcua::StatusCode
PortBridge::read(::opcua::Span<::opcua::Variant> outputs) noexcept {
  if (outputs.size() != 2U) {
    return UA_STATUSCODE_BADINVALIDARGUMENT;
  }
  auto *input = dynamic_cast<RTT::base::InputPortInterface *>(peer_.get());
  if (input == nullptr || input->getTypeInfo() == nullptr) {
    return UA_STATUSCODE_BADMETHODINVALID;
  }

  try {
    const auto value = input->getTypeInfo()->buildValue();
    const TypeCodec *codec =
        type_registry_->codecForTypeInfo(input->getTypeInfo());
    if (!value || codec == nullptr) {
      return UA_STATUSCODE_BADNOTSUPPORTED;
    }
    const RTT::FlowStatus status = input->read(value, true);
    if (!encodeStatus(*type_registry_, "FlowStatus", status, &outputs[0])) {
      return UA_STATUSCODE_BADINTERNALERROR;
    }
    if (!codec->toVariant(value, &outputs[1])) {
      return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
  } catch (...) {
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  }
}

::opcua::StatusCode
PortBridge::write(::opcua::Span<const ::opcua::Variant> inputs,
                  ::opcua::Span<::opcua::Variant> outputs) noexcept {
  if (inputs.size() != 1U || outputs.size() != 1U) {
    return UA_STATUSCODE_BADINVALIDARGUMENT;
  }
  auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(peer_.get());
  if (output == nullptr || output->getTypeInfo() == nullptr) {
    return UA_STATUSCODE_BADMETHODINVALID;
  }

  try {
    const TypeCodec *codec =
        type_registry_->codecForTypeInfo(output->getTypeInfo());
    const auto value = codec == nullptr
                           ? RTT::base::DataSourceBase::shared_ptr{}
                           : codec->makeDataSource(inputs[0]);
    if (!value) {
      return UA_STATUSCODE_BADINVALIDARGUMENT;
    }
    if (!encodeStatus(*type_registry_, "WriteStatus", output->write(value),
                      &outputs[0])) {
      return UA_STATUSCODE_BADINTERNALERROR;
    }
    return UA_STATUSCODE_GOOD;
  } catch (...) {
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  }
}

} // namespace RTT::opcua::detail
