#include "remote_operation.hpp"

#include <rtt/ArgumentDescription.hpp>
#include <rtt/FactoryExceptions.hpp>
#include <rtt/Handle.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/SendStatus.hpp>
#include <rtt/base/ActionInterface.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSourceCommand.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace RTT::opcua::detail {
namespace {

using DataSource = RTT::base::DataSourceBase;
using DataSourceList = std::vector<DataSource::shared_ptr>;

struct RemoteInvocation {
  std::shared_future<std::shared_ptr<RemoteCallResult>> future;
};

using RemoteInvocationPtr = std::shared_ptr<RemoteInvocation>;
using InvocationDataSource =
    RTT::internal::AssignableDataSource<RemoteInvocationPtr>;

DataSourceList
copyDataSources(const DataSourceList &sources,
                std::map<const DataSource *, DataSource *> &already_cloned) {
  DataSourceList copies;
  copies.reserve(sources.size());
  for (const auto &source : sources) {
    copies.push_back(source ? source->copy(already_cloned)
                            : DataSource::shared_ptr{});
  }
  return copies;
}

bool encodeArguments(const EndpointTypeRegistry &type_registry,
                     const DataSourceList &arguments,
                     std::vector<::opcua::Variant> *inputs) {
  inputs->clear();
  inputs->reserve(arguments.size());
  try {
    for (const auto &argument : arguments) {
      const TypeCodec *codec = type_registry.codecForDataSource(argument);
      ::opcua::Variant value;
      if (codec == nullptr || !codec->toVariant(argument, &value)) {
        inputs->clear();
        return false;
      }
      inputs->push_back(std::move(value));
    }
    return true;
  } catch (...) {
    inputs->clear();
    return false;
  }
}

bool assignCallOutputs(const EndpointTypeRegistry &type_registry,
                       const RemoteCallResult &result,
                       const std::vector<std::string> &output_types,
                       const std::vector<std::int32_t> &output_sources,
                       const DataSourceList &arguments,
                       const DataSource::shared_ptr &return_value) {
  if (!result.success || result.outputs.size() != output_types.size() ||
      output_types.size() != output_sources.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < output_types.size(); ++index) {
    const TypeCodec *codec =
        type_registry.codecForTypeName(output_types[index]);
    const std::int32_t source = output_sources[index];
    const DataSource::shared_ptr destination =
        source == -1 ? return_value
                     : (source >= 0 && static_cast<std::size_t>(source) <
                                           arguments.size()
                            ? arguments[static_cast<std::size_t>(source)]
                            : DataSource::shared_ptr{});
    if (codec == nullptr || !destination ||
        !codec->assignVariant(result.outputs[index], destination)) {
      return false;
    }
  }
  return true;
}

class RemoteCallAction final : public RTT::base::ActionInterface {
public:
  RemoteCallAction(std::shared_ptr<ClientSession> session,
                   std::shared_ptr<const EndpointTypeRegistry> type_registry,
                   ::opcua::NodeId object_id, ::opcua::NodeId method_id,
                   DataSourceList arguments,
                   std::vector<std::string> output_types,
                   std::vector<std::int32_t> output_sources,
                   DataSource::shared_ptr return_value)
      : session_(std::move(session)), type_registry_(std::move(type_registry)),
        object_id_(std::move(object_id)), method_id_(std::move(method_id)),
        arguments_(std::move(arguments)),
        output_types_(std::move(output_types)),
        output_sources_(std::move(output_sources)),
        return_value_(std::move(return_value)) {}

  void readArguments() override {
    prepared_ = type_registry_ &&
                encodeArguments(*type_registry_, arguments_, &inputs_);
  }

  bool execute() override {
    if (!prepared_) {
      return false;
    }
    const RemoteCallResult result =
        session_->call(object_id_, method_id_, inputs_);
    return type_registry_ &&
           assignCallOutputs(*type_registry_, result, output_types_,
                             output_sources_, arguments_, return_value_);
  }

  RemoteCallAction *clone() const override {
    return new RemoteCallAction(session_, type_registry_, object_id_,
                                method_id_, arguments_, output_types_,
                                output_sources_, return_value_);
  }

