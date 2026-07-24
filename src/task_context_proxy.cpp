#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include "client_session.hpp"
#include "remote_operation.hpp"
#include "remote_port.hpp"

#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/PropertyBase.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RTT::opcua {
namespace {

void assignError(std::string *output, const std::string &message) {
  if (output != nullptr) {
    *output = message;
  }
}

constexpr std::size_t kMaximumServiceDepth = 32U;

template <typename Callback> class ScopeExit final {
public:
  explicit ScopeExit(Callback callback) : callback_(std::move(callback)) {}
  ~ScopeExit() {
    if (active_) {
      callback_();
    }
  }

  ScopeExit(const ScopeExit &) = delete;
  ScopeExit &operator=(const ScopeExit &) = delete;

  void release() noexcept { active_ = false; }

private:
  Callback callback_;
  bool active_{true};
};

using OperationOwner = std::unique_ptr<RTT::OperationInterfacePart>;
using PropertyOwner = std::unique_ptr<RTT::base::PropertyBase>;
using AttributeOwner = std::unique_ptr<RTT::base::AttributeBase>;

struct StagedService {
  std::string name;
  std::string description;
  std::vector<std::pair<std::string, OperationOwner>> operations;
  std::vector<PropertyOwner> properties;
  std::vector<AttributeOwner> attributes;
  std::vector<std::shared_ptr<detail::RemotePortAdapter>> ports;
  std::vector<std::unique_ptr<StagedService>> children;
};

void collectStagedPorts(
    const StagedService &staged,
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> *ports) {
  ports->insert(ports->end(), staged.ports.begin(), staged.ports.end());
  for (const auto &child : staged.children) {
    collectStagedPorts(*child, ports);
  }
}

std::pair<RTT::types::TypeInfo *, RTT::base::DataSourceBase::shared_ptr>
makeRemoteDataSource(const std::shared_ptr<detail::ClientSession> &session,
                     const detail::RemoteValueDescription &description) {
  RTT::types::TypeInfo *type_info =
      RTT::types::Types()->type(description.type_name);
  const TypeProtocol *protocol = protocolForTypeInfo(type_info);
  if (type_info == nullptr || protocol == nullptr || !protocol->hasValue()) {
    throw std::runtime_error("remote value '" + description.name +
                             "' uses unsupported RTT type '" +
                             description.type_name + "'");
  }

  const ::opcua::NodeId node_id = description.node_id;
  TypeProtocol::VariantReader reader = [session,
                                        node_id](::opcua::Variant *value) {
    return session->readValue(node_id, value);
  };
  TypeProtocol::VariantWriter writer;
  if (description.writable) {
    writer = [session, node_id](const ::opcua::Variant &value) {
      return session->writeValue(node_id, value);
    };
  }
  RTT::base::DataSourceBase::shared_ptr source =
      protocol->makeProxyDataSource(std::move(reader), std::move(writer));
  if (!source) {
    throw std::runtime_error("failed to build remote data source for '" +
                             description.name + "'");
  }
  return {type_info, std::move(source)};
}

StagedService
stageRemoteService(const std::shared_ptr<detail::ClientSession> &session,
                   const std::string &component_name,
                   const detail::RemoteServicePath &service_path,
                   std::string service_name, std::string service_description) {
  if (service_path.size() > kMaximumServiceDepth) {
    throw std::runtime_error("remote RTT service hierarchy exceeds the "
                             "supported depth");
  }

  std::string discovery_error;
  std::vector<detail::RemoteOperationDescription> operation_descriptions =
      session->discoverOperations(component_name, service_path,
                                  &discovery_error);
  if (!discovery_error.empty()) {
    throw std::runtime_error(discovery_error);
  }
  std::vector<detail::RemoteValueDescription> property_descriptions =
      session->discoverValues(component_name, service_path,
                              detail::RemoteValueCategory::properties,
                              &discovery_error);
  if (!discovery_error.empty()) {
    throw std::runtime_error(discovery_error);
  }
  std::vector<detail::RemoteValueDescription> attribute_descriptions =
      session->discoverValues(component_name, service_path,
                              detail::RemoteValueCategory::attributes,
                              &discovery_error);
  if (!discovery_error.empty()) {
    throw std::runtime_error(discovery_error);
  }
  std::vector<detail::RemotePortDescription> port_descriptions =
      session->discoverPorts(component_name, service_path, &discovery_error);
  if (!discovery_error.empty()) {
    throw std::runtime_error(discovery_error);
  }
  std::vector<detail::RemoteServiceDescription> service_descriptions =
      session->discoverServices(component_name, service_path, &discovery_error);
  if (!discovery_error.empty()) {
    throw std::runtime_error(discovery_error);
  }

  StagedService staged;
  staged.name = std::move(service_name);
  staged.description = std::move(service_description);
  staged.operations.reserve(operation_descriptions.size());
  staged.properties.reserve(property_descriptions.size());
  staged.attributes.reserve(attribute_descriptions.size());
  staged.ports.reserve(port_descriptions.size());
  staged.children.reserve(service_descriptions.size());

  for (auto &description : operation_descriptions) {
    const std::string name = description.name;
    staged.operations.emplace_back(name,
                                   OperationOwner(detail::makeRemoteOperation(
                                       session, std::move(description))));
  }

  for (const detail::RemoteValueDescription &description :
       property_descriptions) {
    if (!description.writable) {
      throw std::runtime_error("remote property '" + description.name +
                               "' is not writable for the current OPC UA "
                               "user");
    }
    auto [type_info, source] = makeRemoteDataSource(session, description);
    PropertyOwner property(type_info->buildProperty(
        description.name, description.description, source));
    if (!property || property->getDataSource().get() != source.get()) {
      throw std::runtime_error("failed to construct remote property '" +
                               description.name + "'");
    }
    staged.properties.push_back(std::move(property));
  }

  for (const detail::RemoteValueDescription &description :
       attribute_descriptions) {
    auto [type_info, source] = makeRemoteDataSource(session, description);
    AttributeOwner attribute(
        description.writable
            ? type_info->buildAttribute(description.name, source)
            : type_info->buildAlias(description.name, source));
    if (!attribute || attribute->getDataSource().get() != source.get()) {
      throw std::runtime_error("failed to construct remote attribute '" +
                               description.name + "'");
    }
    staged.attributes.push_back(std::move(attribute));
  }

  for (detail::RemotePortDescription &description : port_descriptions) {
    const std::string port_name = description.name;
    std::shared_ptr<detail::RemotePortAdapter> port =
        detail::RemotePortAdapter::create(session, std::move(description),
                                          &discovery_error);
    if (!port) {
      throw std::runtime_error(discovery_error.empty()
                                   ? "failed to construct remote port '" +
                                         port_name + "'"
                                   : discovery_error);
    }
    staged.ports.push_back(std::move(port));
  }

  for (detail::RemoteServiceDescription &description : service_descriptions) {
    detail::RemoteServicePath child_path = service_path;
    child_path.push_back(description.name);
    staged.children.push_back(std::make_unique<StagedService>(
        stageRemoteService(session, component_name, child_path,
                           std::move(description.name),
                           std::move(description.description))));
  }
  return staged;
}

void installStagedService(
    const RTT::Service::shared_ptr &target, StagedService &staged,
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> *installed_ports) {
  target->doc(staged.description);
  for (auto &[name, operation] : staged.operations) {
    target->add(name, operation.release());
  }
  for (auto &property : staged.properties) {
    if (!target->properties()->ownProperty(property.get())) {
      throw std::logic_error("failed to install staged RTT property");
    }
    property.release();
  }
  for (auto &attribute : staged.attributes) {
    if (!target->setValue(attribute.get())) {
      throw std::logic_error("failed to install staged RTT attribute");
    }
    attribute.release();
  }
  for (const auto &port : staged.ports) {
    target->addLocalPort(port->port());
    installed_ports->push_back(port);
  }
  for (auto &child_staged : staged.children) {
    RTT::Service::shared_ptr child = RTT::Service::Create(child_staged->name);
    if (!target->addService(child)) {
      throw std::logic_error("failed to install staged RTT service '" +
                             child_staged->name + "'");
    }
    installStagedService(child, *child_staged, installed_ports);
  }
}

void clearMirroredInterface(const RTT::Service::shared_ptr &service) {
  for (const std::string &name : service->getPortNames()) {
    service->removeLocalPort(name);
  }
  for (const std::string &name : service->getProviderNames()) {
    if (name == "this") {
      continue;
    }
    RTT::Service::shared_ptr child = service->getService(name);
    if (!child) {
      continue;
    }
    clearMirroredInterface(child);
    child->clear();
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
            std::move(endpoint_url), options.request_timeout)),
        port_poll_interval(options.port_poll_interval),
        port_thread([this](std::stop_token stop) { pumpPorts(stop); }) {}

