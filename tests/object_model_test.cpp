#define BOOST_TEST_MODULE rtt_opcua_object_model
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>

#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/Types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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

  client.disconnect();
  server.stop();
}