  RemoteCallAction *copy(std::map<const DataSource *, DataSource *>
                             &already_cloned) const override {
    DataSource::shared_ptr return_copy =
        return_value_ ? return_value_->copy(already_cloned)
                      : DataSource::shared_ptr{};
    return new RemoteCallAction(
        session_, type_registry_, object_id_, method_id_,
        copyDataSources(arguments_, already_cloned), output_types_,
        output_sources_, std::move(return_copy));
  }

private:
  std::shared_ptr<ClientSession> session_;
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  ::opcua::NodeId object_id_;
  ::opcua::NodeId method_id_;
  DataSourceList arguments_;
  std::vector<std::string> output_types_;
  std::vector<std::int32_t> output_sources_;
  DataSource::shared_ptr return_value_;
  std::vector<::opcua::Variant> inputs_;
  bool prepared_{false};
};

class RemoteSendAction final : public RTT::base::ActionInterface {
public:
  RemoteSendAction(std::shared_ptr<ClientSession> session,
                   std::shared_ptr<const EndpointTypeRegistry> type_registry,
                   ::opcua::NodeId object_id, ::opcua::NodeId method_id,
                   DataSourceList arguments,
                   InvocationDataSource::shared_ptr handle)
      : session_(std::move(session)), type_registry_(std::move(type_registry)),
        object_id_(std::move(object_id)), method_id_(std::move(method_id)),
        arguments_(std::move(arguments)), handle_(std::move(handle)) {}

  void readArguments() override {
    prepared_ = type_registry_ &&
                encodeArguments(*type_registry_, arguments_, &inputs_);
  }

  bool execute() override {
    if (!prepared_) {
      return false;
    }
    try {
      auto invocation = std::make_shared<RemoteInvocation>();
      invocation->future =
          std::async(std::launch::async, [session = session_,
                                          object_id = object_id_,
                                          method_id = method_id_,
                                          inputs = inputs_]() {
            return std::make_shared<RemoteCallResult>(
                session->call(object_id, method_id, inputs));
          }).share();
      handle_->set(std::move(invocation));
      return true;
    } catch (...) {
      handle_->set({});
      return false;
    }
  }

  RemoteSendAction *clone() const override {
    return new RemoteSendAction(session_, type_registry_, object_id_,
                                method_id_, arguments_, handle_);
  }

  RemoteSendAction *copy(std::map<const DataSource *, DataSource *>
                             &already_cloned) const override {
    InvocationDataSource::shared_ptr handle_copy(handle_->copy(already_cloned));
    return new RemoteSendAction(
        session_, type_registry_, object_id_, method_id_,
        copyDataSources(arguments_, already_cloned), std::move(handle_copy));
  }

private:
  std::shared_ptr<ClientSession> session_;
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  ::opcua::NodeId object_id_;
  ::opcua::NodeId method_id_;
  DataSourceList arguments_;
  InvocationDataSource::shared_ptr handle_;
  std::vector<::opcua::Variant> inputs_;
  bool prepared_{false};
};

class RemoteCollectDataSource final
    : public RTT::internal::DataSource<RTT::SendStatus> {
public:
  RemoteCollectDataSource(
      std::shared_ptr<const EndpointTypeRegistry> type_registry,
      RemoteInvocationPtr invocation, DataSourceList destinations,
      std::vector<std::string> output_types,
      RTT::internal::DataSource<bool>::shared_ptr blocking)
      : type_registry_(std::move(type_registry)),
        invocation_(std::move(invocation)),
        destinations_(std::move(destinations)),
        output_types_(std::move(output_types)), blocking_(std::move(blocking)) {
  }

  RTT::SendStatus get() const override {
    if (status_ == RTT::SendSuccess || status_ == RTT::SendFailure) {
      return status_;
    }
    if (!invocation_ || !invocation_->future.valid()) {
      status_ = RTT::SendFailure;
      return status_;
    }

    try {
      if (!blocking_->get() &&
          invocation_->future.wait_for(std::chrono::milliseconds::zero()) !=
              std::future_status::ready) {
        status_ = RTT::SendNotReady;
        return status_;
      }
      const std::shared_ptr<RemoteCallResult> result =
          invocation_->future.get();
      if (!result || !result->success ||
          result->outputs.size() != destinations_.size() ||
          destinations_.size() != output_types_.size()) {
        status_ = RTT::SendFailure;
        return status_;
      }
      for (std::size_t index = 0U; index < destinations_.size(); ++index) {
        const TypeCodec *codec =
            type_registry_
                ? type_registry_->codecForTypeName(output_types_[index])
                : nullptr;
        if (codec == nullptr || !codec->assignVariant(result->outputs[index],
                                                      destinations_[index])) {
          status_ = RTT::SendFailure;
          return status_;
        }
      }
      status_ = RTT::SendSuccess;
      return status_;
    } catch (...) {
      status_ = RTT::SendFailure;
      return status_;
    }
  }

  RTT::SendStatus value() const override { return status_; }

  const RTT::SendStatus &rvalue() const override { return status_; }

  RemoteCollectDataSource *clone() const override {
    return new RemoteCollectDataSource(type_registry_, invocation_,
                                       destinations_, output_types_, blocking_);
  }

  RemoteCollectDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    RTT::internal::DataSource<bool>::shared_ptr blocking_copy(
        blocking_->copy(already_cloned));
    return new RemoteCollectDataSource(
        type_registry_, invocation_,
        copyDataSources(destinations_, already_cloned), output_types_,
        std::move(blocking_copy));
  }

private:
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  RemoteInvocationPtr invocation_;
  DataSourceList destinations_;
  std::vector<std::string> output_types_;
  RTT::internal::DataSource<bool>::shared_ptr blocking_;
  mutable RTT::SendStatus status_{RTT::SendNotReady};
};