  ~Impl() { stopPortPump(); }

  void pausePortPump() {
    std::unique_lock<std::mutex> lock(port_mutex);
    ports_paused = true;
    port_condition.notify_all();
    port_condition.wait(lock, [this] { return !ports_pumping; });
  }

  void clearPortAdapters() noexcept {
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> previous;
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> quarantined;
    try {
      const std::lock_guard<std::mutex> lock(port_mutex);
      previous = std::move(port_adapters);
      quarantined = std::move(quarantined_port_adapters);
    } catch (...) {
    }
  }

  void quarantinePortAdapters(
      std::vector<std::shared_ptr<detail::RemotePortAdapter>> adapters) noexcept {
    try {
      const std::lock_guard<std::mutex> lock(port_mutex);
      if (quarantined_port_adapters.empty()) {
        quarantined_port_adapters.swap(adapters);
      } else {
        quarantined_port_adapters.insert(
            quarantined_port_adapters.end(),
            std::make_move_iterator(adapters.begin()),
            std::make_move_iterator(adapters.end()));
      }
    } catch (...) {
    }
  }

  void clearQuarantinedPortAdapters() noexcept {
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> previous;
    try {
      const std::lock_guard<std::mutex> lock(port_mutex);
      previous = std::move(quarantined_port_adapters);
    } catch (...) {
    }
  }

