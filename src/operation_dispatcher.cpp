#include "operation_dispatcher.hpp"

#include "component_state.hpp"

#include <rtt/opcua/endpoint_type_registry.hpp>

#include <rtt/ArgumentDescription.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/SendStatus.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/internal/OperationCallerC.hpp>
#include <rtt/internal/SendHandleC.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace RTT::opcua::detail {
namespace {

std::string pointerFingerprint(const void *pointer) {
  std::ostringstream stream;
  stream << pointer;
  return stream.str();
}

std::string argumentName(const std::vector<RTT::ArgumentDescription> &arguments,
                         std::size_t index) {
  if (index < arguments.size() && !arguments[index].name.empty()) {
    return arguments[index].name;
  }
  return "argument" + std::to_string(index + 1U);
}

std::string
argumentDescription(const std::vector<RTT::ArgumentDescription> &arguments,
                    std::size_t index) {
  return index < arguments.size() ? arguments[index].description
                                  : std::string{};
}

bool isMutableReference(std::string_view type) {
  while (!type.empty() &&
         std::isspace(static_cast<unsigned char>(type.back()))) {
    type.remove_suffix(1U);
  }
  if (!type.ends_with('&')) {
    return false;
  }
  while (!type.empty() &&
         std::isspace(static_cast<unsigned char>(type.front()))) {
    type.remove_prefix(1U);
  }
  return !type.starts_with("const ");
}

OperationSchema unsupportedSchema(const RTT::types::TypeInfo *type,
                                  std::string reason) {
  OperationSchema schema;
  schema.unsupported_type_name =
      type == nullptr ? "<unknown>" : type->getTypeName();
  schema.unsupported_reason = std::move(reason);
  return schema;
}

OperationSchema unsupportedValueSchema(const RTT::types::TypeInfo *type,
                                       const TypeCodec *codec) {
  if (type == nullptr) {
    return unsupportedSchema(nullptr, "has no RTT type information");
  }
  if (codec == nullptr) {
    return unsupportedSchema(type, "has no registered OPC UA protocol");
  }
  return unsupportedSchema(type, "does not support OPC UA values");
}

struct PendingInvocation {
  PendingInvocation(
      ComponentLease &&component_lease,
      std::unique_ptr<RTT::internal::SendHandleC> operation_handle,
      std::vector<RTT::base::DataSourceBase::shared_ptr>
          collected_values) noexcept
      : lease(std::move(component_lease)), handle(std::move(operation_handle)),
        values(std::move(collected_values)) {}

  bool collectIfDone() noexcept {
    try {
      return handle->collectIfDone() != RTT::SendNotReady;
    } catch (...) {
      return true;
    }
  }

  void wait() noexcept {
    while (!collectIfDone()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ComponentLease lease;
  std::unique_ptr<RTT::internal::SendHandleC> handle;
  std::vector<RTT::base::DataSourceBase::shared_ptr> values;
};

void waitForCompletion(RTT::internal::SendHandleC &handle) noexcept {
  while (true) {
    try {
      if (handle.collectIfDone() != RTT::SendNotReady) {
        return;
      }
    } catch (...) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

} // namespace

class OperationDispatcher::Impl final {
public:
  Impl(std::shared_ptr<const EndpointTypeRegistry> endpoint_type_registry,
       std::chrono::milliseconds operation_timeout)
      : type_registry(std::move(endpoint_type_registry)),
        timeout(operation_timeout),
        reaper([this](std::stop_token stop) noexcept { runReaper(stop); }) {}

  void
  retain(ComponentLease &&lease, RTT::internal::SendHandleC &handle,
         std::vector<RTT::base::DataSourceBase::shared_ptr> values) noexcept {
    try {
      auto retained_handle =
          std::make_unique<RTT::internal::SendHandleC>(handle);
      auto pending = std::make_shared<PendingInvocation>(
          std::move(lease), std::move(retained_handle), std::move(values));
      bool retained = false;
      try {
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!draining) {
            invocations.push_back(pending);
            retained = true;
          }
        }
      } catch (...) {
        pending->wait();
        return;
      }
      if (retained) {
        wake.notify_one();
        return;
      }
      pending->wait();
    } catch (...) {
      waitForCompletion(handle);
    }
  }

  void drain() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex);
      draining = true;
    }
    reaper.request_stop();
    wake.notify_all();
    if (reaper.joinable()) {
      reaper.join();
    }
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        reapOnce();
        if (invocations.empty()) {
          return;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  std::size_t count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    return invocations.size();
  }

