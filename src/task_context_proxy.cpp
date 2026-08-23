#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include "client_session.hpp"
#include "remote_operation.hpp"
#include "remote_port.hpp"

#include <open62541pp/ua/nodeids.hpp>

#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/PropertyBase.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <algorithm>
#include <array>
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

struct RequiredRootOperation {
  std::string_view name;
  std::string_view output_type;
};

constexpr std::array<RequiredRootOperation, 8U> kRequiredRootOperations{{
    {"getTaskState", "TaskState"},
    {"getTargetState", "TaskState"},
    {"isConfigured", "Bool"},
    {"isActive", "Bool"},
    {"isRunning", "Bool"},
    {"inFatalError", "Bool"},
    {"inException", "Bool"},
    {"inRunTimeError", "Bool"},
}};

void validateRequiredRootOperations(
    const std::vector<detail::RemoteOperationDescription> &operations) {
  for (const RequiredRootOperation &required : kRequiredRootOperations) {
    const auto found = std::find_if(
        operations.begin(), operations.end(), [&](const auto &operation) {
          return operation.name == required.name;
        });
    if (found == operations.end()) {
      throw std::runtime_error("remote RTT root operation '" +
                               std::string(required.name) + "' is required");
    }
    if (!found->input_types.empty() || found->output_types.size() != 1U ||
        found->output_types.front() != required.output_type ||
        found->output_sources.size() != 1U ||
        found->output_sources.front() != -1) {
      throw std::runtime_error(
          "remote RTT root operation '" + std::string(required.name) +
          "' must have no inputs and one return output of RTT type '" +
          std::string(required.output_type) + "'");
    }
  }
}

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
  const auto type_registry = session ? session->typeRegistry() : nullptr;
  const TypeCodec *codec =
      type_registry ? type_registry->codecForTypeInfo(type_info) : nullptr;
  if (type_info == nullptr || codec == nullptr || !codec->hasValue()) {
    throw std::runtime_error("remote value '" + description.name +
                             "' uses unsupported RTT type '" +
                             description.type_name + "'");
  }

  const ::opcua::NodeId node_id = description.node_id;
  VariantReader reader = [session, type_registry,
                          node_id](::opcua::Variant *value) {
    return type_registry && session->readValue(node_id, value);
  };
  VariantWriter writer;
  if (description.writable) {
    writer = [session, type_registry, node_id](const ::opcua::Variant &value) {
      return type_registry && session->writeValue(node_id, value);
    };
  }
  RTT::base::DataSourceBase::shared_ptr source =
      codec->makeProxyDataSource(std::move(reader), std::move(writer));
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
  if (service_path.empty()) {
    validateRequiredRootOperations(operation_descriptions);
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
  Impl(std::string endpoint_url, std::string component_name,
       TaskContextProxyOptions options)
      : session(std::make_shared<detail::ClientSession>(
            std::move(endpoint_url), options.request_timeout)),
        component_name(std::move(component_name)),
        port_poll_interval(options.port_poll_interval),
        port_thread([this](std::stop_token stop) { pumpPorts(stop); }) {}

  ~Impl() { stopPortPump(); }

  template <typename Result, typename... Arguments>
  Result invokeOperation(std::string_view operation_name, Result fallback,
                         Arguments... arguments) {
    const std::lock_guard<std::mutex> lock(synchronize_mutex);
    if (!interface_ready.load()) {
      setControlError("remote RTT interface is not ready");
      return fallback;
    }
    if (interface_stale.load()) {
      setControlError("remote RTT interface is stale; synchronize it before "
                      "calling operations");
      return fallback;
    }

    std::vector<::opcua::Variant> inputs;
    inputs.reserve(sizeof...(Arguments));
    (inputs.emplace_back(arguments), ...);
    detail::RemoteCallResult result = session->callOperation(
        component_name, {}, std::string(operation_name), inputs);
    if (!result.success) {
      markInterfaceStale(result.error.empty()
                             ? "remote RTT operation call failed"
                             : std::move(result.error));
      return fallback;
    }
    if (result.outputs.size() != 1U) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned an invalid result count");
      return fallback;
    }
    try {
      Result value = result.outputs.front().template to<Result>();
      clearControlError();
      return value;
    } catch (const std::exception &exception) {
      markInterfaceStale(
          "remote RTT operation '" + std::string(operation_name) +
          "' returned an incompatible result: " + exception.what());
    } catch (...) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned an incompatible result");
    }
    return fallback;
  }

  void invokeVoidOperation(std::string_view operation_name) {
    const std::lock_guard<std::mutex> lock(synchronize_mutex);
    if (!interface_ready.load()) {
      setControlError("remote RTT interface is not ready");
      return;
    }
    if (interface_stale.load()) {
      setControlError("remote RTT interface is stale; synchronize it before "
                      "calling operations");
      return;
    }

    detail::RemoteCallResult result = session->callOperation(
        component_name, {}, std::string(operation_name), {});
    if (!result.success) {
      markInterfaceStale(result.error.empty()
                             ? "remote RTT operation call failed"
                             : std::move(result.error));
      return;
    }
    if (!result.outputs.empty()) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned unexpected results");
      return;
    }
    clearControlError();
  }

  RTT::base::TaskCore::TaskState
  invokeTaskStateOperation(std::string_view operation_name) {
    const std::lock_guard<std::mutex> lock(synchronize_mutex);
    if (!interface_ready.load()) {
      setControlError("remote RTT interface is not ready");
      return RTT::base::TaskCore::Init;
    }
    if (interface_stale.load()) {
      return RTT::base::TaskCore::Init;
    }

    detail::RemoteCallResult result = session->callOperation(
        component_name, {}, std::string(operation_name), {});
    if (!result.success) {
      markInterfaceStale(result.error.empty()
                             ? "remote RTT task state operation failed"
                             : std::move(result.error));
      return RTT::base::TaskCore::Init;
    }
    if (result.outputs.size() != 1U) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned an invalid result count");
      return RTT::base::TaskCore::Init;
    }
    const ::opcua::Variant &output = result.outputs.front();
    if (!output.isScalar() ||
        !output.isType(::opcua::NodeId(::opcua::DataTypeId::Int32))) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' did not return a scalar Int32 TaskState");
      return RTT::base::TaskCore::Init;
    }
    try {
      const std::int32_t code = output.to<std::int32_t>();
      if (code < static_cast<std::int32_t>(RTT::base::TaskCore::Init) ||
          code > static_cast<std::int32_t>(
                     RTT::base::TaskCore::RunTimeError)) {
        markInterfaceStale("remote RTT operation '" +
                           std::string(operation_name) +
                           "' returned invalid TaskState code " +
                           std::to_string(code));
        return RTT::base::TaskCore::Init;
      }
      clearControlError();
      return static_cast<RTT::base::TaskCore::TaskState>(code);
    } catch (const std::exception &exception) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned an incompatible TaskState: " +
                         exception.what());
    } catch (...) {
      markInterfaceStale("remote RTT operation '" +
                         std::string(operation_name) +
                         "' returned an incompatible TaskState");
    }
    return RTT::base::TaskCore::Init;
  }

  std::string lastControlError() const {
    const std::lock_guard<std::mutex> lock(control_error_mutex);
    return control_error;
  }

  ProxyConnectionState connectionState() const noexcept {
    const ProxyConnectionState session_state = session->state();
    return interface_stale.load() &&
                   session_state == ProxyConnectionState::connected
               ? ProxyConnectionState::stale
               : session_state;
  }

  void markInterfaceStale(std::string error) {
    interface_stale.store(true);
    setControlError(std::move(error));
    session->setInterfaceAccessEnabled(false);
    pausePortPump();
    discardPendingPortState();
  }

  void markInterfaceFresh() {
    session->setInterfaceAccessEnabled(true);
    interface_stale.store(false);
    clearControlError();
  }

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

  void
  quarantinePortAdapters(std::vector<std::shared_ptr<detail::RemotePortAdapter>>
                             adapters) noexcept {
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
    }
    previous.clear();
  }

  void resumePortPump() {
    {
      const std::lock_guard<std::mutex> lock(port_mutex);
      if (interface_stale.load()) {
        return;
      }
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
    {
      const std::lock_guard<std::mutex> lock(port_mutex);
      port_thread.request_stop();
    }
    port_condition.notify_all();
    port_thread.join();
  }

  std::shared_ptr<detail::ClientSession> session;
  const std::string component_name;
  mutable std::mutex synchronize_mutex;
  std::atomic<bool> interface_ready{false};
  std::atomic<bool> interface_stale{false};

private:
  void discardPendingPortState() noexcept {
    std::vector<std::shared_ptr<detail::RemotePortAdapter>> adapters;
    try {
      const std::lock_guard<std::mutex> lock(port_mutex);
      adapters = port_adapters;
    } catch (...) {
      return;
    }
    for (const auto &adapter : adapters) {
      adapter->discardPendingState();
    }
  }

  void setControlError(std::string message) const {
    const std::lock_guard<std::mutex> lock(control_error_mutex);
    control_error = std::move(message);
  }

  void clearControlError() const {
    const std::lock_guard<std::mutex> lock(control_error_mutex);
    control_error.clear();
  }

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
  mutable std::mutex control_error_mutex;
  mutable std::string control_error;
};

