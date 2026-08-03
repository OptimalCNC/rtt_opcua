#include "remote_port.hpp"

#include <rtt/FlowStatus.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <exception>
#include <utility>
#include <vector>

namespace RTT::opcua::detail {
namespace {

void assignError(std::string *output, std::string value) {
  if (output != nullptr) {
    *output = std::move(value);
  }
}

} // namespace

std::shared_ptr<RemotePortAdapter>
RemotePortAdapter::create(std::shared_ptr<ClientSession> session,
                          RemotePortDescription description,
                          std::string *error) {
  RTT::types::TypeInfo *type_info =
      RTT::types::Types()->type(description.type_name);
  const auto type_registry = session ? session->typeRegistry() : nullptr;
  const TypeCodec *codec =
      type_registry ? type_registry->codecForTypeInfo(type_info) : nullptr;
  if (!session || type_info == nullptr || codec == nullptr ||
      !codec->hasValue()) {
    assignError(error, "remote port '" + description.name +
                           "' uses unsupported RTT type '" +
                           description.type_name + "'");
    return {};
  }

  std::unique_ptr<RTT::base::PortInterface> port(
      description.direction == RemotePortDirection::input
          ? static_cast<RTT::base::PortInterface *>(
                type_info->inputPort(description.name))
          : static_cast<RTT::base::PortInterface *>(
                type_info->outputPort(description.name)));
  if (!port) {
    assignError(error, "failed to construct remote RTT port '" +
                           description.name + "'");
    return {};
  }
  port->doc(description.description);
  assignError(error, {});
  return std::shared_ptr<RemotePortAdapter>(
      new RemotePortAdapter(std::move(session), std::move(description),
                            type_registry, type_info, codec, std::move(port)));
}

RemotePortAdapter::RemotePortAdapter(
    std::shared_ptr<ClientSession> session, RemotePortDescription description,
    std::shared_ptr<const EndpointTypeRegistry> type_registry,
    const RTT::types::TypeInfo *type_info, const TypeCodec *codec,
    std::unique_ptr<RTT::base::PortInterface> port)
    : session_(std::move(session)), description_(std::move(description)),
      type_registry_(std::move(type_registry)), type_info_(type_info),
      codec_(codec), port_(std::move(port)) {}

RemotePortAdapter::~RemotePortAdapter() noexcept {
  if (!port_) {
    return;
  }

  // DataFlowInterface stores non-owning port pointers. If RTT failed to
  // unregister this port, releasing it is the only safe teardown fallback.
  if (port_->getInterface() != nullptr) {
    static_cast<void>(port_.release());
    return;
  }
  try {
    port_->disconnect();
  } catch (...) {
    static_cast<void>(port_.release());
  }
}

RTT::base::PortInterface &RemotePortAdapter::port() const { return *port_; }

void RemotePortAdapter::pump() noexcept {
  try {
    if (description_.direction == RemotePortDirection::input) {
      pumpInput();
    } else {
      pumpOutput();
    }
  } catch (const std::exception &exception) {
    setError("remote port '" + description_.name +
             "' pump failed: " + exception.what());
  } catch (...) {
    setError("remote port '" + description_.name +
             "' pump failed with an unknown error");
  }
}

bool RemotePortAdapter::hasPendingState() const noexcept {
  return pending_input_source_ || pending_input_ || pending_output_;
}

bool RemotePortAdapter::canTransferStateTo(
    const RemotePortAdapter &replacement) const noexcept {
  return hasSameIdentity(replacement);
}

bool RemotePortAdapter::transferPendingStateTo(RemotePortAdapter &replacement) {
  if (!hasSameIdentity(replacement)) {
    return false;
  }

  pending_input_source_.swap(replacement.pending_input_source_);
  pending_input_.swap(replacement.pending_input_);
  pending_output_.swap(replacement.pending_output_);
  const std::scoped_lock lock(error_mutex_, replacement.error_mutex_);
  replacement.last_error_ = std::move(last_error_);
  return true;
}

void RemotePortAdapter::discardPendingState() noexcept {
  pending_input_source_.reset();
  pending_input_.reset();
  pending_output_.reset();
  output_was_connected_ = false;
  try {
    const std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
  } catch (...) {
  }
}

const std::string &RemotePortAdapter::name() const noexcept {
  return description_.name;
}

std::string RemotePortAdapter::lastError() const {
  const std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

void RemotePortAdapter::pumpInput() {
  auto *input = dynamic_cast<RTT::base::InputPortInterface *>(port_.get());
  if (input == nullptr) {
    return;
  }

  if (!pending_input_source_ && !pending_input_) {
    if (!input->connected()) {
      return;
    }
    RTT::base::DataSourceBase::shared_ptr value = type_info_->buildValue();
    if (!value || input->read(value, false) != RTT::NewData) {
      return;
    }
    pending_input_source_ = std::move(value);
  }

  if (!pending_input_) {
    ::opcua::Variant encoded;
    if (!codec_->toVariant(pending_input_source_, &encoded)) {
      setError("failed to encode pending sample for remote input port '" +
               description_.name + "'");
      return;
    }
    pending_input_ = std::move(encoded);
    pending_input_source_.reset();
  }

  const std::vector<::opcua::Variant> inputs{*pending_input_};
  const RemoteCallResult result = session_->callPort(
      description_.object_id, description_.method_id, inputs);
  if (!result.success) {
    setError(result.error);
    return;
  }
  if (result.outputs.size() != 1U) {
    setError("remote input port '" + description_.name +
             "' returned an invalid response");
    return;
  }
  const std::string status = result.outputs.front().to<std::string>();
  if (status == "WriteSuccess") {
    pending_input_.reset();
    clearError();
  } else if (status == "WriteFailure" || status == "NotConnected") {
    setError("remote input port '" + description_.name + "' returned " +
             status);
  } else {
    setError("remote input port '" + description_.name +
             "' returned invalid status '" + status + "'");
  }
}

void RemotePortAdapter::pumpOutput() {
  auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(port_.get());
  if (output == nullptr || !output->connected()) {
    output_was_connected_ = false;
    return;
  }

  if (pending_output_) {
    const RTT::WriteStatus status = output->write(pending_output_);
    if (status != RTT::WriteSuccess) {
      setError("local mirror for remote output port '" + description_.name +
               "' returned " +
               (status == RTT::NotConnected ? "NotConnected" : "WriteFailure"));
      return;
    }
    pending_output_.reset();
    output_was_connected_ = true;
    clearError();
    return;
  }

  const bool first_connected_poll = !output_was_connected_;
  const RemoteCallResult result =
      session_->callPort(description_.object_id, description_.method_id, {});
  if (!result.success) {
    setError(result.error);
    return;
  }
  if (result.outputs.size() != 2U) {
    setError("remote output port '" + description_.name +
             "' returned an invalid response");
    return;
  }
  const std::string status = result.outputs[0].to<std::string>();
  if (status == "NewData" || (status == "OldData" && first_connected_poll)) {
    pending_output_ = codec_->makeDataSource(result.outputs[1]);
    if (!pending_output_) {
      setError("failed to decode sample from remote output port '" +
               description_.name + "'");
      return;
    }
    const RTT::WriteStatus write_status = output->write(pending_output_);
    if (write_status != RTT::WriteSuccess) {
      setError("local mirror for remote output port '" + description_.name +
               "' returned " +
               (write_status == RTT::NotConnected ? "NotConnected"
                                                  : "WriteFailure"));
      return;
    }
    pending_output_.reset();
  } else if (status != "OldData" && status != "NoData") {
    setError("remote output port '" + description_.name +
             "' returned invalid status '" + status + "'");
    return;
  }
  output_was_connected_ = true;
  clearError();
}

void RemotePortAdapter::setError(std::string error) {
  const std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_ = std::move(error);
}

void RemotePortAdapter::clearError() {
  const std::lock_guard<std::mutex> lock(error_mutex_);
  last_error_.clear();
}

bool RemotePortAdapter::hasSameIdentity(
    const RemotePortAdapter &other) const noexcept {
  return description_.name == other.description_.name &&
         description_.type_name == other.description_.type_name &&
         description_.service_path == other.description_.service_path &&
         description_.direction == other.description_.direction;
}

} // namespace RTT::opcua::detail
