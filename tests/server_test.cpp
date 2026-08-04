#define BOOST_TEST_MODULE rtt_opcua_server
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/datatype_registry.hpp>
#include <rtt/opcua/endpoint_type_registry.hpp>
#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/server.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/datatype.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/nodemanagement.hpp>
#include <open62541pp/services/view.hpp>
#include <open62541pp/ua/nodeids.hpp>
#include <open62541pp/ua/types.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr std::string_view kFixtureNamespaceUri =
    "urn:test:rtt-opcua:server-types";

struct ServerFixtureValue {
  std::int32_t value;
};

struct ServerDatatypeFixture {
  ServerDatatypeFixture() {
    const RTT::opcua::LogicalDataTypeId id{
        std::string(kFixtureNamespaceUri), "types/ServerFixtureValue",
        "encodings/ServerFixtureValue/Binary"};
    RTT::opcua::CustomDataTypeDefinition definition;
    definition.name = "ServerFixtureValue";
    definition.id = id;
    definition.schema_fingerprint = "server-fixture-value-v1";
    definition.materialize =
        [id](const RTT::opcua::DataTypeFactoryContext &context) {
          return ::opcua::DataTypeBuilder<ServerFixtureValue>::createStructure(
                     "ServerFixtureValue", context.nodeId(id),
                     {context.namespaceIndex(id.namespace_uri),
                      id.binary_encoding_node_id})
              .addField<&ServerFixtureValue::value>("value")
              .build();
        };

    RTT::opcua::DataTypeProvider provider;
    provider.name = "server-fixture";
    provider.namespace_uri = std::string(kFixtureNamespaceUri);
    provider.data_types.push_back(std::move(definition));
    std::string error;
    if (!RTT::opcua::registerDataTypeProvider(std::move(provider), &error)) {
      throw std::runtime_error(error);
    }
  }
};

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

BOOST_GLOBAL_FIXTURE(ServerDatatypeFixture);

