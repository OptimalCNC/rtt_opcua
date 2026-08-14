#include "client_session.hpp"

#include <rtt/opcua/node_id.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/exception.hpp>
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

bool invalidatesInterface(::opcua::StatusCode status) noexcept {
  const UA_StatusCode code = status.get();
  return code == UA_STATUSCODE_BADNODEIDINVALID ||
         code == UA_STATUSCODE_BADNODEIDUNKNOWN ||
         code == UA_STATUSCODE_BADNOTFOUND ||
         code == UA_STATUSCODE_BADMETHODINVALID ||
         code == UA_STATUSCODE_BADSESSIONIDINVALID ||
         code == UA_STATUSCODE_BADSESSIONCLOSED ||
         code == UA_STATUSCODE_BADSESSIONNOTACTIVATED ||
         code == UA_STATUSCODE_BADSECURECHANNELIDINVALID ||
         code == UA_STATUSCODE_BADSECURECHANNELCLOSED ||
         code == UA_STATUSCODE_BADSECURECHANNELTOKENUNKNOWN ||
         code == UA_STATUSCODE_BADCONNECTIONREJECTED ||
         code == UA_STATUSCODE_BADCONNECTIONCLOSED ||
         code == UA_STATUSCODE_BADSERVERNOTCONNECTED ||
         code == UA_STATUSCODE_BADNOTCONNECTED ||
         code == UA_STATUSCODE_BADCOMMUNICATIONERROR ||
         code == UA_STATUSCODE_BADTIMEOUT ||
         code == UA_STATUSCODE_BADTYPEMISMATCH ||
         code == UA_STATUSCODE_BADINVALIDARGUMENT;
}

