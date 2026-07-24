#include "client_session.hpp"

#include <rtt/opcua/node_id.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/method.hpp>
#include <open62541pp/services/view.hpp>
#include <open62541pp/ua/nodeids.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace RTT::opcua::detail {
namespace {

void assignError(std::string *output, const std::string &message) {
  if (output != nullptr) {
    *output = message;
  }
}

std::string statusMessage(std::string_view operation,
                          ::opcua::StatusCode status) {
  std::string message(operation);
  message += ": ";
  message += status.name();
  return message;
}

std::string modelPath(std::string_view component,
                      const RemoteServicePath &service_path,
                      std::span<const std::string_view> trailing_segments) {
  std::vector<std::string_view> segments{"components", component};
  segments.reserve(segments.size() + service_path.size() * 2U +
                   trailing_segments.size());
  for (const std::string &service_name : service_path) {
    segments.push_back("services");
    segments.push_back(service_name);
  }
  segments.insert(segments.end(), trailing_segments.begin(),
                  trailing_segments.end());
  return makeNodePath(segments);
}

std::string_view categorySegment(RemoteValueCategory category) noexcept {
  switch (category) {
  case RemoteValueCategory::properties:
    return "properties";
  case RemoteValueCategory::attributes:
    return "attributes";
  }
  return {};
}

template <typename T>
bool readOptionalArray(::opcua::Client &client, const ::opcua::NodeId &node_id,
                       std::vector<T> *values, std::string *error) {
  const auto result = ::opcua::services::readValue(client, node_id);
  if (!result) {
    if (result.code().get() == UA_STATUSCODE_BADNODEIDUNKNOWN) {
      values->clear();
      return true;
    }
    assignError(error, statusMessage("failed to read RTT method metadata",
                                     result.code()));
    return false;
  }
  try {
    *values = result.value().template to<std::vector<T>>();
    return true;
  } catch (const std::exception &exception) {
    assignError(error, std::string("invalid RTT method metadata: ") +
                           exception.what());
    return false;
  }
}

bool readInputDescriptions(::opcua::Client &client,
                           const ::opcua::NodeId &method_id,
                           std::vector<std::string> *names,
                           std::vector<std::string> *descriptions,
                           std::string *error) {
  const ::opcua::BrowseDescription browse(
      method_id, ::opcua::BrowseDirection::Forward,
      ::opcua::ReferenceTypeId::HasProperty, true, ::opcua::NodeClass::Variable,
      ::opcua::BrowseResultMask::All);
  const auto browse_result = ::opcua::services::browseAll(client, browse);
  if (!browse_result) {
    assignError(error, statusMessage("failed to browse RTT method arguments",
                                     browse_result.code()));
    return false;
  }

  for (const ::opcua::ReferenceDescription &reference : browse_result.value()) {
    if (reference.browseName().name() != "InputArguments" ||
        !reference.nodeId().isLocal()) {
      continue;
    }
    const auto value =
        ::opcua::services::readValue(client, reference.nodeId().nodeId());
    if (!value) {
      assignError(error, statusMessage("failed to read RTT input arguments",
                                       value.code()));
      return false;
    }
    try {
      const auto arguments = value.value().to<std::vector<::opcua::Argument>>();
      names->clear();
      descriptions->clear();
      names->reserve(arguments.size());
      descriptions->reserve(arguments.size());
      for (const ::opcua::Argument &argument : arguments) {
        names->emplace_back(std::string_view(argument.name()));
        descriptions->emplace_back(argument.description().text());
      }
      return true;
    } catch (const std::exception &exception) {
      assignError(error, std::string("invalid RTT input argument metadata: ") +
                             exception.what());
      return false;
    }
  }

  names->clear();
  descriptions->clear();
  return true;
}

} // namespace

ClientSession::ClientSession(std::string endpoint_url,
                             std::chrono::milliseconds request_timeout)
    : endpoint_url_(std::move(endpoint_url)),
      request_timeout_(request_timeout) {}