  const std::shared_ptr<const EndpointTypeRegistry> type_registry;
  const std::chrono::milliseconds timeout;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::vector<std::shared_ptr<PendingInvocation>> invocations;
  bool draining{false};
  std::jthread reaper;

private:
  void reapOnce() noexcept {
    std::erase_if(invocations, [](const auto &invocation) {
      return invocation->collectIfDone();
    });
  }

  void runReaper(std::stop_token stop) noexcept {
    try {
      std::unique_lock<std::mutex> lock(mutex);
      while (!stop.stop_requested()) {
        wake.wait(lock, [&] {
          return stop.stop_requested() || !invocations.empty();
        });
        if (stop.stop_requested()) {
          return;
        }
        reapOnce();
        if (!invocations.empty()) {
          wake.wait_for(lock, std::chrono::milliseconds(1),
                        [&] { return stop.stop_requested(); });
        }
      }
    } catch (...) {
    }
  }
};

OperationDispatcher::OperationDispatcher(
    std::shared_ptr<const EndpointTypeRegistry> type_registry,
    std::chrono::milliseconds timeout)
    : impl_(std::make_unique<Impl>(std::move(type_registry), timeout)) {}

OperationDispatcher::~OperationDispatcher() { impl_->drain(); }

OperationSchema
OperationDispatcher::describe(RTT::OperationInterfacePart &operation) const {
  OperationSchema schema;
  try {
    const std::vector<RTT::ArgumentDescription> arguments =
        operation.getArgumentList();
    std::ostringstream fingerprint;
    fingerprint << pointerFingerprint(&operation) << '|'
                << operation.description();

    for (unsigned int index = 0U; index < operation.arity(); ++index) {
      const RTT::types::TypeInfo *type = operation.getArgumentType(index + 1U);
      const TypeCodec *codec =
          impl_->type_registry ? impl_->type_registry->codecForTypeInfo(type)
                               : nullptr;
      if (type == nullptr || codec == nullptr || !codec->hasValue()) {
        return unsupportedValueSchema(type, codec);
      }
      const std::string name = argumentName(arguments, index);
      const std::string description = argumentDescription(arguments, index);
      schema.inputs.emplace_back(name,
                                 ::opcua::LocalizedText("en-US", description),
                                 codec->dataTypeNodeId(), codec->valueRank());
      schema.input_type_names.push_back(type->getTypeName());
      fingerprint << "|in:" << name << ':' << description << ':'
                  << type->getTypeName();
    }

    bool has_return_value = false;
    const RTT::types::TypeInfo *return_type = operation.getArgumentType(0U);
    const TypeCodec *return_codec =
        impl_->type_registry
            ? impl_->type_registry->codecForTypeInfo(return_type)
            : nullptr;
    if (return_type == nullptr || return_codec == nullptr) {
      return unsupportedValueSchema(return_type, return_codec);
    }
    if (!return_codec->hasValue() && return_type->getTypeName() != "Void") {
      return unsupportedValueSchema(return_type, return_codec);
    }
    has_return_value = return_codec->hasValue();

    std::vector<std::size_t> mutable_arguments;
    for (std::size_t index = 0U; index < arguments.size(); ++index) {
      if (isMutableReference(arguments[index].type)) {
        mutable_arguments.push_back(index);
      }
    }

    std::size_t mutable_index = 0U;
    for (unsigned int index = 0U; index < operation.collectArity(); ++index) {
      const RTT::types::TypeInfo *type = operation.getCollectType(index + 1U);
      const TypeCodec *codec =
          impl_->type_registry ? impl_->type_registry->codecForTypeInfo(type)
                               : nullptr;
      if (type == nullptr || codec == nullptr || !codec->hasValue()) {
        return unsupportedValueSchema(type, codec);
      }

      std::string name;
      std::string description;
      if (index == 0U && has_return_value) {
        name = "result";
        description = operation.description();
        schema.output_sources.push_back(-1);
      } else if (mutable_index < mutable_arguments.size()) {
        const std::size_t argument_index = mutable_arguments[mutable_index++];
        name = argumentName(arguments, argument_index);
        description = argumentDescription(arguments, argument_index);
        schema.output_sources.push_back(
            static_cast<std::int32_t>(argument_index));
      } else {
        return unsupportedSchema(type,
                                 "has unsupported mutable output metadata");
      }

      schema.outputs.emplace_back(name,
                                  ::opcua::LocalizedText("en-US", description),
                                  codec->dataTypeNodeId(), codec->valueRank());
      schema.output_type_names.push_back(type->getTypeName());
      fingerprint << "|out:" << name << ':' << description << ':'
                  << type->getTypeName();
    }

    schema.fingerprint = fingerprint.str();
    schema.supported = true;
  } catch (...) {
    return unsupportedSchema(nullptr,
                             "could not be described for OPC UA publication");
  }
  return schema;
}

