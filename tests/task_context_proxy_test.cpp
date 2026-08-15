#define BOOST_TEST_MODULE rtt_opcua_task_context_proxy
#include <boost/test/included/unit_test.hpp>

#include "custom_datatype_test_support.hpp"

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/port_direction.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <rtt/FactoryExceptions.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Property.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/internal/OperationCallerC.hpp>
#include <rtt/internal/SendHandleC.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/Types.hpp>

#include <open62541pp/exception.hpp>
#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/method.hpp>
#include <open62541pp/services/nodemanagement.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using RTT::opcua::test::FixtureValue;

std::uint16_t unusedLoopbackPort() {
  const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) {
    throw std::runtime_error("failed to create test socket");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    ::close(socket_fd);
    throw std::runtime_error("failed to bind test socket");
  }

  socklen_t size = sizeof(address);
  if (::getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &size) !=
      0) {
    ::close(socket_fd);
    throw std::runtime_error("failed to inspect test socket");
  }
  const std::uint16_t port = ntohs(address.sin_port);
  ::close(socket_fd);
  return port;
}

::opcua::NodeId modelNodeId(std::uint16_t namespace_index,
                            std::initializer_list<std::string_view> segments) {
  const std::vector<std::string_view> path_segments(segments);
  return ::opcua::NodeId(namespace_index,
                         RTT::opcua::makeNodePath(path_segments));
}

::opcua::NodeId modelNodeId(std::uint16_t namespace_index,
                            const std::vector<std::string_view> &segments) {
  return ::opcua::NodeId(namespace_index, RTT::opcua::makeNodePath(segments));
}

void removeNodeIfPresent(::opcua::Server &server, const ::opcua::NodeId &id) {
  const ::opcua::StatusCode status =
      ::opcua::services::deleteNode(server, id, true);
  if (!status.isGood() && status.get() != UA_STATUSCODE_BADNODEIDUNKNOWN) {
    throw ::opcua::BadStatus(status);
  }
}

void replaceDirectionMetadata(
    RTT::opcua::Server &server, std::uint16_t namespace_index,
    std::string_view component_name, std::string_view port_name,
    ::opcua::Variant value, ::opcua::NodeId data_type,
    ::opcua::ValueRank value_rank) {
  const auto port_id = modelNodeId(
      namespace_index,
      {"components", component_name, "ports", port_name});
  const auto direction_id = modelNodeId(
      namespace_index,
      {"components", component_name, "ports", port_name, "direction"});
  bool replaced = false;
  std::string error;
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&, value = std::move(value), data_type = std::move(data_type),
           value_rank](
              ::opcua::Server &native) mutable {
            const auto deleted =
                ::opcua::services::deleteNode(native, direction_id, true);
            if (!deleted.isGood()) {
              throw ::opcua::BadStatus(deleted);
            }
            ::opcua::VariableAttributes attributes;
            attributes.setDisplayName(
                ::opcua::LocalizedText("en-US", "direction"));
            attributes.setDescription(::opcua::LocalizedText(
                "en-US", "RTT port direction."));
            attributes.setValue(std::move(value));
            attributes.setDataType(data_type);
            attributes.setValueRank(value_rank);
            if (value_rank == ::opcua::ValueRank::OneDimension) {
              attributes.setArrayDimensions({0U});
            }
            attributes.setAccessLevel(::opcua::AccessLevel::CurrentRead);
            attributes.setUserAccessLevel(
                ::opcua::AccessLevel::CurrentRead);
            replaced = static_cast<bool>(::opcua::services::addVariable(
                native, port_id, direction_id, "direction", attributes,
                ::opcua::VariableTypeId::BaseDataVariableType,
                ::opcua::ReferenceTypeId::HasComponent));
          },
          std::chrono::seconds(1), &error),
      error);
  BOOST_REQUIRE(replaced);
}

void replacePortValueVariable(RTT::opcua::Server &server,
                              std::uint16_t namespace_index,
                              std::string_view component_name,
                              std::string_view port_name,
                              ::opcua::Variant value, ::opcua::NodeId data_type,
                              ::opcua::ValueRank value_rank,
                              ::opcua::Bitmask<::opcua::AccessLevel> access) {
  const auto port_id = modelNodeId(
      namespace_index, {"components", component_name, "ports", port_name});
  const auto value_id =
      modelNodeId(namespace_index,
                  {"components", component_name, "ports", port_name, "value"});
  bool replaced = false;
  std::string error;
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&, value = std::move(value), data_type = std::move(data_type),
           value_rank, access](::opcua::Server &native) mutable {
            const auto deleted =
                ::opcua::services::deleteNode(native, value_id, true);
            if (!deleted.isGood()) {
              throw ::opcua::BadStatus(deleted);
            }
            ::opcua::VariableAttributes attributes;
            attributes.setDisplayName(::opcua::LocalizedText("en-US", "value"));
            attributes.setValue(std::move(value));
            attributes.setDataType(data_type);
            attributes.setValueRank(value_rank);
            if (value_rank == ::opcua::ValueRank::OneDimension) {
              attributes.setArrayDimensions({0U});
            }
            attributes.setAccessLevel(access);
            attributes.setUserAccessLevel(access);
            replaced = static_cast<bool>(::opcua::services::addVariable(
                native, port_id, value_id, "value", attributes,
                ::opcua::VariableTypeId::BaseDataVariableType,
                ::opcua::ReferenceTypeId::HasComponent));
          },
          std::chrono::seconds(1), &error),
      error);
  BOOST_REQUIRE(replaced);
}

template <typename T>
void addArrayMetadata(::opcua::Server &server, const ::opcua::NodeId &parent,
                      const ::opcua::NodeId &id, std::string_view name,
                      std::vector<T> values,
                      const ::opcua::NodeId &data_type) {
  ::opcua::VariableAttributes attributes;
  attributes.setDisplayName(::opcua::LocalizedText("en-US", name));
  attributes.setValue(::opcua::Variant(std::move(values)));
  attributes.setDataType(data_type);
  attributes.setValueRank(::opcua::ValueRank::OneDimension);
  attributes.setArrayDimensions({0U});
  attributes.setAccessLevel(::opcua::AccessLevel::CurrentRead);
  attributes.setUserAccessLevel(::opcua::AccessLevel::CurrentRead);
  const auto added = ::opcua::services::addVariable(
      server, parent, id, name, attributes,
      ::opcua::VariableTypeId::BaseDataVariableType,
      ::opcua::ReferenceTypeId::HasProperty);
  if (!added) {
    throw ::opcua::BadStatus(added.code());
  }
}