ClientSession::~ClientSession() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (client_) {
    try {
      client_->disconnect();
    } catch (...) {
    }
  }
  state_.store(ProxyConnectionState::disconnected);
}

bool ClientSession::connect(std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.store(ProxyConnectionState::connecting);
  last_error_.clear();

  try {
    ::opcua::ClientConfig config;
    config.setTimeout(static_cast<std::uint32_t>(request_timeout_.count()));
    client_ = std::make_unique<::opcua::Client>(std::move(config));
    client_->connect(endpoint_url_);

    const auto namespaces = client_->namespaceArray();
    const auto namespace_iterator =
        std::find(namespaces.begin(), namespaces.end(), kNamespaceUri);
    if (namespace_iterator == namespaces.end()) {
      last_error_ = "server does not expose the Orocos RTT OPC UA namespace";
      client_->disconnect();
      client_.reset();
      state_.store(ProxyConnectionState::disconnected);
      assignError(error, last_error_);
      return false;
    }
    const auto index = std::distance(namespaces.begin(), namespace_iterator);
    if (index <= 0 || index > static_cast<std::ptrdiff_t>(
                                  std::numeric_limits<std::uint16_t>::max())) {
      last_error_ = "server returned an invalid Orocos RTT namespace index";
      client_->disconnect();
      client_.reset();
      state_.store(ProxyConnectionState::disconnected);
      assignError(error, last_error_);
      return false;
    }
    namespace_index_ = static_cast<std::uint16_t>(index);
    state_.store(ProxyConnectionState::connected);
    return true;
  } catch (const std::exception &exception) {
    last_error_ = std::string("failed to connect to '") + endpoint_url_ +
                  "': " + exception.what();
    client_.reset();
    namespace_index_ = 0U;
    state_.store(ProxyConnectionState::disconnected);
    assignError(error, last_error_);
    return false;
  }
}

