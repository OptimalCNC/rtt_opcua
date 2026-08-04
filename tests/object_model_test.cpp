#define BOOST_TEST_MODULE rtt_opcua_object_model
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/method.hpp>

#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(RTT::opcua::UnsupportedResource)

namespace {

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

bool waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

::opcua::NodeId modelNodeId(std::uint16_t namespace_index,
                            std::initializer_list<std::string_view> segments) {
  const std::vector<std::string_view> path_segments(segments);
  return ::opcua::NodeId(namespace_index,
                         RTT::opcua::makeNodePath(path_segments));
}

struct CanonicalTypesFixture {
  CanonicalTypesFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  }
};

class OperationComponent final : public RTT::TaskContext {
public:
  OperationComponent() : RTT::TaskContext("calculator") {
    addOperation("add", &OperationComponent::add, this, RTT::OwnThread)
        .doc("Add two signed values.")
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");
    addOperation("increment", &OperationComponent::increment, this,
                 RTT::OwnThread)
        .doc("Increment a value in place.")
        .arg("value", "Value to increment.");
    addOperation("onOwnerThread", &OperationComponent::onOwnerThread, this,
                 RTT::OwnThread)
        .doc("Report whether RTT dispatched to the component engine.");
    addOperation("onGlobalEngine", &OperationComponent::onGlobalEngine, this,
                 RTT::ClientThread)
        .doc("Report whether RTT dispatched to the global engine.");
    addOperation("slow", &OperationComponent::slow, this, RTT::OwnThread)
        .doc("Sleep for the requested duration.")
        .arg("milliseconds", "Sleep duration in milliseconds.");
  }

  std::int32_t add(std::int32_t left, std::int32_t right) {
    return left + right;
  }

  void increment(std::int32_t &value) { ++value; }

  bool onOwnerThread() const { return engine()->isSelf(); }

  bool onGlobalEngine() const {
    return RTT::internal::GlobalEngine::Instance()->isSelf();
  }

  bool slow(std::uint32_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    slow_completed.store(true);
    return true;
  }

  std::atomic_bool slow_completed{false};
};

struct UnsupportedValue {
  std::int32_t value{0};
};

class CanonicalArrayComponent final : public RTT::TaskContext {
public:
  CanonicalArrayComponent() : RTT::TaskContext("canonical-arrays") {
    addProperty("Float64ArrayProperty", float64_property);
    addProperty("Int32ArrayProperty", int32_property);
    addProperty("StringArrayProperty", string_property);
    addAttribute("Float64ArrayAttribute", float64_attribute);
    addAttribute("Int32ArrayAttribute", int32_attribute);
    addAttribute("StringArrayAttribute", string_attribute);
  }

  std::vector<double> float64_property{1.0, 2.0};
  std::vector<std::int32_t> int32_property{3, 4};
  std::vector<std::string> string_property{"five", "six"};
  std::vector<double> float64_attribute{7.0, 8.0};
  std::vector<std::int32_t> int32_attribute{9, 10};
  std::vector<std::string> string_attribute{"eleven", "twelve"};
};

constexpr std::string_view kUnsupportedTypeName = "/test/UnsupportedValue";
constexpr std::string_view kMissingProtocolReason =
    "has no registered OPC UA protocol";

void registerUnsupportedValueType() {
  if (RTT::types::Types()->type(std::string(kUnsupportedTypeName)) != nullptr) {
    return;
  }
  BOOST_REQUIRE(RTT::types::Types()->addType(
      new RTT::types::TemplateTypeInfo<UnsupportedValue, false>(
          std::string(kUnsupportedTypeName))));
}

class UnsupportedResourceComponent final : public RTT::TaskContext {
public:
  UnsupportedResourceComponent()
      : RTT::TaskContext("unsupported-component"),
        unsupported_service(RTT::Service::Create("unsupported")) {
    unsupported_service
        ->addOperation("consume", &UnsupportedResourceComponent::consume, this,
                       RTT::ClientThread)
        .arg("value", "Unsupported input value.");
    unsupported_service->addOperation("produce",
                                      &UnsupportedResourceComponent::produce,
                                      this, RTT::ClientThread);
    unsupported_service->addProperty("UnsupportedProperty", property);
    unsupported_service->addAttribute("UnsupportedAttribute", attribute);
    unsupported_service->addPort(input);
    unsupported_service->addPort(output);
    provides()->addProperty("SupportedProperty", supported_property);
    BOOST_REQUIRE(provides()->addService(unsupported_service));
  }