bool isMissingCategory(::opcua::StatusCode status) noexcept {
  return status.get() == UA_STATUSCODE_BADNODEIDUNKNOWN;
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

bool readStringValue(::opcua::Client &client, const ::opcua::NodeId &node_id,
                     std::string_view label, std::string *value,
                     std::string *error) {
  const auto result = ::opcua::services::readValue(client, node_id);
  if (!result) {
    assignError(error, statusMessage("failed to read " + std::string(label),
                                     result.code()));
    return false;
  }
  try {
    *value = result.value().to<std::string>();
    return true;
  } catch (const std::exception &exception) {
    assignError(error,
                "invalid " + std::string(label) + ": " + exception.what());
    return false;
  }
}

bool readPortDirection(::opcua::Client &client,
                       const ::opcua::NodeId &node_id,
                       std::string_view port_name,
                       PortDirection *direction, std::string *error) {
  const auto result = ::opcua::services::readValue(client, node_id);
  if (!result) {
    assignError(error,
                statusMessage("failed to read RTT port direction metadata",
                              result.code()));
    return false;
  }

  const ::opcua::Variant &value = result.value();
  if (!value.isScalar() ||
      !value.isType(::opcua::NodeId(::opcua::DataTypeId::Int32))) {
    assignError(error, "remote port '" + std::string(port_name) +
                           "' has invalid direction metadata: expected "
                           "scalar Int32");
    return false;
  }

  const std::int32_t code = value.to<std::int32_t>();
  switch (code) {
  case static_cast<std::int32_t>(PortDirection::input):
    *direction = PortDirection::input;
    return true;
  case static_cast<std::int32_t>(PortDirection::output):
    *direction = PortDirection::output;
    return true;
  default:
    assignError(error, "remote port '" + std::string(port_name) +
                           "' has unsupported direction code " +
                           std::to_string(code));
    return false;
  }
}

bool readMethodArguments(::opcua::Client &client,
                         const ::opcua::NodeId &method_id,
                         std::vector<::opcua::Argument> *inputs,
                         std::vector<::opcua::Argument> *outputs,
                         std::string *error) {
  inputs->clear();
  outputs->clear();
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

  bool found_inputs = false;
  bool found_outputs = false;
  for (const ::opcua::ReferenceDescription &reference : browse_result.value()) {
    if (!reference.nodeId().isLocal()) {
      continue;
    }
    const std::string_view name = reference.browseName().name();
    std::vector<::opcua::Argument> *destination = nullptr;
    bool *found = nullptr;
    if (name == "InputArguments") {
      destination = inputs;
      found = &found_inputs;
    } else if (name == "OutputArguments") {
      destination = outputs;
      found = &found_outputs;
    } else {
      continue;
    }
    if (*found) {
      assignError(error, "RTT method exposes duplicate " + std::string(name) +
                             " metadata");
      return false;
    }
    const auto value =
        ::opcua::services::readValue(client, reference.nodeId().nodeId());
    if (!value) {
      assignError(error, statusMessage("failed to read RTT method arguments",
                                       value.code()));
      return false;
    }
    try {
      *destination = value.value().to<std::vector<::opcua::Argument>>();
      *found = true;
    } catch (const std::exception &exception) {
      assignError(error, std::string("invalid RTT method argument metadata: ") +
                             exception.what());
      return false;
    }
  }
  return true;
}

bool readInputDescriptions(::opcua::Client &client,
                           const ::opcua::NodeId &method_id,
                           std::vector<std::string> *names,
                           std::vector<std::string> *descriptions,
                           std::string *error) {
  std::vector<::opcua::Argument> inputs;
  std::vector<::opcua::Argument> outputs;
  if (!readMethodArguments(client, method_id, &inputs, &outputs, error)) {
    return false;
  }

  names->clear();
  descriptions->clear();
  names->reserve(inputs.size());
  descriptions->reserve(inputs.size());
  for (const ::opcua::Argument &argument : inputs) {
    names->emplace_back(std::string_view(argument.name()));
    descriptions->emplace_back(argument.description().text());
  }
  return true;
}

bool argumentMatches(const ::opcua::Argument &argument,
                     const ::opcua::NodeId &data_type,
                     ::opcua::ValueRank value_rank) {
  return argument.dataType() == data_type && argument.valueRank() == value_rank;
}

bool validatePortMethod(::opcua::Client &client,
                        const EndpointTypeRegistry &type_registry,
                        const RemotePortDescription &port, std::string *error) {
  const TypeCodec *codec = type_registry.codecForTypeName(port.type_name);
  const TypeCodec *status_codec = type_registry.codecForTypeName(
      port.direction == PortDirection::input ? "WriteStatus" : "FlowStatus");
  if (codec == nullptr || !codec->hasValue() || status_codec == nullptr ||
      !status_codec->hasValue()) {
    assignError(error, "remote port '" + port.name +
                           "' uses unsupported RTT type '" + port.type_name +
                           "'");
    return false;
  }

  const auto executable =
      ::opcua::services::readExecutable(client, port.method_id);
  const auto user_executable =
      ::opcua::services::readUserExecutable(client, port.method_id);
  if (!executable || !user_executable) {
    assignError(error, "failed to read execution metadata for remote port '" +
                           port.name + "'");
    return false;
  }
  if (!executable.value() || !user_executable.value()) {
    assignError(error, "remote port '" + port.name +
                           "' method is not executable for the current user");
    return false;
  }

  std::vector<::opcua::Argument> inputs;
  std::vector<::opcua::Argument> outputs;
  if (!readMethodArguments(client, port.method_id, &inputs, &outputs, error)) {
    return false;
  }
  const bool status_output =
      !outputs.empty() &&
      argumentMatches(outputs.front(), status_codec->dataTypeNodeId(),
                      status_codec->valueRank());
  const bool valid =
      port.direction == PortDirection::input
          ? inputs.size() == 1U && outputs.size() == 1U && status_output &&
                argumentMatches(inputs.front(), codec->dataTypeNodeId(),
                                codec->valueRank())
          : inputs.empty() && outputs.size() == 2U && status_output &&
                argumentMatches(outputs[1], codec->dataTypeNodeId(),
                                codec->valueRank());
  if (!valid) {
    assignError(error, "remote port '" + port.name +
                           "' exposes an incompatible method signature");
    return false;
  }
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
    if (client_) {
      client_->disconnect();
      client_.reset();
    }
    type_registry_.reset();

    std::vector<::opcua::String> namespaces;
    {
      ::opcua::ClientConfig discovery_config;
      discovery_config.setTimeout(
          static_cast<std::uint32_t>(request_timeout_.count()));
      ::opcua::Client discovery_client(std::move(discovery_config));
      discovery_client.connect(endpoint_url_);
      namespaces = discovery_client.namespaceArray();
      discovery_client.disconnect();
    }
    const auto namespace_iterator =
        std::find(namespaces.begin(), namespaces.end(), kNamespaceUri);
    if (namespace_iterator == namespaces.end()) {
      last_error_ = "server does not expose the Orocos RTT OPC UA namespace";
      state_.store(ProxyConnectionState::disconnected);
      assignError(error, last_error_);
      return false;
    }
    const auto index = std::distance(namespaces.begin(), namespace_iterator);
    if (index <= 0 || index > static_cast<std::ptrdiff_t>(
                                  std::numeric_limits<std::uint16_t>::max())) {
      last_error_ = "server returned an invalid Orocos RTT namespace index";
      state_.store(ProxyConnectionState::disconnected);
      assignError(error, last_error_);
      return false;
    }
    namespace_index_ = static_cast<std::uint16_t>(index);

    std::vector<std::pair<std::string, std::uint16_t>> namespace_table;
    namespace_table.reserve(namespaces.size());
    for (std::size_t namespace_position = 0U;
         namespace_position < namespaces.size(); ++namespace_position) {
      namespace_table.emplace_back(
          std::string(std::string_view(namespaces[namespace_position])),
          static_cast<std::uint16_t>(namespace_position));
    }
    std::string registry_error;
    auto endpoint_registry =
        EndpointTypeRegistry::create(namespace_table, &registry_error);
    if (!endpoint_registry) {
      last_error_ = "failed to bind local OPC UA datatypes: " + registry_error;
      namespace_index_ = 0U;
      state_.store(ProxyConnectionState::disconnected);
      assignError(error, last_error_);
      return false;
    }

    ::opcua::ClientConfig config;
    config.setTimeout(static_cast<std::uint32_t>(request_timeout_.count()));
    if (!endpoint_registry->customDataTypes().empty()) {
      config.addCustomDataTypes(endpoint_registry->customDataTypes());
    }
    type_registry_ = std::move(endpoint_registry);
    client_ = std::make_unique<::opcua::Client>(std::move(config));
    client_->connect(endpoint_url_);
    state_.store(ProxyConnectionState::connected);
    return true;
  } catch (const std::exception &exception) {
    last_error_ = std::string("failed to connect to '") + endpoint_url_ +
                  "': " + exception.what();
    client_.reset();
    type_registry_.reset();
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
    std::vector<::opcua::ReferenceDescription> references;
    try {
      const auto browse_result = ::opcua::services::browseAll(*client_, browse);
      if (!browse_result) {
        if (isMissingCategory(browse_result.code())) {
          last_error_.clear();
          assignError(error, "");
          return operations;
        }
        last_error_ = statusMessage("failed to discover remote RTT operations",
                                    browse_result.code());
        assignError(error, last_error_);
        return operations;
      }
      references = browse_result.value();
    } catch (const ::opcua::BadStatus &exception) {
      if (isMissingCategory(::opcua::StatusCode(exception.code()))) {
        last_error_.clear();
        assignError(error, "");
        return operations;
      }
      throw;
    }

    for (const ::opcua::ReferenceDescription &reference : references) {
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
    std::vector<::opcua::ReferenceDescription> references;
    try {
      const auto browse_result = ::opcua::services::browseAll(*client_, browse);
      if (!browse_result) {
        if (isMissingCategory(browse_result.code())) {
          last_error_.clear();
          assignError(error, "");
          return values;
        }
        last_error_ = statusMessage("failed to discover remote RTT " +
                                        std::string(category_name),
                                    browse_result.code());
        assignError(error, last_error_);
        return values;
      }
      references = browse_result.value();
    } catch (const ::opcua::BadStatus &exception) {
      if (isMissingCategory(::opcua::StatusCode(exception.code()))) {
        last_error_.clear();
        assignError(error, "");
        return values;
      }
      throw;
    }

    for (const ::opcua::ReferenceDescription &reference : references) {
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
    std::vector<::opcua::ReferenceDescription> references;
    try {
      const auto browse_result = ::opcua::services::browseAll(*client_, browse);
      if (!browse_result) {
        if (isMissingCategory(browse_result.code())) {
          last_error_.clear();
          assignError(error, "");
          return services;
        }
        last_error_ = statusMessage("failed to discover remote RTT services",
                                    browse_result.code());
        assignError(error, last_error_);
        return services;
      }
      references = browse_result.value();
    } catch (const ::opcua::BadStatus &exception) {
      if (isMissingCategory(::opcua::StatusCode(exception.code()))) {
        last_error_.clear();
        assignError(error, "");
        return services;
      }
      throw;
    }

    for (const ::opcua::ReferenceDescription &reference : references) {
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

std::vector<RemotePortDescription>
ClientSession::discoverPorts(const std::string &component_name,
                             const RemoteServicePath &service_path,
                             std::string *error) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RemotePortDescription> ports;
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    assignError(error, last_error_);
    return ports;
  }

  try {
    const std::vector<std::string_view> folder_segments{"ports"};
    const ::opcua::NodeId folder_id(
        namespace_index_,
        modelPath(component_name, service_path, folder_segments));
    const ::opcua::BrowseDescription browse(
        folder_id, ::opcua::BrowseDirection::Forward,
        ::opcua::ReferenceTypeId::HasComponent, false,
        ::opcua::NodeClass::Object, ::opcua::BrowseResultMask::All);
    std::vector<::opcua::ReferenceDescription> references;
    try {
      const auto browse_result = ::opcua::services::browseAll(*client_, browse);
      if (!browse_result) {
        if (isMissingCategory(browse_result.code())) {
          last_error_.clear();
          assignError(error, "");
          return ports;
        }
        last_error_ = statusMessage("failed to discover remote RTT ports",
                                    browse_result.code());
        assignError(error, last_error_);
        return ports;
      }
      references = browse_result.value();
    } catch (const ::opcua::BadStatus &exception) {
      if (isMissingCategory(::opcua::StatusCode(exception.code()))) {
        last_error_.clear();
        assignError(error, "");
        return ports;
      }
      throw;
    }

    for (const ::opcua::ReferenceDescription &reference : references) {
      if (!reference.nodeId().isLocal()) {
        continue;
      }
      RemotePortDescription port;
      port.name = std::string(reference.browseName().name());
      port.service_path = service_path;
      port.object_id = reference.nodeId().nodeId();
      if (port.name.empty()) {
        continue;
      }

      const std::vector<std::string_view> type_segments{"ports", port.name,
                                                        "type"};
      const std::vector<std::string_view> direction_segments{"ports", port.name,
                                                             "direction"};
      if (!readStringValue(
              *client_,
              ::opcua::NodeId(
                  namespace_index_,
                  modelPath(component_name, service_path, type_segments)),
              "RTT port type metadata", &port.type_name, &last_error_) ||
          !readPortDirection(
              *client_,
              ::opcua::NodeId(
                  namespace_index_,
                  modelPath(component_name, service_path, direction_segments)),
              port.name, &port.direction, &last_error_)) {
        assignError(error, last_error_);
        ports.clear();
        return ports;
      }

      const auto description =
          ::opcua::services::readDescription(*client_, port.object_id);
      if (!description) {
        last_error_ = statusMessage(
            "failed to read description for remote port '" + port.name + "'",
            description.code());
        assignError(error, last_error_);
        ports.clear();
        return ports;
      }
      port.description = std::string(description.value().text());

      const std::string_view method_name =
          port.direction == PortDirection::input ? "write" : "read";
      const ::opcua::BrowseDescription method_browse(
          port.object_id, ::opcua::BrowseDirection::Forward,
          ::opcua::ReferenceTypeId::HasComponent, false,
          ::opcua::NodeClass::Method, ::opcua::BrowseResultMask::All);
      const auto method_result =
          ::opcua::services::browseAll(*client_, method_browse);
      if (!method_result) {
        last_error_ = statusMessage("failed to browse remote port methods",
                                    method_result.code());
        assignError(error, last_error_);
        ports.clear();
        return ports;
      }
      bool found_method = false;
      for (const ::opcua::ReferenceDescription &method_reference :
           method_result.value()) {
        if (!method_reference.nodeId().isLocal() ||
            method_reference.browseName().name() != method_name) {
          continue;
        }
        if (found_method) {
          last_error_ = "remote port '" + port.name + "' exposes duplicate " +
                        std::string(method_name) + " methods";
          assignError(error, last_error_);
          ports.clear();
          return ports;
        }
        port.method_id = method_reference.nodeId().nodeId();
        found_method = true;
      }
      if (!found_method) {
        last_error_ = "remote port '" + port.name + "' does not expose its " +
                      std::string(method_name) + " method";
        assignError(error, last_error_);
        ports.clear();
        return ports;
      }
      if (!type_registry_ ||
          !validatePortMethod(*client_, *type_registry_, port, &last_error_)) {
        assignError(error, last_error_);
        ports.clear();
        return ports;
      }
      ports.push_back(std::move(port));
    }

    std::sort(ports.begin(), ports.end(),
              [](const auto &left, const auto &right) {
                return left.name < right.name;
              });
    if (std::adjacent_find(ports.begin(), ports.end(),
                           [](const auto &left, const auto &right) {
                             return left.name == right.name;
                           }) != ports.end()) {
      last_error_ = "remote component exposes duplicate port names";
      assignError(error, last_error_);
      ports.clear();
      return ports;
    }
    last_error_.clear();
    assignError(error, "");
    return ports;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to discover remote RTT ports: ") + exception.what();
    if (!client_->isConnected()) {
      state_.store(ProxyConnectionState::stale);
    }
    assignError(error, last_error_);
    ports.clear();
    return ports;
  }
}

bool ClientSession::readValue(const ::opcua::NodeId &node_id,
                              ::opcua::Variant *value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (value == nullptr) {
    last_error_ = "remote RTT value destination must not be null";
    return false;
  }
  if (!interface_access_enabled_.load()) {
    last_error_ =
        "remote RTT interface is stale; synchronize it before reading values";
    return false;
  }
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    return false;
  }

  try {
    const auto result = ::opcua::services::readValue(*client_, node_id);
    if (!result) {
      const ::opcua::StatusCode status = result.code();
      last_error_ = statusMessage("failed to read remote RTT value", status);
      if (invalidatesInterface(status) || !client_->isConnected()) {
        invalidateInterfaceLocked();
      }
      return false;
    }
    *value = result.value();
    last_error_.clear();
    return true;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to read remote RTT value: ") + exception.what();
    invalidateInterfaceLocked();
    return false;
  }
}

bool ClientSession::writeValue(const ::opcua::NodeId &node_id,
                               const ::opcua::Variant &value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!interface_access_enabled_.load()) {
    last_error_ =
        "remote RTT interface is stale; synchronize it before writing values";
    return false;
  }
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    last_error_ = "OPC UA client is not connected";
    return false;
  }

  try {
    const ::opcua::StatusCode status =
        ::opcua::services::writeValue(*client_, node_id, value);
    if (!status.isGood()) {
      last_error_ = statusMessage("failed to write remote RTT value", status);
      if (invalidatesInterface(status) || !client_->isConnected()) {
        invalidateInterfaceLocked();
      }
      return false;
    }
    last_error_.clear();
    return true;
  } catch (const std::exception &exception) {
    last_error_ =
        std::string("failed to write remote RTT value: ") + exception.what();
    invalidateInterfaceLocked();
    return false;
  }
}