  bool canReplacePortAdapters(
      const std::vector<std::shared_ptr<detail::RemotePortAdapter>>
          &replacements,
      std::string *error) const {
    const std::lock_guard<std::mutex> lock(port_mutex);
    for (const auto &adapter : port_adapters) {
      if (!adapter->hasPendingState()) {
        continue;
      }
      const bool has_destination =
          std::any_of(replacements.begin(), replacements.end(),
                      [&](const auto &replacement) {
                        return adapter->canTransferStateTo(*replacement);
                      });
      if (!has_destination) {
        assignError(error, "cannot replace remote port '" + adapter->name() +
                               "' while it has an unacknowledged sample");
        return false;
      }
    }
    assignError(error, "");
    return true;
  }

  void replacePortAdapters(
      std::vector<std::shared_ptr<detail::RemotePortAdapter>> adapters) {
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> previous;
    {
      const std::lock_guard<std::mutex> lock(port_mutex);
      for (const auto &old_adapter : port_adapters) {
        for (const auto &new_adapter : adapters) {
          if (old_adapter->transferPendingStateTo(*new_adapter)) {
            break;
          }
        }
      }
      previous = std::move(port_adapters);
      port_adapters = std::move(adapters);
      ports_paused = false;
    }
    previous.clear();
    port_condition.notify_all();
  }

  void resumePortPump() {
    {
      const std::lock_guard<std::mutex> lock(port_mutex);
      ports_paused = false;
    }
    port_condition.notify_all();
  }

  std::string lastPortError() const {
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> adapters;
    {
      const std::lock_guard<std::mutex> lock(port_mutex);
      adapters = port_adapters;
    }
    for (const auto &adapter : adapters) {
      std::string error = adapter->lastError();
      if (!error.empty()) {
        return error;
      }
    }
    return {};
  }

  void stopPortPump() {
    if (!port_thread.joinable()) {
      return;
    }
    port_thread.request_stop();
    port_condition.notify_all();
    port_thread.join();
  }

  std::shared_ptr<detail::ClientSession> session;
  std::mutex synchronize_mutex;
  std::atomic<bool> interface_ready{false};

private:
  void pumpPorts(std::stop_token stop) {
    std::unique_lock<std::mutex> lock(port_mutex);
    while (!stop.stop_requested()) {
      port_condition.wait(
          lock, [&] { return stop.stop_requested() || !ports_paused; });
      if (stop.stop_requested()) {
        break;
      }

      const auto adapters = port_adapters;
      ports_pumping = true;
      lock.unlock();
      for (const auto &adapter : adapters) {
        if (stop.stop_requested()) {
          break;
        }
        adapter->pump();
      }
      lock.lock();
      ports_pumping = false;
      port_condition.notify_all();
      port_condition.wait_for(lock, port_poll_interval, [&] {
        return stop.stop_requested() || ports_paused;
      });
    }
    ports_pumping = false;
    port_condition.notify_all();
  }

  const std::chrono::milliseconds port_poll_interval;
  mutable std::mutex port_mutex;
  std::condition_variable port_condition;
  std::vector<std::shared_ptr<detail::RemotePortAdapter>> port_adapters;
  std::vector<std::shared_ptr<detail::RemotePortAdapter>>
      quarantined_port_adapters;
  bool ports_paused{true};
  bool ports_pumping{false};
  std::jthread port_thread;
};