void replaceStateGetterWithStringResult(
    RTT::opcua::Server &server, std::uint16_t namespace_index,
    std::string_view component_name, std::string_view operation_name) {
  const auto operations_id = modelNodeId(
      namespace_index, {"components", component_name, "operations"});
  const auto method_id = modelNodeId(
      namespace_index,
      {"components", component_name, "operations", operation_name});
  const auto output_types_id = modelNodeId(
      namespace_index, {"components", component_name, "operations",
                        operation_name, "rttOutputTypes"});
  const auto output_sources_id = modelNodeId(
      namespace_index, {"components", component_name, "operations",
                        operation_name, "rttOutputSources"});
  bool replaced = false;
  std::string error;
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&](::opcua::Server &native) {
            removeNodeIfPresent(native, output_types_id);
            removeNodeIfPresent(native, output_sources_id);
            removeNodeIfPresent(native, method_id);

            ::opcua::MethodAttributes attributes;
            attributes.setDisplayName(
                ::opcua::LocalizedText("en-US", operation_name));
            attributes.setExecutable(true);
            attributes.setUserExecutable(true);
            ::opcua::services::MethodCallback callback =
                std::function<::opcua::StatusCode(
                    ::opcua::Session &,
                    ::opcua::Span<const ::opcua::Variant>,
                    ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
                    const ::opcua::NodeId &)>(
                    [](::opcua::Session &,
                       ::opcua::Span<const ::opcua::Variant>,
                       ::opcua::Span<::opcua::Variant> outputs,
                       const ::opcua::NodeId &, const ::opcua::NodeId &) {
                      if (outputs.size() != 1U) {
                        return ::opcua::StatusCode(
                            UA_STATUSCODE_BADINVALIDARGUMENT);
                      }
                      outputs[0] = ::opcua::Variant(std::string("invalid"));
                      return ::opcua::StatusCode(UA_STATUSCODE_GOOD);
                    });
            const std::vector<::opcua::Argument> outputs{
                ::opcua::Argument(
                    "result",
                    ::opcua::LocalizedText("en-US", "Invalid state result."),
                    ::opcua::DataTypeId::String, ::opcua::ValueRank::Scalar)};
            const auto method = ::opcua::services::addMethod(
                native, operations_id, method_id, operation_name,
                std::move(callback), {}, outputs, attributes,
                ::opcua::ReferenceTypeId::HasComponent);
            if (!method) {
              throw ::opcua::BadStatus(method.code());
            }
            addArrayMetadata(native, method_id, output_types_id,
                             "rttOutputTypes",
                             std::vector<std::string>{"String"},
                             ::opcua::DataTypeId::String);
            addArrayMetadata(native, method_id, output_sources_id,
                             "rttOutputSources",
                             std::vector<std::int32_t>{-1},
                             ::opcua::DataTypeId::Int32);
            replaced = true;
          },
          std::chrono::seconds(1), &error),
      error);
  BOOST_REQUIRE(replaced);
}

void replaceStateGetterCallbackWithCode(
    RTT::opcua::Server &server, std::uint16_t namespace_index,
    std::string_view component_name, std::string_view operation_name,
    std::int32_t code) {
  const auto method_id = modelNodeId(
      namespace_index,
      {"components", component_name, "operations", operation_name});
  bool replaced = false;
  std::string error;
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&](::opcua::Server &native) {
            ::opcua::services::MethodCallback callback =
                std::function<::opcua::StatusCode(
                    ::opcua::Session &,
                    ::opcua::Span<const ::opcua::Variant>,
                    ::opcua::Span<::opcua::Variant>, const ::opcua::NodeId &,
                    const ::opcua::NodeId &)>(
                    [code](::opcua::Session &,
                           ::opcua::Span<const ::opcua::Variant>,
                           ::opcua::Span<::opcua::Variant> outputs,
                           const ::opcua::NodeId &, const ::opcua::NodeId &) {
                      if (outputs.size() != 1U) {
                        return ::opcua::StatusCode(
                            UA_STATUSCODE_BADINVALIDARGUMENT);
                      }
                      outputs[0] = ::opcua::Variant(code);
                      return ::opcua::StatusCode(UA_STATUSCODE_GOOD);
                    });
            const ::opcua::StatusCode status =
                ::opcua::services::setMethodCallback(native, method_id,
                                                     std::move(callback));
            if (!status.isGood()) {
              throw ::opcua::BadStatus(status);
            }
            replaced = true;
          },
          std::chrono::seconds(1), &error),
      error);
  BOOST_REQUIRE(replaced);
}

void ensureEmptyCategory(::opcua::Server &server, const ::opcua::NodeId &parent,
                         const ::opcua::NodeId &id,
                         std::string_view browse_name) {
  const auto existing = ::opcua::services::readNodeClass(server, id);
  if (existing) {
    return;
  }
  if (existing.code().get() != UA_STATUSCODE_BADNODEIDUNKNOWN) {
    throw ::opcua::BadStatus(existing.code());
  }
  ::opcua::ObjectAttributes attributes;
  attributes.setDisplayName(::opcua::LocalizedText("en-US", browse_name));
  const auto added =
      ::opcua::services::addObject(server, parent, id, browse_name, attributes,
                                   ::opcua::ObjectTypeId::BaseObjectType,
                                   ::opcua::ReferenceTypeId::HasComponent);
  if (!added) {
    throw ::opcua::BadStatus(added.code());
  }
}

void removeCategories(::opcua::Server &server, std::uint16_t namespace_index,
                      const std::vector<std::string_view> &base,
                      std::initializer_list<std::string_view> categories) {
  for (const std::string_view category : categories) {
    auto path = base;
    path.push_back(category);
    removeNodeIfPresent(server, modelNodeId(namespace_index, path));
  }
}

void ensureDenseCategories(::opcua::Server &server,
                           std::uint16_t namespace_index,
                           const std::vector<std::string_view> &base) {
  const auto parent = modelNodeId(namespace_index, base);
  for (const auto &[segment, browse_name] :
       std::array<std::pair<std::string_view, std::string_view>, 5U>{{
           {"operations", "Operations"},
           {"properties", "Properties"},
           {"attributes", "Attributes"},
           {"ports", "Ports"},
           {"services", "Services"},
       }}) {
    auto path = base;
    path.push_back(segment);
    ensureEmptyCategory(server, parent, modelNodeId(namespace_index, path),
                        browse_name);
  }
}

struct CanonicalTypesFixture {
  CanonicalTypesFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  }
};

struct CustomDatatypeFixture {
  CustomDatatypeFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    std::string error;
    if (!RTT::opcua::registerCanonicalTypeProtocols(&error) ||
        !RTT::opcua::test::registerFixtureType(&error)) {
      throw std::runtime_error(error);
    }
  }
};

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout =
                                        std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

