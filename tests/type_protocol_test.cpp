#define BOOST_TEST_MODULE rtt_opcua_type_protocol
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/type_descriptor.hpp>
#include <rtt/opcua/type_protocol.hpp>
#include <rtt/opcua/type_transport_plugin.hpp>

#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/Types.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

namespace {

struct CanonicalTypeFixture {
  CanonicalTypeFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
  }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(type_protocol_suite, CanonicalTypeFixture)

BOOST_AUTO_TEST_CASE(all_canonical_types_receive_the_opcua_transport) {
  BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());

  for (const auto &descriptor : RTT::opcua::canonicalTypeDescriptors()) {
    RTT::types::TypeInfo *type_info =
        RTT::types::Types()->type(std::string(descriptor.rtt_name));
    BOOST_REQUIRE_MESSAGE(type_info != nullptr, descriptor.rtt_name);
    BOOST_TEST(type_info->hasProtocol(RTT::opcua::kTransportProtocolId));
    const RTT::opcua::TypeProtocol *protocol =
        RTT::opcua::protocolForTypeName(descriptor.rtt_name);
    BOOST_REQUIRE_MESSAGE(protocol != nullptr, descriptor.rtt_name);
    BOOST_CHECK(protocol->dataTypeNodeId() == descriptor.data_type);
    BOOST_TEST(protocol->hasValue() == descriptor.has_value);
  }

  BOOST_TEST(RTT::opcua::protocolForTypeName("int") == nullptr);
  BOOST_TEST(RTT::opcua::protocolForTypeName("uint16") == nullptr);
}

BOOST_AUTO_TEST_CASE(scalar_protocol_round_trips_data_sources) {
  BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());
  const RTT::opcua::TypeProtocol *protocol =
      RTT::opcua::protocolForTypeName("Int32");
  BOOST_REQUIRE(protocol != nullptr);

  RTT::internal::ValueDataSource<std::int32_t>::shared_ptr value =
      new RTT::internal::ValueDataSource<std::int32_t>(42);
  ::opcua::Variant encoded;
  BOOST_REQUIRE(protocol->toVariant(value, &encoded));
  BOOST_TEST(encoded.to<std::int32_t>() == 42);

  BOOST_REQUIRE(
      protocol->assignVariant(::opcua::Variant(std::int32_t{84}), value));
  BOOST_TEST(value->get() == 84);

  const auto decoded =
      protocol->makeDataSource(::opcua::Variant(std::int32_t{-7}));
  const auto typed =
      boost::dynamic_pointer_cast<RTT::internal::DataSource<std::int32_t>>(
          decoded);
  BOOST_REQUIRE(typed);
  BOOST_TEST(typed->get() == -7);

  BOOST_TEST(
      !protocol->assignVariant(::opcua::Variant(std::string("wrong")), value));
}

BOOST_AUTO_TEST_CASE(byte_and_character_protocols_preserve_numeric_values) {
  BOOST_REQUIRE(RTT::opcua::registerCanonicalTypeProtocols());

  const auto *byte_protocol = RTT::opcua::protocolForTypeName("UInt8");
  BOOST_REQUIRE(byte_protocol != nullptr);
  RTT::internal::ValueDataSource<std::uint8_t>::shared_ptr byte =
      new RTT::internal::ValueDataSource<std::uint8_t>(200U);
  ::opcua::Variant encoded_byte;
  BOOST_REQUIRE(byte_protocol->toVariant(byte, &encoded_byte));
  BOOST_TEST(static_cast<unsigned int>(encoded_byte.to<std::uint8_t>()) ==
             200U);

  const auto *char_protocol = RTT::opcua::protocolForTypeName("Char");
  BOOST_REQUIRE(char_protocol != nullptr);
  RTT::internal::ValueDataSource<char>::shared_ptr character =
      new RTT::internal::ValueDataSource<char>('A');
  ::opcua::Variant encoded_character;
  BOOST_REQUIRE(char_protocol->toVariant(character, &encoded_character));
  if constexpr (std::is_signed_v<char>) {
    BOOST_TEST(encoded_character.to<std::int8_t>() == 65);
  } else {
    BOOST_TEST(encoded_character.to<std::uint8_t>() == 65U);
  }
}

BOOST_AUTO_TEST_CASE(transport_plugin_rejects_noncanonical_names) {
  RTT::opcua::TypeTransportPlugin plugin;
  BOOST_TEST(plugin.getTransportName() == "OPCUA");
  BOOST_TEST(plugin.getTypekitName() == "rtt-types");
  BOOST_TEST(plugin.getName() == "OPCUA://rtt-types");

  RTT::types::TypeInfo *int32 = RTT::types::Types()->type("Int32");
  BOOST_REQUIRE(int32 != nullptr);
  BOOST_TEST(plugin.registerTransport("Int32", int32));
  BOOST_TEST(!plugin.registerTransport("int", int32));
}

BOOST_AUTO_TEST_SUITE_END()
