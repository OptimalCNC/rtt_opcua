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
#include <rtt/types/Types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

} // namespace

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
  component.addProperty("Gain", gain).doc("Controller gain");
  component.addAttribute("Status", status);

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
  const auto status_id = modelNodeId(
      namespace_index, {"components", "arm/left", "attributes", "Status"});
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
  BOOST_CHECK(::opcua::services::writeValue(client, gain_id,
                                            ::opcua::Variant(std::int32_t{11}))
                  .isGood());
  BOOST_TEST(gain == 11);

  const auto status_value = ::opcua::services::readValue(client, status_id);
  BOOST_REQUIRE(status_value);
  BOOST_TEST(status_value.value().to<std::string>() == "idle");
  BOOST_TEST(!::opcua::services::writeValue(
                  client, status_id, ::opcua::Variant(std::string("unsafe")))
                  .isGood());
  BOOST_TEST(status == "idle");

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