TaskContextProxy::TaskContextProxy(std::string endpoint_url,
                                   std::string component_name,
                                   TaskContextProxyOptions options)
    : RTT::TaskContext(component_name),
      impl_(std::make_unique<Impl>(std::move(endpoint_url), component_name,
                                   options)) {
  clear();
}

TaskContextProxy::~TaskContextProxy() {
  if (!impl_) {
    return;
  }
  impl_->interface_ready.store(false);
  try {
    impl_->session->setInterfaceAccessEnabled(false);
  } catch (...) {
  }
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
  impl_->session->setInterfaceAccessEnabled(false);
  impl_->pausePortPump();
  ScopeExit resume_pump([this] { impl_->resumePortPump(); });
  const ProxyConnectionState entry_session_state = impl_->session->state();
  if (impl_->interface_ready.load() &&
      entry_session_state != ProxyConnectionState::connected &&
      !impl_->interface_stale.load()) {
    std::string stale_error = impl_->session->lastError();
    if (stale_error.empty()) {
      stale_error = "remote RTT interface became stale before synchronization";
    }
    impl_->markInterfaceStale(std::move(stale_error));
  }
  const bool can_restore_existing_interface =
      impl_->interface_ready.load() && !impl_->interface_stale.load() &&
      entry_session_state == ProxyConnectionState::connected;
  if (entry_session_state != ProxyConnectionState::connected) {
    if (!impl_->session->connect(error)) {
      impl_->markInterfaceStale(error == nullptr ? impl_->session->lastError()
                                                 : *error);
      return false;
    }
  }
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
    const std::string message =
        std::string("failed to stage remote RTT interface: ") +
        exception.what();
    impl_->markInterfaceStale(message);
    assignError(error, message);
    return false;
  }

  const RTT::Service::shared_ptr root = provides();
  std::vector<std::shared_ptr<detail::RemotePortAdapter>> replacement_ports;
  collectStagedPorts(staged, &replacement_ports);
  std::string replacement_error;
  if (!impl_->canReplacePortAdapters(replacement_ports, &replacement_error)) {
    if (can_restore_existing_interface &&
        impl_->session->state() == ProxyConnectionState::connected) {
      impl_->session->setInterfaceAccessEnabled(true);
    }
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
    const std::string message =
        std::string("failed to install remote RTT interface: ") +
        exception.what() + cleanup_error;
    impl_->markInterfaceStale(message);
    assignError(error, message);
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
    const std::string message =
        "failed to install remote RTT interface: unknown error";
    impl_->markInterfaceStale(message);
    assignError(error, message);
    return false;
  }
  impl_->replacePortAdapters(std::move(installed_ports));
  impl_->interface_ready.store(true);
  impl_->markInterfaceFresh();
  impl_->resumePortPump();
  resume_pump.release();
  assignError(error, "");
  return true;
}