  bool consume(UnsupportedValue) const { return true; }
  UnsupportedValue produce() const { return UnsupportedValue{42}; }

  RTT::Service::shared_ptr unsupported_service;
  UnsupportedValue property{1};
  UnsupportedValue attribute{2};
  std::int32_t supported_property{3};
  RTT::InputPort<UnsupportedValue> input{"UnsupportedInput"};
  RTT::OutputPort<UnsupportedValue> output{"UnsupportedOutput"};
};

} // namespace

BOOST_FIXTURE_TEST_CASE(canonical_array_value_nodes_publish_and_remain_writable,
                        CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModel model(server);
  CanonicalArrayComponent component;
  auto registration = model.registerComponent(component, &error);
  BOOST_REQUIRE_MESSAGE(registration.has_value(), error);

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto float64_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "Float64ArrayProperty"});
  const auto string_attribute_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "attributes", "StringArrayAttribute"});
  BOOST_TEST(::opcua::services::readValue(client, float64_property_id)
                     .value()
                     .to<std::vector<double>>() == component.float64_property,
             boost::test_tools::per_element());
  BOOST_TEST(
      ::opcua::services::writeValue(
          client, string_attribute_id,
          ::opcua::Variant(std::vector<std::string>{"updated", "attribute"}))
          .isGood());
  BOOST_REQUIRE(waitUntil([&] {
    return component.string_attribute ==
           std::vector<std::string>{"updated", "attribute"};
  }));

  client.disconnect();
  registration->reset();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    unsupported_resources_reject_the_complete_initial_component,
    CanonicalTypesFixture) {
  registerUnsupportedValueType();

  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  std::vector<std::string> messages;
  RTT::opcua::ObjectModelOptions model_options;
  model_options.reconcile_interval = std::chrono::milliseconds(20);
  model_options.warning_sink = [&messages](const std::string &message) {
    messages.push_back(message);
  };
  RTT::opcua::ObjectModel model(server, model_options);

  UnsupportedResourceComponent component;
  const std::uint64_t revision_before = model.revision();
  std::vector<RTT::opcua::UnsupportedResource> diagnostics;
  auto registration = model.registerComponent(component, &error, &diagnostics);
  BOOST_TEST(!registration.has_value());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(model.revision() == revision_before);
  BOOST_TEST(diagnostics.size() == 6U);
  BOOST_REQUIRE_EQUAL(diagnostics.size(), 6U);
  BOOST_TEST(model.unsupportedResources(component.getName()) == diagnostics,
             boost::test_tools::per_element());
  BOOST_TEST(std::ranges::is_sorted(diagnostics));
  BOOST_TEST(messages.size() == diagnostics.size());
  BOOST_TEST(error.starts_with("strict OPC UA publication rejected component"));

  const std::vector<std::pair<std::string, std::string>> expected_resources{
      {"unsupported.UnsupportedAttribute", "attribute"},
      {"unsupported.UnsupportedInput", "input port"},
      {"unsupported.UnsupportedOutput", "output port"},
      {"unsupported.UnsupportedProperty", "property"},
      {"unsupported.consume", "operation"},
      {"unsupported.produce", "operation"},
  };
  for (std::size_t index = 0U; index < diagnostics.size(); ++index) {
    const auto &diagnostic = diagnostics[index];
    BOOST_TEST(diagnostic.component == component.getName());
    BOOST_TEST(diagnostic.path == expected_resources[index].first);
    BOOST_TEST(diagnostic.kind == expected_resources[index].second);
    BOOST_TEST(diagnostic.type_name == kUnsupportedTypeName);
    BOOST_TEST(diagnostic.reason == kMissingProtocolReason);
    BOOST_TEST(messages[index] == diagnostic.message());
  }

  ::opcua::Client client;
  client.connect(server.endpointUrl());
  const auto component_root_id =
      modelNodeId(namespace_index, {"components", component.getName()});
  const auto supported_property_id =
      modelNodeId(namespace_index, {"components", component.getName(),
                                    "properties", "SupportedProperty"});
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_root_id));
  BOOST_TEST(
      !::opcua::services::readBrowseName(client, supported_property_id));

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    component_metadata_reconciles_and_registration_guards_lifetime,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions model_options;
  model_options.reconcile_interval = std::chrono::milliseconds(20);
  RTT::opcua::ObjectModel model(server, model_options);

  RTT::TaskContext component("arm/left");
  std::int32_t gain = 7;
  std::string status = "idle";
  std::string model_name = "arm-v1";
  component.addProperty("Gain", gain).doc("Controller gain");
  component.addAttribute("Status", status);
  component.addConstant("ModelName", model_name);

  RTT::OutputPort<double> feedback("Feedback");
  RTT::InputPort<std::uint16_t> command("Command");
  component.addPort(feedback).doc("Measured feedback");
  component.addPort(command).doc("Requested command");

  RTT::Service::shared_ptr motion =
      RTT::Service::Create("motion/raw", &component);
  std::uint16_t scale = 2U;
  motion->addProperty("Scale", scale).doc("Motion scale");

  auto registration = model.registerComponent(component, &error);
  BOOST_REQUIRE_MESSAGE(registration.has_value(), error);
  BOOST_TEST(registration->name() == "arm/left");
  BOOST_TEST(model.componentCount() == 1U);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());

  const auto component_id =
      modelNodeId(namespace_index, {"components", "arm/left"});
  const auto gain_id = modelNodeId(
      namespace_index, {"components", "arm/left", "properties", "Gain"});
  const auto gain_type_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "properties", "Gain", "rttType"});
  const auto status_id = modelNodeId(
      namespace_index, {"components", "arm/left", "attributes", "Status"});
  const auto model_name_id = modelNodeId(
      namespace_index,
      {"components", "arm/left", "attributes", "ModelName"});
  const auto feedback_type_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Feedback", "type"});
  const auto feedback_direction_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Feedback", "direction"});
  const auto command_direction_id =
      modelNodeId(namespace_index,
                  {"components", "arm/left", "ports", "Command", "direction"});
  const auto feedback_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Feedback"});
  const auto feedback_read_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Feedback", "read"});
  const auto command_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Command"});
  const auto command_write_id = modelNodeId(
      namespace_index, {"components", "arm/left", "ports", "Command", "write"});
  const auto service_id = modelNodeId(
      namespace_index, {"components", "arm/left", "services", "motion/raw"});
  const auto revision_id = modelNodeId(namespace_index, {"model", "revision"});

  const auto component_name =
      ::opcua::services::readBrowseName(client, component_id);
  BOOST_REQUIRE(component_name);
  BOOST_TEST(component_name.value().name() == "arm/left");

  const auto gain_value = ::opcua::services::readValue(client, gain_id);
  BOOST_REQUIRE(gain_value);
  BOOST_TEST(gain_value.value().to<std::int32_t>() == 7);
  const auto gain_type = ::opcua::services::readValue(client, gain_type_id);
  BOOST_REQUIRE(gain_type);
  BOOST_TEST(gain_type.value().to<std::string>() == "Int32");
  BOOST_CHECK(::opcua::services::writeValue(client, gain_id,
                                            ::opcua::Variant(std::int32_t{11}))
                  .isGood());
  BOOST_TEST(gain == 11);

  const auto status_value = ::opcua::services::readValue(client, status_id);
  BOOST_REQUIRE(status_value);
  BOOST_TEST(status_value.value().to<std::string>() == "idle");
  BOOST_CHECK(::opcua::services::writeValue(
                  client, status_id, ::opcua::Variant(std::string("active")))
                  .isGood());
  BOOST_TEST(status == "active");

  const auto wrong_status_write = ::opcua::services::writeValue(
      client, status_id, ::opcua::Variant(std::int32_t{42}));
  BOOST_TEST(wrong_status_write.get() == UA_STATUSCODE_BADTYPEMISMATCH);
  BOOST_TEST(status == "active");

  const auto model_name_value =
      ::opcua::services::readValue(client, model_name_id);
  BOOST_REQUIRE(model_name_value);
  BOOST_TEST(model_name_value.value().to<std::string>() == "arm-v1");
  const auto model_name_write = ::opcua::services::writeValue(
      client, model_name_id, ::opcua::Variant(std::string("unsafe")));
  BOOST_TEST(model_name_write.get() == UA_STATUSCODE_BADNOTWRITABLE);
  BOOST_TEST(model_name == "arm-v1");

  BOOST_TEST(::opcua::services::readValue(client, feedback_type_id)
                 .value()
                 .to<std::string>() == "Float64");
  BOOST_TEST(::opcua::services::readValue(client, feedback_direction_id)
                 .value()
                 .to<std::string>() == "output");
  BOOST_TEST(::opcua::services::readValue(client, command_direction_id)
                 .value()
                 .to<std::string>() == "input");
  BOOST_REQUIRE(::opcua::services::readBrowseName(client, service_id));

  const auto empty_feedback_result =
      ::opcua::services::call(client, feedback_id, feedback_read_id, {});
  BOOST_REQUIRE(empty_feedback_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(empty_feedback_result.outputArguments().size(), 2U);
  BOOST_TEST(empty_feedback_result.outputArguments()[0].to<std::string>() ==
             "NoData");

  BOOST_TEST(feedback.write(4.25) == RTT::WriteSuccess);
  const auto feedback_result =
      ::opcua::services::call(client, feedback_id, feedback_read_id, {});
  BOOST_REQUIRE(feedback_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(feedback_result.outputArguments().size(), 2U);
  BOOST_TEST(feedback_result.outputArguments()[0].to<std::string>() ==
             "NewData");
  BOOST_TEST(feedback_result.outputArguments()[1].to<double>() == 4.25);

  const auto feedback_old_result =
      ::opcua::services::call(client, feedback_id, feedback_read_id, {});
  BOOST_REQUIRE(feedback_old_result.statusCode().isGood());
  BOOST_TEST(feedback_old_result.outputArguments()[0].to<std::string>() ==
             "OldData");
  BOOST_TEST(feedback_old_result.outputArguments()[1].to<double>() == 4.25);

  const std::vector<::opcua::Variant> wrong_command_inputs{
      ::opcua::Variant(std::string("not-an-integer"))};
  const auto wrong_command_result = ::opcua::services::call(
      client, command_id, command_write_id, wrong_command_inputs);
  BOOST_TEST(wrong_command_result.statusCode().isBad());
  std::uint16_t commanded_value = 0U;
  BOOST_TEST(command.read(commanded_value) == RTT::NoData);

  const std::vector<::opcua::Variant> command_inputs{
      ::opcua::Variant(std::uint16_t{73})};
  const auto command_result = ::opcua::services::call(
      client, command_id, command_write_id, command_inputs);
  BOOST_REQUIRE(command_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(command_result.outputArguments().size(), 1U);
  BOOST_TEST(command_result.outputArguments()[0].to<std::string>() ==
             "WriteSuccess");
  BOOST_TEST(command.read(commanded_value) == RTT::NewData);
  BOOST_TEST(commanded_value == 73U);

  const std::uint64_t first_revision =
      ::opcua::services::readValue(client, revision_id)
          .value()
          .to<std::uint64_t>();
  BOOST_TEST(first_revision > 0U);

  std::int32_t dynamic_value = 23;
  RTT::Property<std::int32_t> &dynamic =
      component.addProperty("Dynamic", dynamic_value);
  const auto dynamic_id = modelNodeId(
      namespace_index, {"components", "arm/left", "properties", "Dynamic"});
  BOOST_REQUIRE(waitUntil([&] {
    const auto value = ::opcua::services::readValue(client, dynamic_id);
    return static_cast<bool>(value) && value.value().to<std::int32_t>() == 23;
  }));
  BOOST_TEST(::opcua::services::readValue(client, revision_id)
                 .value()
                 .to<std::uint64_t>() > first_revision);

  BOOST_REQUIRE(component.provides()->removeProperty(dynamic));
  BOOST_REQUIRE(waitUntil(
      [&] { return !::opcua::services::readValue(client, dynamic_id); }));

  registration->reset();
  BOOST_TEST(!registration->active());
  BOOST_TEST(model.componentCount() == 0U);
  BOOST_TEST(!::opcua::services::readBrowseName(client, component_id));
  BOOST_TEST(!feedback.connected());
  BOOST_TEST(!command.connected());

  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    operations_dispatch_on_rtt_engines_and_retain_timed_out_calls,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions model_options;
  model_options.reconcile_interval = std::chrono::milliseconds(10);
  model_options.operation_timeout = std::chrono::milliseconds(30);
  RTT::opcua::ObjectModel model(server, model_options);

  OperationComponent component;
  auto registration = model.registerComponent(component, &error);
  BOOST_REQUIRE_MESSAGE(registration.has_value(), error);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());

  const auto operations_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations"});
  const auto add_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "add"});
  const auto increment_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "increment"});
  const auto add_input_types_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttInputTypes"});
  const auto add_output_types_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttOutputTypes"});
  const auto add_output_sources_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "add", "rttOutputSources"});
  const auto increment_output_sources_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations",
                                    "increment", "rttOutputSources"});
  const auto owner_thread_id =
      modelNodeId(namespace_index,
                  {"components", "calculator", "operations", "onOwnerThread"});
  const auto global_engine_id =
      modelNodeId(namespace_index,
                  {"components", "calculator", "operations", "onGlobalEngine"});
  const auto slow_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "slow"});

  const std::vector<::opcua::Variant> add_inputs{
      ::opcua::Variant(std::int32_t{20}), ::opcua::Variant(std::int32_t{22})};
  BOOST_TEST(::opcua::services::readValue(client, add_input_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"Int32", "Int32"}));
  BOOST_TEST(::opcua::services::readValue(client, add_output_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"Int32"}));
  BOOST_TEST(::opcua::services::readValue(client, add_output_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({-1}));
  BOOST_TEST(::opcua::services::readValue(client, increment_output_sources_id)
                 .value()
                 .to<std::vector<std::int32_t>>() ==
             std::vector<std::int32_t>({0}));
  const auto add_result =
      ::opcua::services::call(client, operations_id, add_id, add_inputs);
  BOOST_REQUIRE(add_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(add_result.outputArguments().size(), 1U);
  BOOST_TEST(add_result.outputArguments()[0].to<std::int32_t>() == 42);

  const std::vector<::opcua::Variant> increment_inputs{
      ::opcua::Variant(std::int32_t{4})};
  const auto increment_result = ::opcua::services::call(
      client, operations_id, increment_id, increment_inputs);
  BOOST_REQUIRE(increment_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(increment_result.outputArguments().size(), 1U);
  BOOST_TEST(increment_result.outputArguments()[0].to<std::int32_t>() == 5);

  const auto owner_thread_result =
      ::opcua::services::call(client, operations_id, owner_thread_id, {});
  BOOST_REQUIRE(owner_thread_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(owner_thread_result.outputArguments().size(), 1U);
  BOOST_TEST(owner_thread_result.outputArguments()[0].to<bool>());

  const auto global_engine_result =
      ::opcua::services::call(client, operations_id, global_engine_id, {});
  BOOST_REQUIRE(global_engine_result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(global_engine_result.outputArguments().size(), 1U);
  BOOST_TEST(global_engine_result.outputArguments()[0].to<bool>());

  const std::vector<::opcua::Variant> wrong_inputs{
      ::opcua::Variant(std::string("not-an-integer")),
      ::opcua::Variant(std::int32_t{2})};
  const auto wrong_result =
      ::opcua::services::call(client, operations_id, add_id, wrong_inputs);
  BOOST_TEST(wrong_result.statusCode() == UA_STATUSCODE_BADINVALIDARGUMENT);

  const std::vector<::opcua::Variant> slow_inputs{
      ::opcua::Variant(std::uint32_t{120})};
  const auto started_at = std::chrono::steady_clock::now();
  const auto slow_result =
      ::opcua::services::call(client, operations_id, slow_id, slow_inputs);
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  BOOST_TEST(slow_result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(elapsed < std::chrono::milliseconds(100));
  BOOST_TEST(model.pendingOperationCount() == 1U);
  BOOST_REQUIRE(waitUntil([&] {
    return component.slow_completed.load() &&
           model.pendingOperationCount() == 0U;
  }));

  registration->reset();
  client.disconnect();
  server.stop();
}

BOOST_FIXTURE_TEST_CASE(
    object_model_shutdown_waits_for_timed_out_own_thread_operations,
    CanonicalTypesFixture) {
  RTT::opcua::ServerOptions server_options;
  server_options.port = unusedLoopbackPort();
  RTT::opcua::Server server(server_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  const std::uint16_t namespace_index = *server.namespaceIndex();

  RTT::opcua::ObjectModelOptions model_options;
  model_options.reconcile_interval = std::chrono::milliseconds(10);
  model_options.operation_timeout = std::chrono::milliseconds(30);
  auto model = std::make_unique<RTT::opcua::ObjectModel>(server, model_options);

  OperationComponent component;
  auto registration = model->registerComponent(component, &error);
  BOOST_REQUIRE_MESSAGE(registration.has_value(), error);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());

  const auto operations_id =
      modelNodeId(namespace_index, {"components", "calculator", "operations"});
  const auto slow_id = modelNodeId(
      namespace_index, {"components", "calculator", "operations", "slow"});
  const std::vector<::opcua::Variant> inputs{
      ::opcua::Variant(std::uint32_t{200})};

  const auto result =
      ::opcua::services::call(client, operations_id, slow_id, inputs);
  BOOST_TEST(result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(model->pendingOperationCount() == 1U);

  client.disconnect();
  model.reset();

  BOOST_TEST(component.slow_completed.load());
  BOOST_TEST(!registration->active());
  registration->reset();
  server.stop();
}