class ProxyTarget final : public RTT::TaskContext {
public:
  ProxyTarget()
      : RTT::TaskContext("remote/calculator",
                         RTT::TaskContext::PreOperational) {
    provides()->doc("Remote calculator.");
    addPort(feedback).doc("Calculated feedback.");
    addPort(command).doc("Requested command.");
    addProperty("Gain", gain).doc("Controller gain.");
    addAttribute("Status", status);
    addConstant("ModelName", model_name);
    addOperation("add", &ProxyTarget::add, this, RTT::OwnThread)
        .doc("Add two signed values.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");
    addOperation("increment", &ProxyTarget::increment, this, RTT::OwnThread)
        .doc("Increment a value in place.")
        .arg("value", "Value to increment.");
    addOperation("delayedAdd", &ProxyTarget::delayedAdd, this, RTT::OwnThread)
        .doc("Add two values after a short delay.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");

    RTT::Service::shared_ptr math = RTT::Service::Create("math");
    math->doc("Math utilities.");
    math->addPort(math_feedback).doc("Nested calculation feedback.");
    math->addProperty("Offset", offset).doc("Scale offset.");
    math->addAttribute("Mode", mode);
    math->addConstant("Unit", unit);
    math->addOperation("scale", &ProxyTarget::scale, this, RTT::OwnThread)
        .doc("Scale a value and add the configured offset.")
        .arg("value", "Value to scale.")
        .arg("factor", "Scale factor.");
    RTT::Service::shared_ptr advanced = RTT::Service::Create("advanced");
    advanced->addOperation("negate", &ProxyTarget::negate, this, RTT::OwnThread)
        .doc("Negate a signed value.")
        .arg("value", "Value to negate.");
    BOOST_REQUIRE(math->addService(advanced));
    BOOST_REQUIRE(provides()->addService(math));

    RTT::Service::shared_ptr sparse_empty =
        RTT::Service::Create("sparse_empty");
    sparse_empty->doc("Intentionally sparse empty service.");
    BOOST_REQUIRE(provides()->addService(sparse_empty));

    RTT::Service::shared_ptr dense_empty = RTT::Service::Create("dense_empty");
    dense_empty->doc("Legacy dense empty service.");
    BOOST_REQUIRE(provides()->addService(dense_empty));
  }

  ~ProxyTarget() override {
    if (RTT::Service::shared_ptr math = provides()->getService("math")) {
      math->removePort(math_feedback.getName());
      math->clear();
    }
  }

  std::int32_t add(std::int32_t left, std::int32_t right) {
    return left + right;
  }

  void increment(std::int32_t &value) { ++value; }

  std::int32_t delayedAdd(std::int32_t left, std::int32_t right) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return left + right;
  }

  std::int32_t scale(std::int32_t value, std::int32_t factor) {
    return value * factor + offset;
  }

  std::int32_t negate(std::int32_t value) { return -value; }

  std::int32_t gain{7};
  std::int32_t offset{2};
  std::string status{"idle"};
  std::string model_name{"calculator-v1"};
  std::string mode{"manual"};
  std::string unit{"counts"};
  RTT::OutputPort<std::int32_t> feedback{"Feedback"};
  RTT::InputPort<std::int32_t> command{"Command"};
  RTT::OutputPort<std::int32_t> math_feedback{"MathFeedback"};
};

class CustomProxyTarget final : public RTT::TaskContext {
public:
  CustomProxyTarget()
      : RTT::TaskContext("remote/custom", RTT::TaskContext::PreOperational) {
    addProperty("Configured", configured);
    addAttribute("Observed", observed);
    addPort(feedback);
    addPort(command);
    addOperation("adjust", &CustomProxyTarget::adjust, this, RTT::OwnThread)
        .arg("value", "Value to adjust.");
  }

  FixtureValue adjust(FixtureValue value) {
    ++value.count;
    value.scale *= 2.0;
    return value;
  }

  FixtureValue configured{3, 1.5};
  FixtureValue observed{4, 2.5};
  RTT::OutputPort<FixtureValue> feedback{"Feedback"};
  RTT::InputPort<FixtureValue> command{"Command"};
};

class SparseRootTarget final : public RTT::TaskContext {
public:
  SparseRootTarget()
      : RTT::TaskContext("remote/sparse-root",
                         RTT::TaskContext::PreOperational) {}
};

class NonRetainingProxyTarget final : public RTT::TaskContext {
public:
  NonRetainingProxyTarget()
      : RTT::TaskContext("remote/non-retaining",
                         RTT::TaskContext::PreOperational),
        output("Ephemeral", false) {
    addPort(output);
  }

  RTT::OutputPort<std::int32_t> output;
};

class DivergentLifecycleTarget final : public RTT::TaskContext {
public:
  DivergentLifecycleTarget()
      : RTT::TaskContext("remote/divergent-lifecycle",
                         RTT::TaskContext::PreOperational) {}

  TaskState getTaskState() const override { return Init; }
  TaskState getTargetState() const override { return Running; }
  bool isConfigured() const override { return true; }
  bool isActive() const override { return false; }
  bool isRunning() const override { return true; }
  bool inFatalError() const override { return false; }
  bool inException() const override { return true; }
  bool inRunTimeError() const override { return false; }
};

} // namespace

BOOST_GLOBAL_FIXTURE(CustomDatatypeFixture);