bool TaskContextProxy::ready() {
  if (impl_ == nullptr || !impl_->interface_ready.load()) {
    return false;
  }
  static_cast<void>(impl_->invokeTaskStateOperation("getTaskState"));
  return impl_->connectionState() == ProxyConnectionState::connected;
}

bool TaskContextProxy::configure() {
  return impl_ && impl_->invokeOperation<bool>("configure", false);
}

bool TaskContextProxy::activate() {
  return impl_ && impl_->invokeOperation<bool>("activate", false);
}

bool TaskContextProxy::start() {
  return impl_ && impl_->invokeOperation<bool>("start", false);
}

bool TaskContextProxy::stop() {
  return impl_ && impl_->invokeOperation<bool>("stop", false);
}

bool TaskContextProxy::cleanup() {
  return impl_ && impl_->invokeOperation<bool>("cleanup", false);
}

bool TaskContextProxy::recover() {
  return impl_ && impl_->invokeOperation<bool>("recover", false);
}

bool TaskContextProxy::isConfigured() const {
  return impl_ && impl_->invokeOperation<bool>("isConfigured", false);
}

bool TaskContextProxy::isActive() const {
  return impl_ && impl_->invokeOperation<bool>("isActive", false);
}

bool TaskContextProxy::isRunning() const {
  return impl_ && impl_->invokeOperation<bool>("isRunning", false);
}

