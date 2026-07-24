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
  std::vector<detail::RemoteOperationDescription> operation_descriptions =
      impl_->session->discoverOperations(getName(), &discovery_error);
  if (!discovery_error.empty()) {
    assignError(error, discovery_error);
    return false;
  }
  std::vector<detail::RemoteValueDescription> property_descriptions =
      impl_->session->discoverValues(
          getName(), detail::RemoteValueCategory::properties, &discovery_error);
  if (!discovery_error.empty()) {
    assignError(error, discovery_error);
    return false;
  }
  std::vector<detail::RemoteValueDescription> attribute_descriptions =
      impl_->session->discoverValues(
          getName(), detail::RemoteValueCategory::attributes, &discovery_error);
  if (!discovery_error.empty()) {
    assignError(error, discovery_error);
    return false;
  }

  using OperationOwner = std::unique_ptr<RTT::OperationInterfacePart>;
  std::vector<std::pair<std::string, OperationOwner>> operations;
  using PropertyOwner = std::unique_ptr<RTT::base::PropertyBase>;
  std::vector<PropertyOwner> properties;
  using AttributeOwner = std::unique_ptr<RTT::base::AttributeBase>;
  std::vector<AttributeOwner> attributes;
  operations.reserve(operation_descriptions.size());
  properties.reserve(property_descriptions.size());
  attributes.reserve(attribute_descriptions.size());
  try {
    for (auto &description : operation_descriptions) {
      const std::string name = description.name;
      operations.emplace_back(name,
                              OperationOwner(detail::makeRemoteOperation(
                                  impl_->session, std::move(description))));
    }

    const auto make_data_source =
        [session = impl_->session](
            const detail::RemoteValueDescription &description) {
          RTT::types::TypeInfo *type_info =
              RTT::types::Types()->type(description.type_name);
          const TypeProtocol *protocol = protocolForTypeInfo(type_info);
          if (type_info == nullptr || protocol == nullptr ||
              !protocol->hasValue()) {
            throw std::runtime_error("remote value '" + description.name +
                                     "' uses unsupported RTT type '" +
                                     description.type_name + "'");
          }

          const ::opcua::NodeId node_id = description.node_id;
          TypeProtocol::VariantReader reader =
              [session, node_id](::opcua::Variant *value) {
                return session->readValue(node_id, value);
              };
          TypeProtocol::VariantWriter writer;
          if (description.writable) {
            writer = [session, node_id](const ::opcua::Variant &value) {
              return session->writeValue(node_id, value);
            };
          }
          RTT::base::DataSourceBase::shared_ptr source =
              protocol->makeProxyDataSource(std::move(reader),
                                            std::move(writer));
          if (!source) {
            throw std::runtime_error(
                "failed to build remote data source for '" + description.name +
                "'");
          }
          return std::pair{type_info, std::move(source)};
        };

    for (const detail::RemoteValueDescription &description :
         property_descriptions) {
      if (!description.writable) {
        throw std::runtime_error("remote property '" + description.name +
                                 "' is not writable for the current OPC UA "
                                 "user");
      }
      auto [type_info, source] = make_data_source(description);
      PropertyOwner property(type_info->buildProperty(
          description.name, description.description, source));
      if (!property || property->getDataSource().get() != source.get()) {
        throw std::runtime_error("failed to construct remote property '" +
                                 description.name + "'");
      }
      properties.push_back(std::move(property));
    }

    for (const detail::RemoteValueDescription &description :
         attribute_descriptions) {
      auto [type_info, source] = make_data_source(description);
      AttributeOwner attribute(
          description.writable
              ? type_info->buildAttribute(description.name, source)
              : type_info->buildAlias(description.name, source));
      if (!attribute || attribute->getDataSource().get() != source.get()) {
        throw std::runtime_error("failed to construct remote attribute '" +
                                 description.name + "'");
      }
      attributes.push_back(std::move(attribute));
    }
  } catch (const std::exception &exception) {
    assignError(error,
                std::string("failed to construct remote RTT interface: ") +
                    exception.what());
    return false;
  }

  const RTT::Service::shared_ptr root = provides();
  clear();
  for (auto &[name, operation] : operations) {
    root->add(name, operation.release());
  }
  for (auto &property : properties) {
    if (root->properties()->ownProperty(property.get())) {
      property.release();
    }
  }
  for (auto &attribute : attributes) {
    if (root->setValue(attribute.get())) {
      attribute.release();
    }
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