BOOST_FIXTURE_TEST_CASE(proxy_calls_each_native_lifecycle_operation,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  DivergentLifecycleTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Init);
  BOOST_TEST(proxy->getTargetState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->isConfigured());
  BOOST_TEST(!proxy->isActive());
  BOOST_TEST(proxy->isRunning());
  BOOST_TEST(!proxy->inFatalError());
  BOOST_TEST(proxy->inException());
  BOOST_TEST(!proxy->inRunTimeError());

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    proxy_reconstructs_a_selected_interface_with_the_mandatory_baseline,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  const std::vector<std::string> selectors{
      "operations/add",
      "properties/Gain",
      "ports/Command",
      "services/math/operations/scale",
      "services/math/properties/Offset",
  };
  BOOST_REQUIRE_MESSAGE(
      model.publishComponentSelected(target, selectors, &error), error);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  for (const std::string_view operation :
       {"configure", "start", "stop", "cleanup"}) {
    const auto node = ::opcua::services::readNodeClass(
        client, modelNodeId(*server.namespaceIndex(),
                            {"components", target.getName(), "operations",
                             operation}));
    BOOST_TEST(!static_cast<bool>(node));
    BOOST_TEST(node.code().get() == UA_STATUSCODE_BADNODEIDUNKNOWN);
  }
  client.disconnect();

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(proxy->getTargetState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(!proxy->isConfigured());
  BOOST_TEST(proxy->isActive());
  BOOST_TEST(!proxy->isRunning());
  BOOST_TEST(!proxy->inFatalError());
  BOOST_TEST(!proxy->inException());
  BOOST_TEST(!proxy->inRunTimeError());

  RTT::OperationInterfacePart *add = proxy->provides()->getOperation("add");
  BOOST_REQUIRE(add != nullptr);
  std::int32_t sum = 0;
  RTT::internal::OperationCallerC add_caller(
      add, "add", RTT::internal::GlobalEngine::Instance());
  add_caller.argC(std::int32_t{4}).argC(std::int32_t{5}).ret(sum);
  add_caller.check();
  BOOST_REQUIRE(add_caller.call());
  BOOST_TEST(sum == 9);

  auto *gain = dynamic_cast<RTT::Property<std::int32_t> *>(
      proxy->provides()->getProperty("Gain"));
  BOOST_REQUIRE(gain != nullptr);
  BOOST_TEST(gain->get() == 7);
  gain->set(11);
  BOOST_TEST(target.gain == 11);

  auto *command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(command != nullptr);
  RTT::OutputPort<std::int32_t> command_source("SelectedCommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *command, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(std::int32_t{23}) == RTT::WriteSuccess);
  std::int32_t command_value = 0;
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value == 23);

  RTT::Service::shared_ptr math = proxy->provides()->getService("math");
  BOOST_REQUIRE(math);
  RTT::OperationInterfacePart *scale = math->getOperation("scale");
  BOOST_REQUIRE(scale != nullptr);
  std::int32_t scaled = 0;
  RTT::internal::OperationCallerC scale_caller(
      scale, "scale", RTT::internal::GlobalEngine::Instance());
  scale_caller.argC(std::int32_t{4}).argC(std::int32_t{5}).ret(scaled);
  scale_caller.check();
  BOOST_REQUIRE(scale_caller.call());
  BOOST_TEST(scaled == 22);
  auto *offset =
      dynamic_cast<RTT::Property<std::int32_t> *>(math->getProperty("Offset"));
  BOOST_REQUIRE(offset != nullptr);
  BOOST_TEST(offset->get() == 2);

  BOOST_TEST(proxy->provides()->getOperation("increment") == nullptr);
  BOOST_TEST(proxy->provides()->getAttribute("Status") == nullptr);
  BOOST_TEST(proxy->provides()->getAttribute("ModelName") == nullptr);
  BOOST_TEST(proxy->ports()->getPort("Feedback") == nullptr);
  BOOST_TEST(proxy->provides()->getService("Feedback") == nullptr);
  BOOST_TEST(math->getService("advanced") == nullptr);
  BOOST_TEST(math->getAttribute("Mode") == nullptr);
  BOOST_TEST(math->getAttribute("Unit") == nullptr);
  BOOST_TEST(math->getPort("MathFeedback") == nullptr);
  BOOST_TEST(math->getService("MathFeedback") == nullptr);

  command_source.disconnect();
  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_requires_both_native_state_getters,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  SparseRootTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  const auto method_id = modelNodeId(
      *server.namespaceIndex(),
      {"components", target.getName(), "operations", "getTargetState"});
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&](::opcua::Server &native) {
            removeNodeIfPresent(native, method_id);
          },
          std::chrono::seconds(1), &error),
      error);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("getTargetState") != std::string::npos);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_incompatible_native_state_schema,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  SparseRootTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  replaceStateGetterWithStringResult(
      server, *server.namespaceIndex(), target.getName(), "getTargetState");

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("getTargetState") != std::string::npos);
  BOOST_TEST(error.find("TaskState") != std::string::npos);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_out_of_range_native_task_state,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  SparseRootTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  replaceStateGetterCallbackWithCode(
      server, *server.namespaceIndex(), target.getName(), "getTaskState", 7);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Init);
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::stale);
  BOOST_TEST(proxy->lastError().find("getTaskState") != std::string::npos);
  BOOST_TEST(proxy->lastError().find("7") != std::string::npos);

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_ready_marks_server_loss_stale,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  SparseRootTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ready());

  server.stop();
  BOOST_TEST(!proxy->ready());
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::stale);
}