::opcua::StatusCode
OperationDispatcher::invoke(const std::shared_ptr<ComponentState> &state,
                            RTT::OperationInterfacePart &operation,
                            ::opcua::Span<const ::opcua::Variant> inputs,
                            ::opcua::Span<::opcua::Variant> outputs) noexcept {
  ComponentLease lease(state);
  if (!lease) {
    return UA_STATUSCODE_BADNOTCONNECTED;
  }
  if (impl_->timeout <= std::chrono::milliseconds::zero()) {
    return UA_STATUSCODE_BADCONFIGURATIONERROR;
  }
  if (inputs.size() != operation.arity() ||
      outputs.size() != operation.collectArity()) {
    return UA_STATUSCODE_BADINVALIDARGUMENT;
  }

  try {
    RTT::internal::OperationCallerC caller(
        &operation, operation.getName(),
        RTT::internal::GlobalEngine::Instance());
    for (std::size_t index = 0U; index < inputs.size(); ++index) {
      const RTT::types::TypeInfo *type =
          operation.getArgumentType(static_cast<unsigned int>(index + 1U));
      const TypeCodec *codec =
          impl_->type_registry ? impl_->type_registry->codecForTypeInfo(type)
                               : nullptr;
      const auto value = codec == nullptr
                             ? RTT::base::DataSourceBase::shared_ptr{}
                             : codec->makeDataSource(inputs[index]);
      if (!value) {
        return UA_STATUSCODE_BADINVALIDARGUMENT;
      }
      caller.arg(value);
    }
    caller.check();
    if (!caller.ready()) {
      return UA_STATUSCODE_BADINTERNALERROR;
    }

    RTT::internal::SendHandleC handle = caller.send();
    std::vector<RTT::base::DataSourceBase::shared_ptr> collected_values;
    collected_values.reserve(operation.collectArity());
    for (unsigned int index = 0U; index < operation.collectArity(); ++index) {
      const RTT::types::TypeInfo *type = operation.getCollectType(index + 1U);
      const auto value = type == nullptr
                             ? RTT::base::DataSourceBase::shared_ptr{}
                             : type->buildValue();
      if (!value) {
        return UA_STATUSCODE_BADNOTSUPPORTED;
      }
      collected_values.push_back(value);
      handle.arg(value);
    }
    handle.check();
    if (!handle.ready()) {
      return UA_STATUSCODE_BADINTERNALERROR;
    }

    const auto deadline = std::chrono::steady_clock::now() + impl_->timeout;
    while (true) {
      const RTT::SendStatus status = handle.collectIfDone();
      if (status == RTT::SendSuccess) {
        for (std::size_t index = 0U; index < outputs.size(); ++index) {
          const TypeCodec *codec =
              impl_->type_registry ? impl_->type_registry->codecForDataSource(
                                         collected_values[index])
                                   : nullptr;
          if (codec == nullptr ||
              !codec->toVariant(collected_values[index], &outputs[index])) {
            return UA_STATUSCODE_BADINTERNALERROR;
          }
        }
        return UA_STATUSCODE_GOOD;
      }
      if (status != RTT::SendNotReady) {
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        impl_->retain(std::move(lease), handle, std::move(collected_values));
        return UA_STATUSCODE_BADTIMEOUT;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (const std::exception &) {
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  } catch (...) {
    return UA_STATUSCODE_BADUNEXPECTEDERROR;
  }
}

void OperationDispatcher::drainPending() noexcept { impl_->drain(); }

std::size_t OperationDispatcher::pendingCount() const noexcept {
  return impl_->count();
}

} // namespace RTT::opcua::detail