std::vector<RemoteOperationDescription>
ClientSession::discoverOperations(const std::string &component_name,
                                  const RemoteServicePath &service_path,
                                  std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RemoteOperationDescription> operations;
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    assignError(error, last_error_);
    return operations;
  }

  try {
    const std::vector<std::string_view> component_segments{"components",
                                                           component_name};
    const ::opcua::NodeId component_id(namespace_index_,
                                       makeNodePath(component_segments));
    const auto component_name_result =
        ::opcua::services::readDisplayName(*client_, component_id);
    if (!component_name_result) {
      last_error_ = statusMessage("remote RTT component '" + component_name +
                                      "' was not found",
                                  component_name_result.code());
      assignError(error, last_error_);
      return operations;
    }

    const std::vector<std::string_view> operation_folder_segment{"operations"};
    const ::opcua::NodeId operations_id(
        namespace_index_,
        modelPath(component_name, service_path, operation_folder_segment));
    const ::opcua::BrowseDescription browse(
        operations_id, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HasComponent, false,
        ::opcua::NodeClass::Method, ::opcua::BrowseResultMask::All);
    const auto browse_result = ::opcua::services::browseAll(*client_, browse);
    if (!browse_result) {
      last_error_ = statusMessage("failed to discover remote RTT operations",
                                  browse_result.code());
      assignError(error, last_error_);
      return operations;
    }

    for (const ::opcua::ReferenceDescription &reference :
         browse_result.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }

      RemoteOperationDescription operation;
      operation.name = std::string(reference.browseName().name());
      operation.object_id = operations_id;
      operation.method_id = reference.nodeId().nodeId();
      if (operation.name.empty()) {
        continue;
      }

      const std::vector<std::string_view> input_metadata_segments{
          "operations", operation.name, "rttInputTypes"};
      const std::vector<std::string_view> output_metadata_segments{
          "operations", operation.name, "rttOutputTypes"};
      const std::vector<std::string_view> source_metadata_segments{
          "operations", operation.name, "rttOutputSources"};
      if (!readOptionalArray(
              *client_,
              ::opcua::NodeId(namespace_index_,
                              modelPath(component_name, service_path,
                                        input_metadata_segments)),
              &operation.input_types, &last_error_) ||
          !readOptionalArray(
              *client_,
              ::opcua::NodeId(namespace_index_,
                              modelPath(component_name, service_path,
                                        output_metadata_segments)),
              &operation.output_types, &last_error_) ||
          !readOptionalArray(
              *client_,
              ::opcua::NodeId(namespace_index_,
                              modelPath(component_name, service_path,
                                        source_metadata_segments)),
              &operation.output_sources, &last_error_)) {
        assignError(error, last_error_);
        operations.clear();
        return operations;
      }
      if (operation.output_types.size() != operation.output_sources.size()) {
        last_error_ = "remote operation '" + operation.name +
                      "' has inconsistent RTT output metadata";
        assignError(error, last_error_);
        operations.clear();
        return operations;
      }
      if (!readInputDescriptions(*client_, operation.method_id,
                                 &operation.input_names,
                                 &operation.input_descriptions, &last_error_)) {
        assignError(error, last_error_);
        operations.clear();
        return operations;
      }
      if ((!operation.input_names.empty() ||
           !operation.input_descriptions.empty()) &&
          (operation.input_names.size() != operation.input_types.size() ||
           operation.input_descriptions.size() !=
               operation.input_types.size())) {
        last_error_ = "remote operation '" + operation.name +
                      "' has inconsistent RTT input metadata";
        assignError(error, last_error_);
        operations.clear();
        return operations;
      }

      const auto description =
          ::opcua::services::readDescription(*client_, operation.method_id);
      if (description) {
        operation.description = std::string(description.value().text());
      }
      operations.push_back(std::move(operation));
    }

    std::sort(operations.begin(), operations.end(),
              [](const auto &left, const auto &right) {
                return left.name < right.name;
              });
    if (std::adjacent_find(operations.begin(), operations.end(),
                           [](const auto &left, const auto &right) {
                             return left.name == right.name;
                           }) != operations.end()) {
      last_error_ = "remote component exposes duplicate operation names";
      assignError(error, last_error_);
      operations.clear();
      return operations;
    }
    last_error_.clear();
    return operations;
  } catch (const std::exception &exception) {
    last_error_ = std::string("failed to discover remote RTT operations: ") +
                  exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    assignError(error, last_error_);
    operations.clear();
    return operations;
  }
}

