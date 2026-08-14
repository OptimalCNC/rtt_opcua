#define BOOST_TEST_MODULE rtt_opcua_foundation
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/port_direction.hpp>
#include <rtt/opcua/server_options.hpp>
#include <rtt/opcua/type_descriptor.hpp>

#include <open62541pp/ua/nodeids.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <vector>

BOOST_AUTO_TEST_CASE(node_id_segments_are_stably_escaped) {
  using RTT::opcua::escapeNodeIdSegment;

  BOOST_TEST(escapeNodeIdSegment("axis_0-tilde~.name") == "axis_0-tilde~.name");
  BOOST_TEST(escapeNodeIdSegment("axis/0%") == "axis%2F0%25");
  BOOST_TEST(escapeNodeIdSegment("") == "%00");
  BOOST_TEST(escapeNodeIdSegment("\xC3\xA9") == "%C3%A9");
}

BOOST_AUTO_TEST_CASE(node_paths_never_embed_unescaped_names) {
  const std::array<std::string_view, 4> segments {
      "components", "arm/left", "services", "motion%raw"};

  BOOST_TEST(RTT::opcua::makeNodePath(segments) ==
             "rtt/components/arm%2Fleft/services/motion%25raw");
}

BOOST_AUTO_TEST_CASE(port_direction_codes_are_stable) {
  using RTT::opcua::PortDirection;

  static_assert(
      std::is_same_v<std::underlying_type_t<PortDirection>, std::int32_t>);
  BOOST_TEST(static_cast<std::int32_t>(PortDirection::input) == 0);
  BOOST_TEST(static_cast<std::int32_t>(PortDirection::output) == 1);
}

BOOST_AUTO_TEST_CASE(server_defaults_are_loopback_only) {
  const RTT::opcua::ServerOptions defaults;

  BOOST_TEST(defaults.bind_address == "127.0.0.1");
  BOOST_TEST(RTT::opcua::isLoopbackAddress(defaults.bind_address));
  BOOST_TEST(!RTT::opcua::validateServerOptions(defaults).has_value());
  BOOST_TEST(RTT::opcua::endpointUrl(defaults) == "opc.tcp://127.0.0.1:4840/rtt");
}

BOOST_AUTO_TEST_CASE(server_rejects_unsafe_or_malformed_bindings) {
  RTT::opcua::ServerOptions options;

  options.bind_address = "0.0.0.0";
  BOOST_TEST(RTT::opcua::validateServerOptions(options).has_value());

  options.bind_address = "192.0.2.10";
  BOOST_TEST(RTT::opcua::validateServerOptions(options).has_value());

  options.bind_address = "::1";
  options.endpoint_path = "rtt";
  BOOST_TEST(RTT::opcua::validateServerOptions(options).has_value());

  options.endpoint_path = "/rtt";
  BOOST_TEST(RTT::opcua::endpointUrl(options) == "opc.tcp://[::1]:4840/rtt");
}

BOOST_AUTO_TEST_CASE(canonical_builtin_catalog_is_exact) {
  const std::vector<std::string_view> expected {
      "Bool",   "Int8",    "UInt8",   "Int16",   "UInt16", "Int32", "UInt32",
      "Int64",  "UInt64",  "Float32", "Float64", "Char",   "String", "Void",
      "Float64Array", "Int32Array", "StringArray", "RtString", "FlowStatus",
      "WriteStatus"};
  const auto& descriptors = RTT::opcua::canonicalTypeDescriptors();

  BOOST_REQUIRE_EQUAL(descriptors.size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    BOOST_TEST(descriptors[index].rtt_name == expected[index]);
    BOOST_TEST(RTT::opcua::descriptorForType(expected[index]) == &descriptors[index]);
  }

  BOOST_CHECK(descriptors[0].data_type == ::opcua::NodeId(::opcua::DataTypeId::Boolean));
  BOOST_CHECK(descriptors[1].data_type == ::opcua::NodeId(::opcua::DataTypeId::SByte));
  BOOST_CHECK(descriptors[2].data_type == ::opcua::NodeId(::opcua::DataTypeId::Byte));
  BOOST_CHECK(descriptors[9].data_type == ::opcua::NodeId(::opcua::DataTypeId::Float));
  BOOST_CHECK(descriptors[10].data_type == ::opcua::NodeId(::opcua::DataTypeId::Double));
  BOOST_CHECK(descriptors[12].data_type == ::opcua::NodeId(::opcua::DataTypeId::String));
  BOOST_TEST(!descriptors[13].has_value);
  BOOST_CHECK(descriptors[18].data_type ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));
  BOOST_CHECK(descriptors[19].data_type ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));
  BOOST_TEST(RTT::opcua::descriptorForType("int") == nullptr);
  BOOST_TEST(RTT::opcua::descriptorForType("uint16") == nullptr);
}
