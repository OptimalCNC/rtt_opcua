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
                      std::span<const std::string_view> trailing_segments) {
  std::vector<std::string_view> segments{"components", component};
  segments.insert(segments.end(), trailing_segments.begin(),
                  trailing_segments.end());
  return makeNodePath(segments);
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
        namespace_index_, modelPath(component_name, operation_folder_segment));
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
              ::opcua::NodeId(
                  namespace_index_,
                  modelPath(component_name, input_metadata_segments)),
              &operation.input_types, &last_error_) ||
          !readOptionalArray(
              *client_,
              ::opcua::NodeId(
                  namespace_index_,
                  modelPath(component_name, output_metadata_segments)),
              &operation.output_types, &last_error_) ||
          !readOptionalArray(
              *client_,
              ::opcua::NodeId(
                  namespace_index_,
                  modelPath(component_name, source_metadata_segments)),
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