TaskContextProxy::TaskContextProxy(std::string endpoint_url,
                                   std::string component_name,
                                   TaskContextProxyOptions options)
    : RTT::TaskContext(component_name),
      impl_(std::make_unique<Impl>(std::move(endpoint_url), options)) {
  clear();
}

TaskContextProxy::~TaskContextProxy() {
  if (!impl_) {
    return;
  }
  impl_->interface_ready.store(false);
  impl_->stopPortPump();
  try {
    clearMirroredInterface(provides());
    clear();
    impl_->clearQuarantinedPortAdapters();
  } catch (...) {
    // Registered ports are deliberately retained by RemotePortAdapter's
    // teardown fallback until the owning RTT service is destroyed.
  }
  impl_->clearPortAdapters();
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
  if (options.port_poll_interval <= std::chrono::milliseconds::zero() ||
      options.port_poll_interval.count() >
          static_cast<std::int64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    assignError(error,
                "OPC UA port poll interval is outside the supported range");
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
  const std::lock_guard<std::mutex> synchronize_lock(impl_->synchronize_mutex);
  StagedService staged;
  try {
    std::string root_description;
    std::string discovery_error;
    if (!impl_->session->readServiceDescription(
            getName(), {}, &root_description, &discovery_error)) {
      throw std::runtime_error(discovery_error);
    }
    staged = stageRemoteService(impl_->session, getName(), {}, {},
                                std::move(root_description));
  } catch (const std::exception &exception) {
    assignError(error, std::string("failed to stage remote RTT interface: ") +
                           exception.what());
    return false;
  }

  const RTT::Service::shared_ptr root = provides();
  impl_->pausePortPump();
  ScopeExit resume_pump([this] { impl_->resumePortPump(); });
  std::vector<std::shared_ptr<detail::RemotePortAdapter>> replacement_ports;
  collectStagedPorts(staged, &replacement_ports);
  std::string replacement_error;
  if (!impl_->canReplacePortAdapters(replacement_ports, &replacement_error)) {
    assignError(error, replacement_error);
    return false;
  }

  std::vector<std::shared_ptr<detail::RemotePortAdapter>> installed_ports;
  try {
    impl_->interface_ready.store(false);
    clearMirroredInterface(root);
    clear();
    impl_->clearQuarantinedPortAdapters();
    installStagedService(root, staged, &installed_ports);
  } catch (const std::exception &exception) {
    std::string cleanup_error;
    try {
      clearMirroredInterface(root);
      clear();
    } catch (const std::exception &cleanup_exception) {
      cleanup_error =
          std::string("; cleanup also failed: ") + cleanup_exception.what();
    } catch (...) {
      cleanup_error = "; cleanup also failed with an unknown error";
    }
    if (!cleanup_error.empty()) {
      impl_->quarantinePortAdapters(std::move(replacement_ports));
    }
    installed_ports.clear();
    assignError(error, std::string("failed to install remote RTT interface: ") +
                           exception.what() + cleanup_error);
    return false;
  } catch (...) {
    bool cleanup_failed = false;
    try {
      clearMirroredInterface(root);
      clear();
    } catch (...) {
      cleanup_failed = true;
    }
    if (cleanup_failed) {
      impl_->quarantinePortAdapters(std::move(replacement_ports));
    }
    installed_ports.clear();
    assignError(error, "failed to install remote RTT interface: unknown error");
    return false;
  }
  impl_->replacePortAdapters(std::move(installed_ports));
  resume_pump.release();
  impl_->interface_ready.store(true);
  assignError(error, "");
  return true;
}

bool TaskContextProxy::ready() {
  return impl_ != nullptr && impl_->interface_ready.load() &&
         impl_->session->state() == ProxyConnectionState::connected;
}

ProxyConnectionState TaskContextProxy::connectionState() const noexcept {
  return impl_ ? impl_->session->state() : ProxyConnectionState::disconnected;
}

std::string TaskContextProxy::endpointUrl() const {
  return impl_ ? impl_->session->endpointUrl() : std::string{};
}

std::string TaskContextProxy::lastError() const {
  if (!impl_) {
    return {};
  }
  std::string port_error = impl_->lastPortError();
  return port_error.empty() ? impl_->session->lastError()
                            : std::move(port_error);
}

} // namespace RTT::opcua