BOOST_FIXTURE_TEST_CASE(proxy_calls_remote_operations_synchronously_and_async,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getName() == target.getName());
  BOOST_TEST(proxy->provides()->doc() == "Remote calculator.");
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::connected);

  RTT::Service::shared_ptr feedback_service =
      proxy->provides()->getService("Feedback");
  RTT::Service::shared_ptr command_service =
      proxy->provides()->getService("Command");
  BOOST_REQUIRE(feedback_service);
  BOOST_REQUIRE(command_service);
  BOOST_REQUIRE(proxy->ports()->getPort("Feedback") != nullptr);
  BOOST_REQUIRE(proxy->ports()->getPort("Command") != nullptr);
  BOOST_REQUIRE(feedback_service->getOperation("last") != nullptr);
  BOOST_REQUIRE(command_service->getOperation("read") != nullptr);

  BOOST_TEST(proxy->isActive());
  BOOST_TEST(proxy->activate());
  BOOST_TEST(proxy->getPeriod() == target.getPeriod());
  BOOST_TEST(proxy->setPeriod(0.0));
  BOOST_TEST(proxy->setPeriod(0.01));
  BOOST_TEST(proxy->getPeriod() == 0.01, boost::test_tools::tolerance(0.001));
  BOOST_TEST(target.getPeriod() == 0.01, boost::test_tools::tolerance(0.001));
  BOOST_TEST(proxy->setPeriod(0.0));
  BOOST_TEST(proxy->getCpuAffinity() == target.getCpuAffinity());
  BOOST_TEST(!proxy->update());
  BOOST_TEST(proxy->trigger());

  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(!proxy->isConfigured());
  BOOST_TEST(!proxy->isRunning());
  BOOST_TEST(proxy->configure());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->isConfigured());
  BOOST_TEST(proxy->start());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->getTargetState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->isRunning());
  proxy->error();
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::RunTimeError);
  BOOST_TEST(proxy->inRunTimeError());
  BOOST_TEST(proxy->recover());
  BOOST_TEST(target.getTaskState() == RTT::TaskContext::Running);
  BOOST_TEST(proxy->stop());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::Stopped);
  BOOST_TEST(proxy->cleanup());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);

  auto *gain = dynamic_cast<RTT::Property<std::int32_t> *>(
      proxy->provides()->getProperty("Gain"));
  BOOST_REQUIRE(gain != nullptr);
  BOOST_TEST(gain->getDescription() == "Controller gain.");
  BOOST_TEST(gain->get() == 7);
  target.gain = 9;
  BOOST_TEST(gain->get() == 9);
  gain->set(11);
  BOOST_TEST(target.gain == 11);

  RTT::base::AttributeBase *status = proxy->provides()->getAttribute("Status");
  BOOST_REQUIRE(status != nullptr);
  auto *status_source =
      RTT::internal::AssignableDataSource<std::string>::narrow(
          status->getDataSource().get());
  BOOST_REQUIRE(status_source != nullptr);
  BOOST_TEST(status_source->get() == "idle");
  BOOST_REQUIRE(proxy->configure());
  BOOST_REQUIRE(proxy->start());
  status_source->set("remote-running");
  BOOST_TEST(target.status == "remote-running");
  BOOST_REQUIRE(proxy->stop());
  BOOST_REQUIRE(proxy->cleanup());
  target.status = "controller-update";
  BOOST_TEST(status_source->get() == "controller-update");

  RTT::base::AttributeBase *model_name =
      proxy->provides()->getAttribute("ModelName");
  BOOST_REQUIRE(model_name != nullptr);
  BOOST_TEST(!model_name->getDataSource()->isAssignable());
  auto *model_name_source = RTT::internal::DataSource<std::string>::narrow(
      model_name->getDataSource().get());
  BOOST_REQUIRE(model_name_source != nullptr);
  BOOST_TEST(model_name_source->get() == "calculator-v1");

  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  BOOST_TEST(remote_feedback->getTypeInfo()->getTypeName() == "Int32");
  BOOST_TEST(remote_feedback->getDescription() == "Calculated feedback.");
  RTT::InputPort<std::int32_t> feedback_sink("FeedbackSink");
  RTT::ConnPolicy feedback_policy =
      RTT::ConnPolicy::buffer(1, RTT::ConnPolicy::LOCK_FREE, false);
  feedback_policy.mandatory = true;
  BOOST_REQUIRE(
      remote_feedback->createConnection(feedback_sink, feedback_policy));
  const RTT::base::DataSourceBase::shared_ptr filler =
      new RTT::internal::ConstantDataSource<std::int32_t>(40);
  BOOST_REQUIRE(remote_feedback->write(filler) == RTT::WriteSuccess);
  BOOST_TEST(target.feedback.write(std::int32_t{41}) == RTT::WriteSuccess);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::int32_t feedback_value = 0;
  BOOST_REQUIRE(feedback_sink.read(feedback_value) == RTT::NewData);
  BOOST_TEST(feedback_value == 40);
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 41);

  std::int32_t generated_last_value = 0;
  RTT::OperationInterfacePart *generated_last =
      feedback_service->getOperation("last");
  RTT::internal::OperationCallerC generated_last_caller(
      generated_last, "last", RTT::internal::GlobalEngine::Instance());
  generated_last_caller.ret(generated_last_value);
  generated_last_caller.check();
  BOOST_REQUIRE(generated_last_caller.call());
  BOOST_TEST(generated_last_value == 41);
  RTT::InputPort<std::int32_t> initialized_feedback_sink(
      "InitializedFeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      initialized_feedback_sink,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, true)));
  std::int32_t initialized_feedback_value = 0;
  BOOST_REQUIRE(initialized_feedback_sink.read(initialized_feedback_value) ==
                RTT::NewData);
  BOOST_TEST(initialized_feedback_value == 41);
  initialized_feedback_sink.disconnect();
  feedback_sink.disconnect();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  BOOST_REQUIRE(
      remote_feedback->createConnection(feedback_sink, feedback_policy));
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 41);

  auto *remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_TEST(remote_command->getTypeInfo()->getTypeName() == "Int32");
  BOOST_TEST(remote_command->getDescription() == "Requested command.");
  RTT::OutputPort<std::int32_t> command_source("CommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(std::int32_t{73}) == RTT::WriteSuccess);
  std::int32_t command_value = 0;
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value == 73);

  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(!feedback_sink.connected());
  BOOST_TEST(!command_source.connected());

  remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  BOOST_REQUIRE(remote_feedback->createConnection(
      feedback_sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.feedback.write(std::int32_t{42}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 42);

  remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(std::int32_t{74}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value == 74);

  RTT::Service::shared_ptr math = proxy->provides()->getService("math");
  BOOST_REQUIRE(math);
  BOOST_TEST(math->doc() == "Math utilities.");
  auto *remote_math_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      math->getPort("MathFeedback"));
  BOOST_REQUIRE(remote_math_feedback != nullptr);
  RTT::Service::shared_ptr math_feedback_service =
      math->getService("MathFeedback");
  BOOST_REQUIRE(math_feedback_service);
  BOOST_REQUIRE(math_feedback_service->getOperation("last") != nullptr);
  BOOST_TEST(remote_math_feedback->getDescription() ==
             "Nested calculation feedback.");
  RTT::InputPort<std::int32_t> math_feedback_sink("MathFeedbackSink");
  BOOST_REQUIRE(remote_math_feedback->createConnection(
      math_feedback_sink,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.math_feedback.write(std::int32_t{84}) == RTT::WriteSuccess);
  std::int32_t math_feedback_value = 0;
  BOOST_REQUIRE(waitUntil([&] {
    return math_feedback_sink.read(math_feedback_value) == RTT::NewData;
  }));
  BOOST_TEST(math_feedback_value == 84);
  auto *offset =
      dynamic_cast<RTT::Property<std::int32_t> *>(math->getProperty("Offset"));
  BOOST_REQUIRE(offset != nullptr);
  RTT::base::DataSourceBase::shared_ptr cached_offset_source =
      offset->getDataSource();
  auto *cached_offset =
      RTT::internal::AssignableDataSource<std::int32_t>::narrow(
          cached_offset_source.get());
  BOOST_REQUIRE(cached_offset != nullptr);
  BOOST_TEST(offset->getDescription() == "Scale offset.");
  BOOST_TEST(offset->get() == 2);
  offset->set(3);
  BOOST_TEST(target.offset == 3);

  RTT::base::AttributeBase *mode = math->getAttribute("Mode");
  BOOST_REQUIRE(mode != nullptr);
  auto *mode_source = RTT::internal::AssignableDataSource<std::string>::narrow(
      mode->getDataSource().get());
  BOOST_REQUIRE(mode_source != nullptr);
  mode_source->set("automatic");
  BOOST_TEST(target.mode == "automatic");

  RTT::base::AttributeBase *unit = math->getAttribute("Unit");
  BOOST_REQUIRE(unit != nullptr);
  BOOST_TEST(!unit->getDataSource()->isAssignable());
  auto *unit_source = RTT::internal::DataSource<std::string>::narrow(
      unit->getDataSource().get());
  BOOST_REQUIRE(unit_source != nullptr);
  BOOST_TEST(unit_source->get() == "counts");

  std::int32_t scaled = 0;
  RTT::OperationInterfacePart *scale = math->getOperation("scale");
  BOOST_REQUIRE(scale != nullptr);
  RTT::internal::OperationCallerC scale_caller(
      scale, "scale", RTT::internal::GlobalEngine::Instance());
  scale_caller.argC(std::int32_t{4}).argC(std::int32_t{5}).ret(scaled);
  scale_caller.check();
  BOOST_TEST(scale_caller.call());
  BOOST_TEST(scaled == 23);

  RTT::Service::shared_ptr advanced = math->getService("advanced");
  BOOST_REQUIRE(advanced);
  std::int32_t negated = 0;
  RTT::OperationInterfacePart *negate = advanced->getOperation("negate");
  BOOST_REQUIRE(negate != nullptr);
  RTT::internal::OperationCallerC negate_caller(
      negate, "negate", RTT::internal::GlobalEngine::Instance());
  negate_caller.argC(std::int32_t{9}).ret(negated);
  negate_caller.check();
  BOOST_TEST(negate_caller.call());
  BOOST_TEST(negated == -9);

  RTT::OperationInterfacePart *add = proxy->provides()->getOperation("add");
  BOOST_REQUIRE(add != nullptr);
  const std::vector<RTT::ArgumentDescription> add_arguments =
      add->getArgumentList();
  BOOST_REQUIRE_EQUAL(add_arguments.size(), 2U);
  BOOST_TEST(add_arguments[0].name == "left");
  BOOST_TEST(add_arguments[0].description == "Left operand.");
  BOOST_TEST(add_arguments[0].type == "Int32");
  BOOST_TEST(add_arguments[1].name == "right");
  BOOST_TEST(add_arguments[1].description == "Right operand.");
  BOOST_TEST(add_arguments[1].type == "Int32");
  const std::vector<RTT::base::DataSourceBase::shared_ptr> too_few_arguments{
      new RTT::internal::ConstantDataSource<std::int32_t>(1)};
  BOOST_CHECK_THROW(
      add->produce(too_few_arguments, RTT::internal::GlobalEngine::Instance()),
      RTT::wrong_number_of_args_exception);
  const std::vector<RTT::base::DataSourceBase::shared_ptr> wrong_arguments{
      new RTT::internal::ConstantDataSource<double>(1.0),
      new RTT::internal::ConstantDataSource<std::int32_t>(2)};
  BOOST_CHECK_THROW(
      add->produce(wrong_arguments, RTT::internal::GlobalEngine::Instance()),
      RTT::wrong_types_of_args_exception);

  std::int32_t sum = 0;
  RTT::internal::OperationCallerC add_caller(
      add, "add", RTT::internal::GlobalEngine::Instance());
  add_caller.argC(std::int32_t{20}).argC(std::int32_t{22}).ret(sum);
  add_caller.check();
  BOOST_REQUIRE(add_caller.ready());
  BOOST_TEST(add_caller.call());
  BOOST_TEST(sum == 42);

  const std::vector<RTT::base::DataSourceBase::shared_ptr> cached_add_arguments{
      new RTT::internal::ConstantDataSource<std::int32_t>(30),
      new RTT::internal::ConstantDataSource<std::int32_t>(12)};
  RTT::base::DataSourceBase::shared_ptr cached_add_call = add->produce(
      cached_add_arguments, RTT::internal::GlobalEngine::Instance());
  BOOST_REQUIRE(cached_add_call);

  RTT::OperationInterfacePart *increment =
      proxy->provides()->getOperation("increment");
  BOOST_REQUIRE(increment != nullptr);
  const std::vector<RTT::ArgumentDescription> increment_arguments =
      increment->getArgumentList();
  BOOST_REQUIRE_EQUAL(increment_arguments.size(), 1U);
  BOOST_TEST(increment_arguments[0].name == "value");
  BOOST_TEST(increment_arguments[0].description == "Value to increment.");
  BOOST_TEST(increment_arguments[0].type == "Int32 &");
  const std::vector<RTT::base::DataSourceBase::shared_ptr>
      constant_increment_argument{
          new RTT::internal::ConstantDataSource<std::int32_t>(4)};
  BOOST_CHECK_THROW(increment->produce(constant_increment_argument,
                                       RTT::internal::GlobalEngine::Instance()),
                    RTT::non_lvalue_args_exception);

  std::int32_t value = 4;
  RTT::internal::OperationCallerC increment_caller(
      increment, "increment", RTT::internal::GlobalEngine::Instance());
  increment_caller.arg(value);
  increment_caller.check();
  BOOST_TEST(increment_caller.call());
  BOOST_TEST(value == 5);

  std::int32_t async_sum = 0;
  RTT::internal::OperationCallerC async_caller(
      add, "add", RTT::internal::GlobalEngine::Instance());
  async_caller.argC(std::int32_t{8}).argC(std::int32_t{9});
  async_caller.check();
  RTT::internal::SendHandleC handle = async_caller.send();
  handle.arg(async_sum);
  handle.check();
  BOOST_REQUIRE(handle.ready());
  BOOST_TEST(handle.collect() == RTT::SendSuccess);
  BOOST_TEST(async_sum == 17);

  RTT::OperationInterfacePart *delayed_add =
      proxy->provides()->getOperation("delayedAdd");
  BOOST_REQUIRE(delayed_add != nullptr);
  std::int32_t delayed_sum = 0;
  RTT::internal::OperationCallerC delayed_caller(
      delayed_add, "delayedAdd", RTT::internal::GlobalEngine::Instance());
  delayed_caller.argC(std::int32_t{10}).argC(std::int32_t{11});
  delayed_caller.check();
  RTT::internal::SendHandleC delayed_handle = delayed_caller.send();
  delayed_handle.arg(delayed_sum);
  delayed_handle.check();
  BOOST_REQUIRE(delayed_handle.ready());
  BOOST_TEST(delayed_handle.collectIfDone() == RTT::SendNotReady);
  BOOST_TEST(delayed_handle.collect() == RTT::SendSuccess);
  BOOST_TEST(delayed_sum == 21);

  target.command.disconnect();
  BOOST_TEST(command_source.write(std::int32_t{75}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil([&] {
    return proxy->lastError().find("NotConnected") != std::string::npos;
  }));
  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(proxy->lastError().find("NotConnected") != std::string::npos);

  BOOST_TEST(cached_add_call->evaluate());
  target.offset = 3;
  cached_offset->set(99);
  BOOST_TEST(target.offset == 99);
  BOOST_TEST(!waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; },
      std::chrono::milliseconds(100)));

  constexpr std::size_t synchronize_count = 4U;
  std::barrier synchronize_start(
      static_cast<std::ptrdiff_t>(synchronize_count));
  std::array<bool, synchronize_count> synchronized{};
  std::array<std::string, synchronize_count> synchronize_errors;
  std::vector<std::jthread> synchronizers;
  synchronizers.reserve(synchronize_count);
  std::atomic<bool> control_calls_succeeded{true};
  std::jthread control_caller([&] {
    for (std::size_t call = 0U; call < 20U; ++call) {
      if (!proxy->isActive() ||
          proxy->getTaskState() != RTT::TaskContext::PreOperational) {
        control_calls_succeeded.store(false);
        return;
      }
    }
  });
  for (std::size_t index = 0; index < synchronize_count; ++index) {
    synchronizers.emplace_back([&, index] {
      synchronize_start.arrive_and_wait();
      synchronized[index] = proxy->synchronize(&synchronize_errors[index]);
    });
  }
  synchronizers.clear();
  control_caller.join();
  for (std::size_t index = 0; index < synchronize_count; ++index) {
    BOOST_TEST(synchronized[index], synchronize_errors[index]);
  }
  BOOST_TEST(control_calls_succeeded.load());
  BOOST_TEST(proxy->ready());
  BOOST_REQUIRE(proxy->ports()->getPort("Feedback") != nullptr);

  remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  target.command.disconnect();
  BOOST_TEST(command_source.write(std::int32_t{76}) == RTT::WriteSuccess);
  BOOST_REQUIRE(waitUntil([&] {
    return proxy->lastError().find("NotConnected") != std::string::npos;
  }));

  BOOST_REQUIRE_MESSAGE(proxy->synchronize(&error), error);
  BOOST_TEST(proxy->connectionState() ==
             RTT::opcua::ProxyConnectionState::connected);
  BOOST_TEST(proxy->ready());
  BOOST_TEST(proxy->getTaskState() == RTT::TaskContext::PreOperational);
  BOOST_TEST(!waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; },
      std::chrono::milliseconds(100)));

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_output_mirror_starts_with_latest_retained_value,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  BOOST_TEST(target.feedback.write(std::int32_t{1}) == RTT::WriteSuccess);
  BOOST_TEST(target.feedback.write(std::int32_t{2}) == RTT::WriteSuccess);
  BOOST_TEST(target.feedback.write(std::int32_t{3}) == RTT::WriteSuccess);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  options.port_poll_interval = std::chrono::milliseconds(5);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  RTT::InputPort<std::int32_t> sink("LatestFeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));

  std::int32_t sample = 0;
  BOOST_REQUIRE(waitUntil([&] { return sink.read(sample) == RTT::NewData; }));
  BOOST_TEST(sample == 3);

  sink.disconnect();
  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_omits_non_retaining_output_without_value,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  NonRetainingProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->ports()->getPort("Ephemeral") == nullptr);
  RTT::Service::shared_ptr generated =
      proxy->provides()->getService("Ephemeral");
  BOOST_REQUIRE(generated);
  BOOST_REQUIRE(generated->getOperation("last") != nullptr);

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_round_trips_an_endpoint_bound_custom_datatype,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  server_options.additional_namespace_uris = {"urn:test:unrelated"};
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  CustomProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);

  auto *configured = dynamic_cast<RTT::Property<FixtureValue> *>(
      proxy->provides()->getProperty("Configured"));
  BOOST_REQUIRE(configured != nullptr);
  BOOST_TEST(configured->get().count == 3);
  BOOST_TEST(configured->get().scale == 1.5);
  configured->set({8, 4.5});
  BOOST_TEST(target.configured.count == 8);
  BOOST_TEST(target.configured.scale == 4.5);

  RTT::base::AttributeBase *observed =
      proxy->provides()->getAttribute("Observed");
  BOOST_REQUIRE(observed != nullptr);
  auto *observed_source =
      RTT::internal::AssignableDataSource<FixtureValue>::narrow(
          observed->getDataSource().get());
  BOOST_REQUIRE(observed_source != nullptr);
  BOOST_TEST(observed_source->get().count == 4);
  observed_source->set({9, 5.5});
  BOOST_TEST(target.observed.count == 9);
  BOOST_TEST(target.observed.scale == 5.5);

  RTT::OperationInterfacePart *adjust =
      proxy->provides()->getOperation("adjust");
  BOOST_REQUIRE(adjust != nullptr);
  FixtureValue adjusted;
  RTT::internal::OperationCallerC adjust_caller(
      adjust, "adjust", RTT::internal::GlobalEngine::Instance());
  adjust_caller.argC(FixtureValue{10, 3.0}).ret(adjusted);
  adjust_caller.check();
  BOOST_REQUIRE(adjust_caller.call());
  BOOST_TEST(adjusted.count == 11);
  BOOST_TEST(adjusted.scale == 6.0);

  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_feedback != nullptr);
  RTT::InputPort<FixtureValue> feedback_sink("FeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      feedback_sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(target.feedback.write(FixtureValue{12, 6.5}) == RTT::WriteSuccess);
  FixtureValue feedback_value;
  BOOST_REQUIRE(waitUntil(
      [&] { return feedback_sink.read(feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value.count == 12);
  BOOST_TEST(feedback_value.scale == 6.5);

  auto *remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      proxy->ports()->getPort("Command"));
  BOOST_REQUIRE(remote_command != nullptr);
  RTT::OutputPort<FixtureValue> command_source("CommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(command_source.write(FixtureValue{13, 7.5}) == RTT::WriteSuccess);
  FixtureValue command_value;
  BOOST_REQUIRE(waitUntil(
      [&] { return target.command.read(command_value) == RTT::NewData; }));
  BOOST_TEST(command_value.count == 13);
  BOOST_TEST(command_value.scale == 7.5);

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_creation_rejects_a_missing_component,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  RTT::opcua::ObjectModel model(server);
  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), "missing/component", proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(!error.empty());

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    proxy_treats_missing_category_objects_as_empty_collections,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  const std::uint16_t namespace_index = *server.namespaceIndex();
  const std::string component_name = target.getName();
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&](::opcua::Server &native) {
            removeCategories(
                native, namespace_index,
                {"components", component_name, "services", "Command"},
                {"properties", "attributes", "ports", "services"});
            removeCategories(native, namespace_index,
                             {"components", component_name, "services", "math",
                              "services", "advanced"},
                             {"properties", "attributes", "ports", "services"});
            removeCategories(
                native, namespace_index,
                {"components", component_name, "services", "sparse_empty"},
                {"operations", "properties", "attributes", "ports",
                 "services"});
            ensureDenseCategories(
                native, namespace_index,
                {"components", component_name, "services", "dense_empty"});
          },
          std::chrono::seconds(1), &error),
      error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_REQUIRE(proxy->provides()->getService("Command"));
  BOOST_REQUIRE(proxy->provides()->getService("Command")->getOperation("read"));
  BOOST_REQUIRE(proxy->provides()->getService("sparse_empty"));
  BOOST_TEST(proxy->provides()->getService("sparse_empty")->doc() ==
             "Intentionally sparse empty service.");
  BOOST_TEST(proxy->provides()
                 ->getService("sparse_empty")
                 ->getOperationNames()
                 .empty());
  BOOST_TEST(proxy->provides()
                 ->getService("sparse_empty")
                 ->properties()
                 ->getPropertyNames()
                 .empty());
  BOOST_TEST(proxy->provides()
                 ->getService("sparse_empty")
                 ->getAttributeNames()
                 .empty());
  BOOST_TEST(
      proxy->provides()->getService("sparse_empty")->getPortNames().empty());
  BOOST_TEST(proxy->provides()
                 ->getService("sparse_empty")
                 ->getProviderNames()
                 .empty());
  BOOST_REQUIRE(proxy->provides()->getService("dense_empty"));
  BOOST_TEST(proxy->provides()->getService("dense_empty")->doc() ==
             "Legacy dense empty service.");
  BOOST_TEST(proxy->provides()
                 ->getService("dense_empty")
                 ->getOperationNames()
                 .empty());
  BOOST_TEST(proxy->provides()
                 ->getService("dense_empty")
                 ->properties()
                 ->getPropertyNames()
                 .empty());
  BOOST_TEST(proxy->provides()
                 ->getService("dense_empty")
                 ->getAttributeNames()
                 .empty());
  BOOST_TEST(
      proxy->provides()->getService("dense_empty")->getPortNames().empty());
  BOOST_TEST(
      proxy->provides()->getService("dense_empty")->getProviderNames().empty());
  BOOST_REQUIRE(proxy->provides()
                    ->getService("math")
                    ->getService("advanced")
                    ->getOperation("negate"));

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_reconstructs_a_sparse_component_root,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  SparseRootTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  const std::uint16_t namespace_index = *server.namespaceIndex();
  const std::string component_name = target.getName();
  BOOST_REQUIRE_MESSAGE(server.invoke(
                            [&](::opcua::Server &native) {
                              removeCategories(
                                  native, namespace_index,
                                  {"components", component_name},
                                  {"properties", "ports", "services"});
                            },
                            std::chrono::seconds(1), &error),
                        error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  BOOST_TEST(proxy->getName() == target.getName());
  BOOST_TEST(proxy->provides()->properties()->getPropertyNames().empty());
  BOOST_TEST(proxy->ports()->getPortNames().empty());
  BOOST_TEST(proxy->provides()->getProviderNames().empty());

  proxy.reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    proxy_rejects_missing_metadata_inside_a_present_category,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);

  const std::uint16_t namespace_index = *server.namespaceIndex();
  const auto type_id =
      modelNodeId(namespace_index, {"components", target.getName(), "ports",
                                    "Feedback", "type"});
  BOOST_REQUIRE_MESSAGE(server.invoke(
                            [&](::opcua::Server &native) {
                              const ::opcua::StatusCode status =
                                  ::opcua::services::deleteNode(native, type_id,
                                                                true);
                              if (!status.isGood()) {
                                throw ::opcua::BadStatus(status);
                              }
                            },
                            std::chrono::seconds(1), &error),
                        error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("failed to read RTT port type metadata") !=
             std::string::npos);
  BOOST_TEST(error.find("BadNodeIdUnknown") != std::string::npos);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_noncanonical_port_direction_metadata,
                        CanonicalTypesFixture) {
  const auto require_rejected =
      [](::opcua::Variant value, ::opcua::NodeId data_type,
         ::opcua::ValueRank value_rank, std::string_view port_name,
         std::string_view expected_error) {
        RTT::opcua::ServerOptions server_options;
        server_options.port = unusedLoopbackPort();
        RTT::opcua::Server server(server_options);
        std::string error;
        BOOST_REQUIRE_MESSAGE(server.start(&error), error);

        ProxyTarget target;
        RTT::opcua::ObjectModel model(server);
        BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
        replaceDirectionMetadata(
            server, *server.namespaceIndex(), target.getName(), port_name,
            std::move(value), std::move(data_type), value_rank);

        RTT::opcua::TaskContextProxyOptions options;
        options.request_timeout = std::chrono::milliseconds(500);
        auto proxy = RTT::opcua::TaskContextProxy::create(
            server.endpointUrl(), target.getName(), options, &error);
        BOOST_TEST(proxy == nullptr);
        BOOST_TEST(error.find(expected_error) != std::string::npos);
        server.stop();
      };

  require_rejected(::opcua::Variant(std::string("input")),
                   ::opcua::NodeId(::opcua::DataTypeId::String),
                   ::opcua::ValueRank::Scalar, "Command",
                   "expected scalar Int32");
  require_rejected(::opcua::Variant(std::string("output")),
                   ::opcua::NodeId(::opcua::DataTypeId::String),
                   ::opcua::ValueRank::Scalar, "Feedback",
                   "expected scalar Int32");
  require_rejected(::opcua::Variant(std::uint32_t{1U}),
                   ::opcua::NodeId(::opcua::DataTypeId::UInt32),
                   ::opcua::ValueRank::Scalar, "Feedback",
                   "expected scalar Int32");
  require_rejected(::opcua::Variant(1.0),
                   ::opcua::NodeId(::opcua::DataTypeId::Double),
                   ::opcua::ValueRank::Scalar, "Feedback",
                   "expected scalar Int32");
  require_rejected(::opcua::Variant(std::vector<std::int32_t>{1}),
                   ::opcua::NodeId(::opcua::DataTypeId::Int32),
                   ::opcua::ValueRank::OneDimension, "Feedback",
                   "expected scalar Int32");
  require_rejected(::opcua::Variant(std::int32_t{7}),
                   ::opcua::NodeId(::opcua::DataTypeId::Int32),
                   ::opcua::ValueRank::Scalar, "Feedback",
                   "remote port 'Feedback' has unsupported direction code 7");
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_missing_port_direction_metadata,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  const auto direction_id = modelNodeId(
      *server.namespaceIndex(),
      {"components", target.getName(), "ports", "Feedback", "direction"});
  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [&](::opcua::Server &native) {
            const auto status =
                ::opcua::services::deleteNode(native, direction_id, true);
            if (!status.isGood()) {
              throw ::opcua::BadStatus(status);
            }
          },
          std::chrono::seconds(1), &error),
      error);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("failed to read RTT port direction metadata") !=
             std::string::npos);
  BOOST_TEST(error.find("BadNodeIdUnknown") != std::string::npos);

  server.stop();
}