std::vector<RemoteValueDescription> ClientSession::discoverValues(
    const std::string &component_name, const RemoteServicePath &service_path,
    RemoteValueCategory category, std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RemoteValueDescription> values;
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    assignError(error, last_error_);
    return values;
  }

  const std::string_view category_name = categorySegment(category);
  if (category_name.empty()) {
    last_error_ = "invalid remote RTT value category";
    assignError(error, last_error_);
    return values;
  }

  try {
    const std::vector<std::string_view> folder_segments{category_name};
    const ::opcua::NodeId folder_id(
        namespace_index_,
        modelPath(component_name, service_path, folder_segments));
    const ::opcua::BrowseDescription browse(
        folder_id, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HasComponent, false,
        ::opcua::NodeClass::Variable, ::opcua::BrowseResultMask::All);
    const auto browse_result = ::opcua::services::browseAll(*client_, browse);
    if (!browse_result) {
      last_error_ = statusMessage("failed to discover remote RTT " +
                                      std::string(category_name),
                                  browse_result.code());
      assignError(error, last_error_);
      return values;
    }

    for (const ::opcua::ReferenceDescription &reference :
         browse_result.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }

      RemoteValueDescription value;
      value.name = std::string(reference.browseName().name());
      value.node_id = reference.nodeId().nodeId();
      if (value.name.empty()) {
        continue;
      }

      const std::vector<std::string_view> type_segments{category_name,
                                                        value.name, "rttType"};
      const auto type_result = ::opcua::services::readValue(
          *client_, ::opcua::NodeId(namespace_index_,
                                    modelPath(component_name, service_path,
                                              type_segments)));
      if (!type_result) {
        last_error_ = statusMessage("failed to read RTT type metadata for '" +
                                        value.name + "'",
                                    type_result.code());
        assignError(error, last_error_);
        values.clear();
        return values;
      }
      try {
        value.type_name = type_result.value().to<std::string>();
      } catch (const std::exception &exception) {
        last_error_ = "invalid RTT type metadata for '" + value.name +
                      "': " + exception.what();
        assignError(error, last_error_);
        values.clear();
        return values;
      }

      const auto description =
          ::opcua::services::readDescription(*client_, value.node_id);
      if (!description) {
        last_error_ = statusMessage("failed to read description for remote " +
                                        std::string(category_name) + " '" +
                                        value.name + "'",
                                    description.code());
        assignError(error, last_error_);
        values.clear();
        return values;
      }
      value.description = std::string(description.value().text());

      const auto access =
          ::opcua::services::readUserAccessLevel(*client_, value.node_id);
      if (!access) {
        last_error_ = statusMessage("failed to read access level for remote " +
                                        std::string(category_name) + " '" +
                                        value.name + "'",
                                    access.code());
        assignError(error, last_error_);
        values.clear();
        return values;
      }
      value.writable = access.value().anyOf(::opcua::AccessLevel::CurrentWrite);
      values.push_back(std::move(value));
    }

    std::sort(values.begin(), values.end(),
              [](const auto &left, const auto &right) {
                return left.name < right.name;
              });
    if (std::adjacent_find(values.begin(), values.end(),
                           [](const auto &left, const auto &right) {
                             return left.name == right.name;
                           }) != values.end()) {
      last_error_ = "remote component exposes duplicate " +
                    std::string(category_name) + " names";
      assignError(error, last_error_);
      values.clear();
      return values;
    }
    last_error_.clear();
    assignError(error, "");
    return values;
  } catch (const std::exception &exception) {
    last_error_ = "failed to discover remote RTT " +
                  std::string(category_name) + ": " + exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    assignError(error, last_error_);
    values.clear();
    return values;
  }
}

bool ClientSession::readServiceDescription(
    const std::string &component_name, const RemoteServicePath &service_path,
    std::string *description, std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (description == nullptr) {
    last_error_ = "remote RTT service description destination must not be null";
    assignError(error, last_error_);
    return false;
  }
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    assignError(error, last_error_);
    return false;
  }

  try {
    const std::vector<std::string_view> no_trailing_segments;
    const ::opcua::NodeId service_id(
        namespace_index_,
        modelPath(component_name, service_path, no_trailing_segments));
    const auto result =
        ::opcua::services::readDescription(*client_, service_id);
    if (!result) {
      last_error_ = statusMessage(
          "failed to read remote RTT service description", result.code());
      assignError(error, last_error_);
      return false;
    }
    *description = std::string(result.value().text());
    last_error_.clear();
    assignError(error, "");
    return true;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to read remote RTT service description: ") +
        exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    assignError(error, last_error_);
    return false;
  }
}