bool TaskContextProxy::inFatalError() const {
  return impl_ && impl_->invokeOperation<bool>("inFatalError", false);
}

bool TaskContextProxy::inException() const {
  return impl_ && impl_->invokeOperation<bool>("inException", false);
}

bool TaskContextProxy::inRunTimeError() const {
  return impl_ && impl_->invokeOperation<bool>("inRunTimeError", false);
}

TaskContextProxy::TaskState TaskContextProxy::getTaskState() const {
  return impl_ ? impl_->invokeTaskStateOperation("getTaskState")
               : RTT::base::TaskCore::Init;
}

TaskContextProxy::TaskState TaskContextProxy::getTargetState() const {
  return impl_ ? impl_->invokeTaskStateOperation("getTargetState")
               : RTT::base::TaskCore::Init;
}

Seconds TaskContextProxy::getPeriod() const {
  return impl_ ? impl_->invokeOperation<Seconds>("getPeriod", -1.0) : -1.0;
}

bool TaskContextProxy::setPeriod(Seconds period) {
  return impl_ && impl_->invokeOperation<bool>("setPeriod", false, period);
}

unsigned TaskContextProxy::getCpuAffinity() const {
  return impl_ ? impl_->invokeOperation<unsigned>("getCpuAffinity", ~0U) : ~0U;
}

bool TaskContextProxy::setCpuAffinity(unsigned cpu) {
  return impl_ && impl_->invokeOperation<bool>("setCpuAffinity", false, cpu);
}

bool TaskContextProxy::update() {
  return impl_ && impl_->invokeOperation<bool>("update", false);
}

bool TaskContextProxy::trigger() {
  return impl_ && impl_->invokeOperation<bool>("trigger", false);
}

void TaskContextProxy::error() {
  if (impl_) {
    impl_->invokeVoidOperation("error");
  }
}

ProxyConnectionState TaskContextProxy::connectionState() const noexcept {
  return impl_ ? impl_->connectionState() : ProxyConnectionState::disconnected;
}

std::string TaskContextProxy::endpointUrl() const {
  return impl_ ? impl_->session->endpointUrl() : std::string{};
}

std::string TaskContextProxy::lastError() const {
  if (!impl_) {
    return {};
  }
  std::string control_error = impl_->lastControlError();
  if (!control_error.empty()) {
    return control_error;
  }
  std::string port_error = impl_->lastPortError();
  return port_error.empty() ? impl_->session->lastError()
                            : std::move(port_error);
}

} // namespace RTT::opcua
