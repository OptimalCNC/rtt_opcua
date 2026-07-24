#include <rtt/opcua/task_context_proxy.hpp>

#include "client_session.hpp"
#include "remote_operation.hpp"

#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace RTT::opcua {
namespace {

void assignError(std::string *output, const std::string &message) {
  if (output != nullptr) {
    *output = message;
  }
}

} // namespace

std::string_view toString(ProxyConnectionState state) noexcept {
  switch (state) {
  case ProxyConnectionState::disconnected:
    return "disconnected";
  case ProxyConnectionState::connecting:
    return "connecting";
  case ProxyConnectionState::connected:
    return "connected";
  case ProxyConnectionState::stale:
    return "stale";
  }
  return "unknown";
}

std::ostream &operator<<(std::ostream &stream, ProxyConnectionState state) {
  return stream << toString(state);
}

class TaskContextProxy::Impl final {
public:
  Impl(std::string endpoint_url, TaskContextProxyOptions options)
      : session(std::make_shared<detail::ClientSession>(
            std::move(endpoint_url), options.request_timeout)) {}

  std::shared_ptr<detail::ClientSession> session;
};

TaskContextProxy::TaskContextProxy(std::string endpoint_url,
                                   std::string component_name,
                                   TaskContextProxyOptions options)
    : RTT::TaskContext(component_name),
      impl_(std::make_unique<Impl>(std::move(endpoint_url), options)) {
  clear();
}

TaskContextProxy::~TaskContextProxy() {
  clear();
  impl_.reset();
}

std::unique_ptr<TaskContextProxy>
TaskContextProxy::create(std::string endpoint_url, std::string component_name,
                         TaskContextProxyOptions options, std::string *error) {
  if (endpoint_url.empty()) {
    assignError(error, "OPC UA endpoint URL must not be empty");
    return {};
  }
  if (component_name.empty()) {
    assignError(error, "remote RTT component name must not be empty");
    return {};
  }
  if (options.request_timeout <= std::chrono::milliseconds::zero() ||
      options.request_timeout.count() >
          static_cast<std::int64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    assignError(error, "OPC UA request timeout is outside the supported range");
    return {};
  }

  try {
    std::unique_ptr<TaskContextProxy> proxy(new TaskContextProxy(
        std::move(endpoint_url), std::move(component_name), options));
    if (!proxy->impl_->session->connect(error) || !proxy->synchronize(error)) {
      return {};
    }
    return proxy;
  } catch (const std::exception &exception) {
    assignError(error, std::string("failed to create RTT OPC UA proxy: ") +
                           exception.what());
    return {};
  }
}

bool TaskContextProxy::synchronize(std::string *error) {
  std::string discovery_error;
  std::vector<detail::RemoteOperationDescription> descriptions =
      impl_->session->discoverOperations(getName(), &discovery_error);
  if (!discovery_error.empty()) {
    assignError(error, discovery_error);
    return false;
  }

  using OperationOwner = std::unique_ptr<RTT::OperationInterfacePart>;
  std::vector<std::pair<std::string, OperationOwner>> operations;
  operations.reserve(descriptions.size());
  try {
    for (auto &description : descriptions) {
      const std::string name = description.name;
      operations.emplace_back(name,
                              OperationOwner(detail::makeRemoteOperation(
                                  impl_->session, std::move(description))));
    }
  } catch (const std::exception &exception) {
    assignError(error,
                std::string("failed to construct remote RTT interface: ") +
                    exception.what());
    return false;
  }

  clear();
  const RTT::Service::shared_ptr root = provides();
  for (auto &[name, operation] : operations) {
    root->add(name, operation.release());
  }
  assignError(error, "");
  return true;
}

bool TaskContextProxy::ready() {
  return impl_ != nullptr &&
         impl_->session->state() == ProxyConnectionState::connected;
}

ProxyConnectionState TaskContextProxy::connectionState() const noexcept {
  return impl_ ? impl_->session->state() : ProxyConnectionState::disconnected;
}

std::string TaskContextProxy::endpointUrl() const {
  return impl_ ? impl_->session->endpointUrl() : std::string{};
}

std::string TaskContextProxy::lastError() const {
  return impl_ ? impl_->session->lastError() : std::string{};
}

} // namespace RTT::opcua