RemoteCallResult
ClientSession::call(const ::opcua::NodeId &object_id,
                    const ::opcua::NodeId &method_id,
                    const std::vector<::opcua::Variant> &inputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  return callLocked(object_id, method_id, inputs, true, false);
}

RemoteCallResult
ClientSession::callPort(const ::opcua::NodeId &object_id,
                        const ::opcua::NodeId &method_id,
                        const std::vector<::opcua::Variant> &inputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  return callLocked(object_id, method_id, inputs, true, true);
}

RemoteCallResult
ClientSession::callOperation(const std::string &component_name,
                             const RemoteServicePath &service_path,
                             const std::string &operation_name,
                             const std::vector<::opcua::Variant> &inputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::vector<std::string_view> operation_folder{"operations"};
  const std::vector<std::string_view> operation_method{"operations",
                                                       operation_name};
  return callLocked(
      ::opcua::NodeId(namespace_index_, modelPath(component_name, service_path,
                                                  operation_folder)),
      ::opcua::NodeId(namespace_index_, modelPath(component_name, service_path,
                                                  operation_method)),
      inputs, false, false);
}

void ClientSession::setInterfaceAccessEnabled(bool enabled) {
  if (!enabled) {
    interface_access_enabled_.store(false);
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  interface_access_enabled_.store(enabled);
  if (enabled) {
    last_error_.clear();
  }
}

void ClientSession::invalidateInterfaceLocked() noexcept {
  interface_access_enabled_.store(false);
  state_.store(ProxyConnectionState::stale);
}

RemoteCallResult ClientSession::callLocked(
    const ::opcua::NodeId &object_id, const ::opcua::NodeId &method_id,
    const std::vector<::opcua::Variant> &inputs, bool require_interface_access,
    bool tolerate_not_connected) {
  RemoteCallResult call_result;
  if (require_interface_access && !interface_access_enabled_.load()) {
    call_result.error =
        "remote RTT interface is stale; synchronize it before calling "
        "operations";
    last_error_ = call_result.error;
    return call_result;
  }
  if (!client_ || state_.load() != ProxyConnectionState::connected) {
    call_result.error = "OPC UA client is not connected";
    last_error_ = call_result.error;
    return call_result;
  }

  try {
    const auto result =
        ::opcua::services::call(*client_, object_id, method_id, inputs);
    if (!result.statusCode().isGood()) {
      const ::opcua::StatusCode status = result.statusCode();
      call_result.error = statusMessage("remote RTT operation failed", status);
      last_error_ = call_result.error;
      const bool expected_port_disconnect =
          tolerate_not_connected &&
          status.get() == UA_STATUSCODE_BADNOTCONNECTED;
      if ((!expected_port_disconnect && invalidatesInterface(status)) ||
          !client_->isConnected()) {
        invalidateInterfaceLocked();
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
    invalidateInterfaceLocked();
    return call_result;
  }
}

ProxyConnectionState ClientSession::state() const noexcept {
  return state_.load();
}

const std::string &ClientSession::endpointUrl() const noexcept {
  return endpoint_url_;
}

std::shared_ptr<const EndpointTypeRegistry>
ClientSession::typeRegistry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return type_registry_;
}

std::string ClientSession::lastError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

} // namespace RTT::opcua::detail
