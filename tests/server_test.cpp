#define BOOST_TEST_MODULE rtt_opcua_server
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/server.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/nodemanagement.hpp>
#include <open62541pp/ua/nodeids.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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

} // namespace

BOOST_AUTO_TEST_CASE(non_loopback_server_options_are_rejected_as_unsupported) {
  for (const std::string &bind_address :
       {std::string("0.0.0.0"), std::string("192.0.2.1"),
        std::string("::")}) {
    RTT::opcua::ServerOptions options;
    options.bind_address = bind_address;

    const std::optional<std::string> error =
        RTT::opcua::validateServerOptions(options);
    BOOST_REQUIRE(error.has_value());
    BOOST_TEST(*error == "non-loopback OPC UA listening is not supported");
  }
}

BOOST_AUTO_TEST_CASE(invalid_server_options_fail_without_starting_a_thread) {
  RTT::opcua::ServerOptions options;
  options.bind_address = "0.0.0.0";
  RTT::opcua::Server server(options);

  std::string error;
  BOOST_TEST(!server.start(&error));
  BOOST_TEST(!error.empty());
  BOOST_CHECK(server.state() == RTT::opcua::ServerState::failed);
  BOOST_TEST(!server.isRunning());
  server.stop();
  BOOST_CHECK(server.state() == RTT::opcua::ServerState::stopped);
}

BOOST_AUTO_TEST_CASE(loopback_server_exposes_namespace_and_serialized_tasks) {
  RTT::opcua::ServerOptions options;
  options.port = unusedLoopbackPort();
  RTT::opcua::Server server(options);

  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);
  BOOST_TEST(server.isRunning());
  BOOST_CHECK(server.state() == RTT::opcua::ServerState::running);
  BOOST_REQUIRE(server.namespaceIndex().has_value());
  const auto namespace_index = *server.namespaceIndex();
  BOOST_TEST(namespace_index != 0U);

  BOOST_REQUIRE_MESSAGE(
      server.invoke(
          [namespace_index](::opcua::Server &native) {
            const auto result = ::opcua::services::addObject(
                native, ::opcua::ObjectId::ObjectsFolder,
                ::opcua::NodeId(namespace_index, "rtt/test-object"),
                "TestObject", ::opcua::ObjectAttributes{},
                ::opcua::ObjectTypeId::BaseObjectType,
                ::opcua::ReferenceTypeId::Organizes);
            if (!result) {
              throw ::opcua::BadStatus(result.code());
            }
          },
          std::chrono::seconds(2), &error),
      error);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(server.endpointUrl());

  const auto namespaces = client.namespaceArray();
  BOOST_TEST(std::any_of(namespaces.begin(), namespaces.end(),
                         [](const ::opcua::String &uri) {
                           return uri == RTT::opcua::kNamespaceUri;
                         }));

  const auto display_name = ::opcua::services::readDisplayName(
      client, ::opcua::NodeId(namespace_index, "rtt/test-object"));
  BOOST_REQUIRE(display_name);
  BOOST_TEST(display_name.value().text() == "TestObject");

  BOOST_TEST(!server.invoke(
      [](::opcua::Server &) {
        throw std::runtime_error("expected task failure");
      },
      std::chrono::seconds(1), &error));
  BOOST_TEST(error == "expected task failure");
  BOOST_TEST(server.isRunning());

  std::atomic_bool blocking_task_started{false};
  std::atomic_int cancelled_side_effect{0};
  BOOST_REQUIRE(server.post([&blocking_task_started](::opcua::Server &) {
    blocking_task_started.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }));
  while (!blocking_task_started.load()) {
    std::this_thread::yield();
  }
  BOOST_TEST(!server.invoke(
      [&cancelled_side_effect](::opcua::Server &) { ++cancelled_side_effect; },
      std::chrono::milliseconds(10), &error));
  BOOST_TEST(error == "OPC UA server task timed out");
  BOOST_REQUIRE(
      server.invoke([](::opcua::Server &) {}, std::chrono::seconds(1), &error));
  BOOST_TEST(cancelled_side_effect.load() == 0);

  client.disconnect();
  server.stop();
  BOOST_CHECK(server.state() == RTT::opcua::ServerState::stopped);
  BOOST_TEST(!server.isRunning());
  server.stop();
}