std::vector<RemoteServiceDescription>
ClientSession::discoverServices(const std::string &component_name,
                                const RemoteServicePath &service_path,
                                std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RemoteServiceDescription> services;
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    assignError(error, last_error_);
    return services;
  }

  try {
    const std::vector<std::string_view> folder_segments{"services"};
    const ::opcua::NodeId folder_id(
        namespace_index_,
        modelPath(component_name, service_path, folder_segments));
    const ::opcua::BrowseDescription browse(
        folder_id, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HasComponent, false,
        ::opcua::NodeClass::Object, ::opcua::BrowseResultMask::All);
    const auto browse_result = ::opcua::services::browseAll(*client_, browse);
    if (!browse_result) {
      last_error_ = statusMessage("failed to discover remote RTT services",
                                  browse_result.code());
      assignError(error, last_error_);
      return services;
    }

    for (const ::opcua::ReferenceDescription &reference :
         browse_result.value()) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }
      RemoteServiceDescription service;
      service.name = std::string(reference.browseName().name());
      if (service.name.empty() || service.name == "this") {
        continue;
      }
      const auto description = ::opcua::services::readDescription(
          *client_, reference.nodeId().nodeId());
      if (!description) {
        last_error_ =
            statusMessage("failed to read description for remote service '" +
                              service.name + "'",
                          description.code());
        assignError(error, last_error_);
        services.clear();
        return services;
      }
      service.description = std::string(description.value().text());
      services.push_back(std::move(service));
    }

    std::sort(services.begin(), services.end(),
              [](const auto &left, const auto &right) {
                return left.name < right.name;
              });
    if (std::adjacent_find(services.begin(), services.end(),
                           [](const auto &left, const auto &right) {
                             return left.name == right.name;
                           }) != services.end()) {
      last_error_ = "remote component exposes duplicate service names";
      assignError(error, last_error_);
      services.clear();
      return services;
    }
    last_error_.clear();
    assignError(error, "");
    return services;
  } catch (const std::exception &exception) {
    last_error_ = std::string("failed to discover remote RTT services: ") +
                  exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    assignError(error, last_error_);
    services.clear();
    return services;
  }
}

bool ClientSession::readValue(const ::opcua::NodeId &node_id,
                              ::opcua::Variant *value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (value == nullptr) {
    last_error_ = "remote RTT value destination must not be null";
    return false;
  }
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    return false;
  }

  try {
    const auto result = ::opcua::services::readValue(*client_, node_id);
    if (!result) {
      last_error_ =
          statusMessage("failed to read remote RTT value", result.code());
      if (!client_->isConnected()) {
        state_.store(ProxyConnectionState::stale);
      }
      return false;
    }
    *value = result.value();
    last_error_.clear();
    return true;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to read remote RTT value: ") + exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    return false;
  }
}

bool ClientSession::writeValue(const ::opcua::NodeId &node_id,
                               const ::opcua::Variant &value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    return false;
  }

  try {
    const ::opcua::StatusCode status =
        ::opcua::services::writeValue(*client_, node_id, value);
    if (!status.isGood()) {
      last_error_ = statusMessage("failed to write remote RTT value", status);
      if (!client_->isConnected()) {
        state_.store(ProxyConnectionState::stale);
      }
      return false;
    }
    last_error_.clear();
    return true;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to write remote RTT value: ") + exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    return false;
  }
}

RemoteCallResult
ClientSession::call(const ::opcua::NodeId &object_id,
                    const ::opcua::NodeId &method_id,
                    const std::vector<::opcua::Variant> &inputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  RemoteCallResult call_result;
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    call_result.error = "OPC UA client is not connected";
    last_error_ = call_result.error;
    return call_result;
  }

  try {
    const auto result =
        ::opcua::services::call(*client_, object_id, method_id, inputs);
    if (!result.statusCode().isGood()) {
      call_result.error =
          statusMessage("remote RTT operation failed", result.statusCode());
      last_error_ = call_result.error;
      if (!client_->isConnected()) {
        state_.store(ProxyConnectionState::stale);
      }
      return call_result;
    }
    call_result.outputs.assign(result.outputArguments().begin(),
                               result.outputArguments().end());
    call_result.success = true;
    last_error_.clear();
    return call_result;
  } catch (const std::exception &exception) {
    call_result.error =
        std::string("remote RTT operation call failed: ") + exception.what();
    last_error_ = call_result.error;
    state_.store(ProxyConnectionState::stale);
    return call_result;
  }
}

ProxyConnectionState ClientSession::state() const noexcept {
  return state_.load();
}

const std::string &ClientSession::endpointUrl() const noexcept {
  return endpoint_url_;
}

std::string ClientSession::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

} // namespace RTT::opcua::detail