class RemoteOperation final : public RTT::OperationInterfacePart {
public:
  RemoteOperation(std::shared_ptr<ClientSession> session,
                  RemoteOperationDescription description)
      : session_(std::move(session)),
        type_registry_(session_ ? session_->typeRegistry() : nullptr),
        description_(std::move(description)) {
    if (!type_registry_) {
      throw std::runtime_error("OPC UA client type registry is unavailable");
    }
    for (const std::string &name : description_.input_types) {
      const RTT::types::TypeInfo *type = RTT::types::Types()->type(name);
      if (type == nullptr ||
          type_registry_->codecForTypeInfo(type) == nullptr) {
        throw std::runtime_error("unsupported remote RTT input type '" + name +
                                 "'");
      }
      input_types_.push_back(type);
    }
    for (const std::string &name : description_.output_types) {
      const RTT::types::TypeInfo *type = RTT::types::Types()->type(name);
      if (type == nullptr ||
          type_registry_->codecForTypeInfo(type) == nullptr) {
        throw std::runtime_error("unsupported remote RTT output type '" + name +
                                 "'");
      }
      output_types_.push_back(type);
    }
    if (output_types_.size() != description_.output_sources.size()) {
      throw std::runtime_error("inconsistent output metadata for remote RTT "
                               "operation '" +
                               description_.name + "'");
    }

    std::set<std::size_t> mutable_arguments;
    for (std::size_t output_index = 0U;
         output_index < description_.output_sources.size(); ++output_index) {
      const std::int32_t source = description_.output_sources[output_index];
      if (source == -1) {
        if (return_type_ != nullptr) {
          throw std::runtime_error("multiple return values for remote RTT "
                                   "operation '" +
                                   description_.name + "'");
        }
        return_type_ = output_types_[output_index];
      } else if (source >= 0 &&
                 static_cast<std::size_t>(source) < input_types_.size()) {
        const std::size_t argument_index = static_cast<std::size_t>(source);
        if (output_types_[output_index] != input_types_[argument_index] ||
            !mutable_arguments.insert(argument_index).second) {
          throw std::runtime_error("inconsistent mutable output for remote "
                                   "RTT operation '" +
                                   description_.name + "'");
        }
      } else {
        throw std::runtime_error("invalid output source for remote RTT "
                                 "operation '" +
                                 description_.name + "'");
      }
    }
    if (return_type_ == nullptr) {
      return_type_ = RTT::types::Types()->type("Void");
    }
    if (return_type_ == nullptr) {
      throw std::runtime_error("RTT Void type is not registered");
    }

    arguments_.reserve(input_types_.size());
    for (std::size_t index = 0U; index < input_types_.size(); ++index) {
      std::string name = index < description_.input_names.size() &&
                                 !description_.input_names[index].empty()
                             ? description_.input_names[index]
                             : "argument" + std::to_string(index + 1U);
      std::string description = index < description_.input_descriptions.size()
                                    ? description_.input_descriptions[index]
                                    : std::string{};
      std::string type = input_types_[index]->getTypeName();
      if (mutable_arguments.contains(index)) {
        type += " &";
      }
      arguments_.emplace_back(std::move(name), std::move(description),
                              std::move(type));
    }
    mutable_arguments_ = std::move(mutable_arguments);
  }

  std::string getName() const override { return description_.name; }

  std::string description() const override { return description_.description; }

  std::vector<RTT::ArgumentDescription> getArgumentList() const override {
    return arguments_;
  }

  std::string resultType() const override {
    return return_type_->getTypeName();
  }

  unsigned int arity() const override {
    return static_cast<unsigned int>(input_types_.size());
  }

  const RTT::types::TypeInfo *
  getArgumentType(unsigned int argument) const override {
    if (argument == 0U) {
      return return_type_;
    }
    if (argument > input_types_.size()) {
      return nullptr;
    }
    return input_types_[argument - 1U];
  }

  unsigned int collectArity() const override {
    return static_cast<unsigned int>(output_types_.size());
  }

  const RTT::types::TypeInfo *
  getCollectType(unsigned int argument) const override {
    if (argument == 0U || argument > output_types_.size()) {
      return nullptr;
    }
    return output_types_[argument - 1U];
  }