BOOST_AUTO_TEST_CASE(non_loopback_server_options_are_rejected_as_unsupported) {
  for (const std::string &bind_address :
       {std::string("0.0.0.0"), std::string("192.0.2.1"), std::string("::")}) {
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

BOOST_AUTO_TEST_CASE(
    started_invoke_waits_for_callback_completion_after_timeout) {
  RTT::opcua::ServerOptions options;
  options.port = unusedLoopbackPort();
  RTT::opcua::Server server(options);

  std::string error;
  BOOST_REQUIRE_MESSAGE(server.start(&error), error);

  std::promise<void> callback_started;
  std::future<void> callback_started_future = callback_started.get_future();
  std::promise<void> release_callback;
  std::shared_future<void> release_callback_future =
      release_callback.get_future().share();
  std::string invocation_error;
  std::future<bool> invocation = std::async(std::launch::async, [&] {
    return server.invoke(
        [&callback_started,
         release_callback_future](::opcua::Server &) mutable {
          callback_started.set_value();
          release_callback_future.wait();
        },
        std::chrono::milliseconds(10), &invocation_error);
  });

  BOOST_REQUIRE(static_cast<bool>(
      callback_started_future.wait_for(std::chrono::seconds(1)) ==
      std::future_status::ready));
  BOOST_TEST(static_cast<bool>(
      invocation.wait_for(std::chrono::milliseconds(50)) ==
      std::future_status::timeout));

  release_callback.set_value();
  BOOST_TEST(invocation.get());
  BOOST_TEST(invocation_error.empty());

  server.stop();
}

BOOST_AUTO_TEST_CASE(custom_datatype_binding_follows_endpoint_namespace_order) {
  RTT::opcua::ServerOptions first_options;
  first_options.port = unusedLoopbackPort();
  RTT::opcua::Server first(first_options);

  std::string error;
  BOOST_REQUIRE_MESSAGE(first.start(&error), error);
  const auto first_registry = first.typeRegistry();
  BOOST_REQUIRE(first_registry != nullptr);
  const auto first_provider_index = first.namespaceIndex(kFixtureNamespaceUri);
  BOOST_REQUIRE(first_provider_index.has_value());
  BOOST_REQUIRE(
      first_registry->namespaceIndex(kFixtureNamespaceUri).has_value());
  BOOST_TEST(*first_registry->namespaceIndex(kFixtureNamespaceUri) ==
             *first_provider_index);
  BOOST_REQUIRE(first_registry->customDataTypes().size() == 1U);
  BOOST_CHECK(
      first_registry->customDataTypes().front().typeId() ==
      ::opcua::NodeId(*first_provider_index, "types/ServerFixtureValue"));
  first.stop();
  BOOST_TEST(first.typeRegistry() == nullptr);

  RTT::opcua::ServerOptions second_options;
  second_options.port = unusedLoopbackPort();
  second_options.additional_namespace_uris = {"urn:test:rtt-opcua:unrelated"};
  RTT::opcua::Server second(second_options);
  BOOST_REQUIRE_MESSAGE(second.start(&error), error);
  const auto second_registry = second.typeRegistry();
  BOOST_REQUIRE(second_registry != nullptr);
  const auto second_provider_index =
      second.namespaceIndex(kFixtureNamespaceUri);
  BOOST_REQUIRE(second_provider_index.has_value());
  BOOST_TEST(*second_provider_index != *first_provider_index);
  BOOST_REQUIRE(
      second_registry->namespaceIndex(kFixtureNamespaceUri).has_value());
  BOOST_TEST(*second_registry->namespaceIndex(kFixtureNamespaceUri) ==
             *second_provider_index);
  BOOST_REQUIRE(second_registry->customDataTypes().size() == 1U);
  BOOST_CHECK(
      second_registry->customDataTypes().front().typeId() ==
      ::opcua::NodeId(*second_provider_index, "types/ServerFixtureValue"));

  bool republished = true;
  std::string publication_error;
  BOOST_REQUIRE_MESSAGE(second.invoke(
                            [&second_registry, &republished,
                             &publication_error](::opcua::Server &native) {
                              republished =
                                  second_registry->publishDataTypeNodes(
                                      native, &publication_error);
                            },
                            std::chrono::seconds(2), &error),
                        error);
  BOOST_TEST(!republished);
  BOOST_TEST(publication_error.find("BadNodeIdExists") != std::string::npos);

  ::opcua::ClientConfig client_config;
  client_config.setTimeout(2000U);
  ::opcua::Client client(std::move(client_config));
  client.connect(second.endpointUrl());
  const auto namespaces = client.namespaceArray();
  const auto provider_uri =
      std::find(namespaces.begin(), namespaces.end(), kFixtureNamespaceUri);
  BOOST_REQUIRE(provider_uri != namespaces.end());
  const auto client_provider_index =
      std::distance(namespaces.begin(), provider_uri);
  BOOST_REQUIRE(client_provider_index > 0);
  BOOST_REQUIRE(client_provider_index <=
                std::numeric_limits<std::uint16_t>::max());
  const auto namespace_index =
      static_cast<std::uint16_t>(client_provider_index);
  const ::opcua::NodeId type_id(namespace_index, "types/ServerFixtureValue");
  const ::opcua::NodeId encoding_id(namespace_index,
                                    "encodings/ServerFixtureValue/Binary");

  const auto type_class = ::opcua::services::readNodeClass(client, type_id);
  BOOST_REQUIRE(type_class);
  BOOST_CHECK(type_class.value() == ::opcua::NodeClass::DataType);
  const auto encoding_class =
      ::opcua::services::readNodeClass(client, encoding_id);
  BOOST_REQUIRE(encoding_class);
  BOOST_CHECK(encoding_class.value() == ::opcua::NodeClass::Object);

  const ::opcua::BrowseDescription encoding_browse(
      type_id, ::opcua::BrowseDirection::Forward,
      ::opcua::ReferenceTypeId::HasEncoding, true, ::opcua::NodeClass::Object,
      ::opcua::BrowseResultMask::All);
  const auto encodings = ::opcua::services::browseAll(client, encoding_browse);
  BOOST_REQUIRE(encodings);
  BOOST_REQUIRE_EQUAL(encodings.value().size(), 1U);
  BOOST_REQUIRE(encodings.value().front().nodeId().isLocal());
  BOOST_CHECK(encodings.value().front().nodeId().nodeId() == encoding_id);

  const auto definition =
      ::opcua::services::readDataTypeDefinition(client, type_id);
  BOOST_REQUIRE(definition);
  BOOST_REQUIRE(definition.value().isScalar());
  BOOST_REQUIRE(definition.value().isType<::opcua::StructureDefinition>());
  const auto structure =
      definition.value().scalar<::opcua::StructureDefinition>();
  BOOST_CHECK(structure.defaultEncodingId() == encoding_id);
  BOOST_REQUIRE_EQUAL(structure.fields().size(), 1U);
  BOOST_TEST(structure.fields().front().name() == "value");

  client.disconnect();
  second.stop();
  BOOST_TEST(second.typeRegistry() == nullptr);
}
