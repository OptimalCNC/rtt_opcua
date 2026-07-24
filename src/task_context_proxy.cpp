#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include "client_session.hpp"
#include "remote_operation.hpp"

#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/PropertyBase.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstddef>
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

constexpr std::size_t kMaximumServiceDepth = 32U;

using OperationOwner = std::unique_ptr<RTT::OperationInterfacePart>;
using PropertyOwner = std::unique_ptr<RTT::base::PropertyBase>;
using AttributeOwner = std::unique_ptr<RTT::base::AttributeBase>;

struct StagedService {
  std::string name;
  std::string description;
  std::vector<std::pair<std::string, OperationOwner>> operations;
  std::vector<PropertyOwner> properties;
  std::vector<AttributeOwner> attributes;
  std::vector<std::unique_ptr<StagedService>> children;
};

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

void installStagedService(const RTT::Service::shared_ptr &target,
                          StagedService &staged) {
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
  for (auto &child_staged : staged.children) {
    RTT::Service::shared_ptr child = RTT::Service::Create(child_staged->name);
    installStagedService(child, *child_staged);
    if (!target->addService(child)) {
      throw std::logic_error("failed to install staged RTT service '" +
                             child_staged->name + "'");
    }
  }
}

void clearNestedServices(const RTT::Service::shared_ptr &service) {
  for (const std::string &name : service->getProviderNames()) {
    if (name == "this") {
      continue;
    }
    RTT::Service::shared_ptr child = service->getService(name);
    if (!child) {
      continue;
    }
    clearNestedServices(child);
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
  clearNestedServices(provides());
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
  clearNestedServices(root);
  clear();
  try {
    installStagedService(root, staged);
  } catch (const std::exception &exception) {
    clearNestedServices(root);
    clear();
    assignError(error, std::string("failed to install remote RTT interface: ") +
                           exception.what());
    return false;
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