  DataSource::shared_ptr produce(const DataSourceList &arguments,
                                 RTT::ExecutionEngine *) const override {
    validateArguments(arguments);
    DataSource::shared_ptr return_value;
    if (return_type_->getTypeName() != "Void") {
      return_value = return_type_->buildValue();
      if (!return_value) {
        throw std::runtime_error("failed to allocate remote RTT return value");
      }
    }
    auto *action = new RemoteCallAction(
        session_, type_registry_, description_.object_id,
        description_.method_id, arguments, description_.output_types,
        description_.output_sources, return_value);
    if (!return_value) {
      return new RTT::internal::DataSourceCommand(action);
    }
    DataSource::shared_ptr call =
        return_type_->buildActionAlias(action, return_value);
    if (!call) {
      delete action;
      throw std::runtime_error("failed to build remote RTT call data source");
    }
    return call;
  }

  DataSource::shared_ptr produceSend(const DataSourceList &arguments,
                                     RTT::ExecutionEngine *) const override {
    validateArguments(arguments);
    InvocationDataSource::shared_ptr handle =
        new RTT::internal::ValueDataSource<RemoteInvocationPtr>();
    return new RTT::internal::ActionAliasAssignableDataSource<
        RemoteInvocationPtr>(
        new RemoteSendAction(session_, type_registry_, description_.object_id,
                             description_.method_id, arguments, handle),
        handle.get());
  }

  DataSource::shared_ptr produceHandle() const override {
    return new RTT::internal::ValueDataSource<RemoteInvocationPtr>();
  }

  DataSource::shared_ptr produceCollect(
      const DataSourceList &arguments,
      RTT::internal::DataSource<bool>::shared_ptr blocking) const override {
    const std::size_t expected = output_types_.size() + 1U;
    if (arguments.size() != expected) {
      throw RTT::wrong_number_of_args_exception(
          static_cast<int>(expected), static_cast<int>(arguments.size()));
    }
    auto *handle = RTT::internal::DataSource<RemoteInvocationPtr>::narrow(
        arguments.front().get());
    if (handle == nullptr) {
      throw RTT::wrong_types_of_args_exception(
          0, "OPC UA remote send handle", arguments.front()->getTypeName());
    }

    DataSourceList destinations(arguments.begin() + 1, arguments.end());
    validateDestinations(destinations);
    return new RemoteCollectDataSource(
        type_registry_, handle->get(), std::move(destinations),
        description_.output_types, std::move(blocking));
  }

#ifdef ORO_SIGNALLING_OPERATIONS
  RTT::Handle produceSignal(RTT::base::ActionInterface *,
                            const DataSourceList &,
                            RTT::ExecutionEngine *) const override {
    return RTT::Handle();
  }
#endif

private:
  void validateArguments(const DataSourceList &arguments) const {
    if (arguments.size() != input_types_.size()) {
      throw RTT::wrong_number_of_args_exception(
          static_cast<int>(input_types_.size()),
          static_cast<int>(arguments.size()));
    }
    for (std::size_t index = 0U; index < arguments.size(); ++index) {
      const std::string received =
          arguments[index] ? arguments[index]->getTypeInfo()->getTypeName()
                           : std::string("null");
      const std::string expected = input_types_[index]->getTypeName();
      if (!arguments[index] || received != expected ||
          type_registry_->codecForDataSource(arguments[index]) == nullptr) {
        throw RTT::wrong_types_of_args_exception(static_cast<int>(index + 1U),
                                                 expected, received);
      }
      if (mutable_arguments_.contains(index) &&
          !arguments[index]->isAssignable()) {
        throw RTT::non_lvalue_args_exception(static_cast<int>(index + 1U),
                                             received);
      }
    }
  }

  void validateDestinations(const DataSourceList &destinations) const {
    for (std::size_t index = 0U; index < destinations.size(); ++index) {
      const std::string received =
          destinations[index]
              ? destinations[index]->getTypeInfo()->getTypeName()
              : std::string("null");
      const std::string expected = output_types_[index]->getTypeName();
      if (!destinations[index] || !destinations[index]->isAssignable() ||
          received != expected) {
        throw RTT::wrong_types_of_args_exception(static_cast<int>(index + 1U),
                                                 expected, received);
      }
    }
  }

  std::shared_ptr<ClientSession> session_;
  std::shared_ptr<const EndpointTypeRegistry> type_registry_;
  RemoteOperationDescription description_;
  std::vector<const RTT::types::TypeInfo *> input_types_;
  std::vector<const RTT::types::TypeInfo *> output_types_;
  const RTT::types::TypeInfo *return_type_{nullptr};
  std::set<std::size_t> mutable_arguments_;
  std::vector<RTT::ArgumentDescription> arguments_;
};

} // namespace

RTT::OperationInterfacePart *
makeRemoteOperation(std::shared_ptr<ClientSession> session,
                    RemoteOperationDescription description) {
  return new RemoteOperation(std::move(session), std::move(description));
}

} // namespace RTT::opcua::detail