BOOST_AUTO_TEST_CASE(proxy_creation_rejects_an_invalid_port_poll_interval) {
  RTT::opcua::TaskContextProxyOptions options;
  options.port_poll_interval = std::chrono::milliseconds::zero();
  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      "opc.tcp://127.0.0.1:4840", "remote/component", options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error ==
             "OPC UA port poll interval is outside the supported range");
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_an_incompatible_port_value_type,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  replacePortValueVariable(server, *server.namespaceIndex(), target.getName(),
                           "Feedback", ::opcua::Variant(std::string("wrong")),
                           ::opcua::NodeId(::opcua::DataTypeId::String),
                           ::opcua::ValueRank::Scalar,
                           ::opcua::AccessLevel::CurrentRead);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("incompatible value Variable") != std::string::npos);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_incompatible_port_value_access,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  replacePortValueVariable(
      server, *server.namespaceIndex(), target.getName(), "Feedback",
      ::opcua::Variant(std::int32_t{0}),
      ::opcua::NodeId(::opcua::DataTypeId::Int32), ::opcua::ValueRank::Scalar,
      ::opcua::AccessLevel::CurrentRead | ::opcua::AccessLevel::CurrentWrite);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("incompatible value Variable") != std::string::npos);

  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_write_only_input_port_value_access,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  replacePortValueVariable(server, *server.namespaceIndex(), target.getName(),
                           "Command", ::opcua::Variant(std::int32_t{0}),
                           ::opcua::NodeId(::opcua::DataTypeId::Int32),
                           ::opcua::ValueRank::Scalar,
                           ::opcua::AccessLevel::CurrentWrite);

  RTT::opcua::TaskContextProxyOptions options;
  options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("incompatible value Variable") != std::string::npos);
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(proxy_rejects_an_input_port_without_a_value_variable,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  ProxyTarget target;
  RTT::opcua::ObjectModel model(server);
  BOOST_REQUIRE_MESSAGE(model.publishComponent(target, &error), error);
  const auto value_id =
      modelNodeId(*server.namespaceIndex(), {"components", target.getName(),
                                             "ports", "Command", "value"});
  BOOST_REQUIRE_MESSAGE(server.invoke(
                            [&](::opcua::Server &native) {
                              const auto status = ::opcua::services::deleteNode(
                                  native, value_id, true);
                              if (!status.isGood()) {
                                throw ::opcua::BadStatus(status);
                              }
                            },
                            std::chrono::seconds(1), &error),
                        error);

  RTT::opcua::TaskContextProxyOptions proxy_options;
  proxy_options.request_timeout = std::chrono::milliseconds(500);
  auto proxy = RTT::opcua::TaskContextProxy::create(
      server.endpointUrl(), target.getName(), proxy_options, &error);
  BOOST_TEST(proxy == nullptr);
  BOOST_TEST(error.find("remote port 'Command' does not expose its value "
                        "Variable") != std::string::npos);

  server.stop();
}
