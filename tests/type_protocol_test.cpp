#define BOOST_TEST_MODULE rtt_opcua_type_protocol
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/type_descriptor.hpp>
#include <rtt/opcua/endpoint_type_registry.hpp>
#include <rtt/opcua/type_protocol.hpp>
#include <rtt/opcua/type_transport_plugin.hpp>

#include <open62541pp/ua/nodeids.hpp>

#include <rtt/ConnPolicy.hpp>
#include <rtt/FlowStatus.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/base/TaskCore.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/rt_string.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/Types.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct CanonicalTypeFixture {
  CanonicalTypeFixture() {
    if (RTT::types::Types()->type("Int32") == nullptr) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    std::string error;
    if (!RTT::opcua::registerCanonicalTypeProtocols(&error)) {
      throw std::runtime_error(error);
    }
  }
};

std::shared_ptr<RTT::opcua::EndpointTypeRegistry> makeRegistry() {
  std::string error;
  auto registry = RTT::opcua::EndpointTypeRegistry::create(
      {{"http://opcfoundation.org/UA/", 0}, {"urn:orocos:rtt", 1}},
      &error);
  BOOST_REQUIRE_MESSAGE(registry, error);
  return registry;
}

void checkConnPolicy(const RTT::ConnPolicy &actual,
                     const RTT::ConnPolicy &expected) {
  BOOST_TEST(actual.type == expected.type);
  BOOST_TEST(actual.size == expected.size);
  BOOST_TEST(actual.lock_policy == expected.lock_policy);
  BOOST_TEST(actual.init == expected.init);
  BOOST_TEST(actual.pull == expected.pull);
  BOOST_TEST(actual.buffer_policy == expected.buffer_policy);
  BOOST_TEST(actual.max_threads == expected.max_threads);
  BOOST_TEST(actual.mandatory == expected.mandatory);
  BOOST_TEST(actual.transport == expected.transport);
  BOOST_TEST(actual.data_size == expected.data_size);
  BOOST_TEST(actual.name_id == expected.name_id);
}

template <typename T>
void exerciseArrayCodec(std::string_view type_name,
                        const ::opcua::NodeId &element_type,
                        const std::vector<T> &initial,
                        const std::vector<T> &replacement) {
  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec = registry->codecForTypeName(type_name);
  BOOST_REQUIRE_MESSAGE(codec != nullptr, type_name);
  BOOST_CHECK(codec->dataTypeNodeId() == element_type);
  BOOST_CHECK(codec->valueRank() == ::opcua::ValueRank::OneDimension);

  typename RTT::internal::ValueDataSource<std::vector<T>>::shared_ptr source =
      new RTT::internal::ValueDataSource<std::vector<T>>(initial);
  ::opcua::Variant encoded;
  BOOST_REQUIRE(codec->toVariant(source, &encoded));
  BOOST_TEST(encoded.to<std::vector<T>>() == initial,
             boost::test_tools::per_element());

  BOOST_REQUIRE(codec->assignVariant(::opcua::Variant(replacement), source));
  BOOST_TEST(source->get() == replacement, boost::test_tools::per_element());
  BOOST_TEST(!codec->assignVariant(::opcua::Variant(T{}), source));

  const auto decoded = codec->makeDataSource(::opcua::Variant(initial));
  const auto typed = boost::dynamic_pointer_cast<
      RTT::internal::DataSource<std::vector<T>>>(decoded);
  BOOST_REQUIRE(typed);
  BOOST_TEST(typed->get() == initial, boost::test_tools::per_element());

  ::opcua::Variant remote(initial);
  RTT::opcua::VariantReader reader = [&remote](::opcua::Variant *value) {
    *value = remote;
    return true;
  };
  RTT::opcua::VariantWriter writer =
      [&remote](const ::opcua::Variant &value) {
        remote = value;
        return true;
      };
  const auto writable = boost::dynamic_pointer_cast<
      RTT::internal::AssignableDataSource<std::vector<T>>>(
      codec->makeProxyDataSource(reader, writer));
  BOOST_REQUIRE(writable);
  BOOST_TEST(writable->get() == initial, boost::test_tools::per_element());
  writable->set(replacement);
  BOOST_TEST(remote.to<std::vector<T>>() == replacement,
             boost::test_tools::per_element());

  const auto read_only = boost::dynamic_pointer_cast<
      RTT::internal::DataSource<std::vector<T>>>(
      codec->makeProxyDataSource(reader));
  BOOST_REQUIRE(read_only);
  BOOST_TEST(read_only->get() == replacement,
             boost::test_tools::per_element());

  RTT::OutputPort<std::vector<T>> port("values");
  port.write(initial);
  ::opcua::Variant port_value;
  BOOST_CHECK(codec->portValue(&port, &port_value) ==
              RTT::opcua::PortValueStatus::value);
  BOOST_TEST(port_value.to<std::vector<T>>() == initial,
             boost::test_tools::per_element());
}

} // namespace

BOOST_GLOBAL_FIXTURE(CanonicalTypeFixture);

BOOST_AUTO_TEST_SUITE(type_protocol_suite)

BOOST_AUTO_TEST_CASE(transport_plugin_rejects_noncanonical_names) {
  RTT::opcua::TypeTransportPlugin plugin;
  BOOST_TEST(plugin.getTransportName() == "OPCUA");
  BOOST_TEST(plugin.getTypekitName() == "rtt-types");
  BOOST_TEST(plugin.getName() == "OPCUA://rtt-types");

  RTT::types::TypeInfo *int32 = RTT::types::Types()->type("Int32");
  BOOST_REQUIRE(int32 != nullptr);
  BOOST_TEST(plugin.registerTransport("Int32", int32));
  BOOST_TEST(!plugin.registerTransport("int", int32));

  RTT::types::TypeInfo *conn_policy = RTT::types::Types()->type("ConnPolicy");
  BOOST_REQUIRE(conn_policy != nullptr);
  BOOST_TEST(plugin.registerTransport("ConnPolicy", conn_policy));
  BOOST_TEST(!plugin.registerTransport("connpolicy", conn_policy));
}

BOOST_AUTO_TEST_CASE(canonical_registration_reports_the_rejected_type) {
  std::string error;
  BOOST_TEST(!RTT::opcua::registerCanonicalTypeProtocol("Int32", nullptr,
                                                        &error));
  BOOST_TEST(error.find("'Int32'") != std::string::npos);

  RTT::types::TypeInfo *int32 = RTT::types::Types()->type("Int32");
  BOOST_REQUIRE(int32 != nullptr);
  BOOST_TEST(!RTT::opcua::registerCanonicalTypeProtocol("NotCanonical", int32,
                                                        &error));
  BOOST_TEST(error.find("'NotCanonical'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(all_canonical_types_receive_the_opcua_transport) {
  const auto registry = makeRegistry();

  for (const auto &descriptor : RTT::opcua::canonicalTypeDescriptors()) {
    RTT::types::TypeInfo *type_info =
        RTT::types::Types()->type(std::string(descriptor.rtt_name));
    BOOST_REQUIRE_MESSAGE(type_info != nullptr, descriptor.rtt_name);
    BOOST_TEST(type_info->hasProtocol(RTT::opcua::kTransportProtocolId));
    const RTT::opcua::TypeCodec *codec =
        registry->codecForTypeName(descriptor.rtt_name);
    BOOST_REQUIRE_MESSAGE(codec != nullptr, descriptor.rtt_name);
    BOOST_CHECK(codec->dataTypeNodeId() == descriptor.data_type);
    BOOST_TEST(codec->hasValue() == descriptor.has_value);
  }

  BOOST_TEST(registry->codecForTypeName("int") == nullptr);
  BOOST_TEST(registry->codecForTypeName("uint16") == nullptr);
}

BOOST_AUTO_TEST_CASE(scalar_protocol_round_trips_data_sources) {
  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec = registry->codecForTypeName("Int32");
  BOOST_REQUIRE(codec != nullptr);

  RTT::internal::ValueDataSource<std::int32_t>::shared_ptr value =
      new RTT::internal::ValueDataSource<std::int32_t>(42);
  ::opcua::Variant encoded;
  BOOST_REQUIRE(codec->toVariant(value, &encoded));
  BOOST_TEST(encoded.to<std::int32_t>() == 42);

  BOOST_REQUIRE(
      codec->assignVariant(::opcua::Variant(std::int32_t{84}), value));
  BOOST_TEST(value->get() == 84);

  const auto decoded =
      codec->makeDataSource(::opcua::Variant(std::int32_t{-7}));
  const auto typed =
      boost::dynamic_pointer_cast<RTT::internal::DataSource<std::int32_t>>(
          decoded);
  BOOST_REQUIRE(typed);
  BOOST_TEST(typed->get() == -7);

  BOOST_TEST(
      !codec->assignVariant(::opcua::Variant(std::string("wrong")), value));
}

BOOST_AUTO_TEST_CASE(status_protocols_use_documented_int32_codes) {
  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *flow =
      registry->codecForTypeName("FlowStatus");
  const RTT::opcua::TypeCodec *write =
      registry->codecForTypeName("WriteStatus");
  BOOST_REQUIRE(flow != nullptr);
  BOOST_REQUIRE(write != nullptr);
  BOOST_CHECK(flow->dataTypeNodeId() ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));
  BOOST_CHECK(write->dataTypeNodeId() ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));

  const std::array<std::pair<RTT::FlowStatus, std::int32_t>, 3> flow_values{{
      {RTT::NoData, 0},
      {RTT::OldData, 1},
      {RTT::NewData, 2},
  }};
  for (const auto &[status, code] : flow_values) {
    RTT::internal::ValueDataSource<RTT::FlowStatus>::shared_ptr source =
        new RTT::internal::ValueDataSource<RTT::FlowStatus>(status);
    ::opcua::Variant encoded;
    BOOST_REQUIRE(flow->toVariant(source, &encoded));
    BOOST_TEST(encoded.to<std::int32_t>() == code);

    const auto decoded =
        flow->makeDataSource(::opcua::Variant(std::int32_t{code}));
    const auto typed = boost::dynamic_pointer_cast<
        RTT::internal::DataSource<RTT::FlowStatus>>(decoded);
    BOOST_REQUIRE(typed);
    BOOST_TEST(typed->get() == status);
  }

  const std::array<std::pair<RTT::WriteStatus, std::int32_t>, 3> write_values{{
      {RTT::WriteSuccess, 0},
      {RTT::WriteFailure, 1},
      {RTT::NotConnected, 2},
  }};
  for (const auto &[status, code] : write_values) {
    RTT::internal::ValueDataSource<RTT::WriteStatus>::shared_ptr source =
        new RTT::internal::ValueDataSource<RTT::WriteStatus>(status);
    ::opcua::Variant encoded;
    BOOST_REQUIRE(write->toVariant(source, &encoded));
    BOOST_TEST(encoded.to<std::int32_t>() == code);

    const auto decoded =
        write->makeDataSource(::opcua::Variant(std::int32_t{code}));
    const auto typed = boost::dynamic_pointer_cast<
        RTT::internal::DataSource<RTT::WriteStatus>>(decoded);
    BOOST_REQUIRE(typed);
    BOOST_TEST(typed->get() == status);
  }

  BOOST_TEST(!flow->makeDataSource(
      ::opcua::Variant(std::string("NewData"))));
  BOOST_TEST(!write->makeDataSource(
      ::opcua::Variant(std::string("WriteSuccess"))));
}

BOOST_AUTO_TEST_CASE(task_state_protocol_is_a_strict_bounded_int32_scalar) {
  using TaskState = RTT::base::TaskCore::TaskState;

  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec =
      registry->codecForTypeName("TaskState");
  BOOST_REQUIRE(codec != nullptr);
  BOOST_CHECK(codec->dataTypeNodeId() ==
              ::opcua::NodeId(::opcua::DataTypeId::Int32));
  BOOST_CHECK(codec->valueRank() == ::opcua::ValueRank::Scalar);

  const std::array<std::pair<TaskState, std::int32_t>, 7> values{{
      {RTT::base::TaskCore::Init, 0},
      {RTT::base::TaskCore::PreOperational, 1},
      {RTT::base::TaskCore::FatalError, 2},
      {RTT::base::TaskCore::Exception, 3},
      {RTT::base::TaskCore::Stopped, 4},
      {RTT::base::TaskCore::Running, 5},
      {RTT::base::TaskCore::RunTimeError, 6},
  }};
  for (const auto &[state, code] : values) {
    RTT::internal::ValueDataSource<TaskState>::shared_ptr source =
        new RTT::internal::ValueDataSource<TaskState>(state);
    ::opcua::Variant encoded;
    BOOST_REQUIRE(codec->toVariant(source, &encoded));
    BOOST_TEST(encoded.isScalar());
    BOOST_TEST(encoded.isType(::opcua::NodeId(::opcua::DataTypeId::Int32)));
    BOOST_TEST(encoded.to<std::int32_t>() == code);

    BOOST_REQUIRE(codec->assignVariant(::opcua::Variant(code), source));
    BOOST_TEST(static_cast<std::int32_t>(source->get()) == code);

    const auto decoded = codec->makeDataSource(::opcua::Variant(code));
    const auto typed = boost::dynamic_pointer_cast<
        RTT::internal::DataSource<TaskState>>(decoded);
    BOOST_REQUIRE(typed);
    BOOST_TEST(static_cast<std::int32_t>(typed->get()) == code);

    RTT::OutputPort<TaskState> port("state");
    port.write(state);
    ::opcua::Variant port_value;
    BOOST_CHECK(codec->portValue(&port, &port_value) ==
                RTT::opcua::PortValueStatus::value);
    BOOST_TEST(port_value.to<std::int32_t>() == code);
  }

  RTT::internal::ValueDataSource<TaskState>::shared_ptr destination =
      new RTT::internal::ValueDataSource<TaskState>(
          RTT::base::TaskCore::Stopped);
  for (const std::int32_t invalid : {-1, 7}) {
    BOOST_TEST(!codec->assignVariant(::opcua::Variant(invalid), destination));
    BOOST_TEST(destination->get() == RTT::base::TaskCore::Stopped);
    BOOST_TEST(!codec->makeDataSource(::opcua::Variant(invalid)));
  }
  BOOST_TEST(!codec->assignVariant(::opcua::Variant(std::uint32_t{4}),
                                   destination));
  BOOST_TEST(!codec->assignVariant(::opcua::Variant(std::string("Stopped")),
                                   destination));
  BOOST_TEST(!codec->assignVariant(
      ::opcua::Variant(std::vector<std::int32_t>{4}), destination));

  ::opcua::Variant remote(std::int32_t{4});
  RTT::opcua::VariantReader reader = [&remote](::opcua::Variant *value) {
    *value = remote;
    return true;
  };
  bool wrote = false;
  RTT::opcua::VariantWriter writer =
      [&remote, &wrote](const ::opcua::Variant &value) {
        remote = value;
        wrote = true;
        return true;
      };
  const auto writable = boost::dynamic_pointer_cast<
      RTT::internal::AssignableDataSource<TaskState>>(
      codec->makeProxyDataSource(reader, writer));
  BOOST_REQUIRE(writable);
  for (const std::int32_t invalid : {-1, 7}) {
    writable->set(static_cast<TaskState>(invalid));
    BOOST_TEST(!wrote);
    BOOST_TEST(remote.to<std::int32_t>() == 4);
  }

  remote = ::opcua::Variant(std::int32_t{7});
  BOOST_TEST(!writable->evaluate());

  ::opcua::Variant invalid_encoded;
  for (const std::int32_t invalid : {-1, 7}) {
    RTT::internal::ValueDataSource<TaskState>::shared_ptr invalid_source =
        new RTT::internal::ValueDataSource<TaskState>(
            static_cast<TaskState>(invalid));
    BOOST_TEST(!codec->toVariant(invalid_source, &invalid_encoded));
  }

  RTT::OutputPort<TaskState> invalid_port("invalid-state");
  invalid_port.write(static_cast<TaskState>(7));
  BOOST_CHECK(codec->portValue(&invalid_port, &invalid_encoded) ==
              RTT::opcua::PortValueStatus::error);
}

BOOST_AUTO_TEST_CASE(output_port_value_distinguishes_unwritten_from_current) {
  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec = registry->codecForTypeName("Int32");
  BOOST_REQUIRE(codec != nullptr);

  RTT::OutputPort<std::int32_t> port("value");
  ::opcua::Variant encoded;
  BOOST_CHECK(codec->portValue(&port, &encoded) ==
              RTT::opcua::PortValueStatus::waiting_for_initial_data);

  BOOST_TEST(port.write(42) == RTT::NotConnected);
  std::int32_t sample = 0;
  BOOST_REQUIRE(port.getLastWrittenValue(sample));
  BOOST_TEST(sample == 42);
  BOOST_CHECK(codec->portValue(&port, &encoded) ==
              RTT::opcua::PortValueStatus::value);
  BOOST_TEST(encoded.to<std::int32_t>() == 42);
}

BOOST_AUTO_TEST_CASE(byte_and_character_protocols_preserve_numeric_values) {
  const auto registry = makeRegistry();

  const auto *byte_protocol = registry->codecForTypeName("UInt8");
  BOOST_REQUIRE(byte_protocol != nullptr);
  RTT::internal::ValueDataSource<std::uint8_t>::shared_ptr byte =
      new RTT::internal::ValueDataSource<std::uint8_t>(200U);
  ::opcua::Variant encoded_byte;
  BOOST_REQUIRE(byte_protocol->toVariant(byte, &encoded_byte));
  BOOST_TEST(static_cast<unsigned int>(encoded_byte.to<std::uint8_t>()) ==
             200U);

  const auto *char_protocol = registry->codecForTypeName("Char");
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

BOOST_AUTO_TEST_CASE(canonical_array_protocols_round_trip_all_surfaces) {
  exerciseArrayCodec<double>("Float64Array", ::opcua::DataTypeId::Double,
                             {1.5, 2.5}, {3.5, 4.5, 5.5});
  exerciseArrayCodec<std::int32_t>("Int32Array", ::opcua::DataTypeId::Int32,
                                   {1, 2}, {3, 4, 5});
  exerciseArrayCodec<std::string>("StringArray", ::opcua::DataTypeId::String,
                                  {"one", "two"}, {"three", "four"});
}

BOOST_AUTO_TEST_CASE(rt_string_protocol_round_trips_all_surfaces) {
  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec = registry->codecForTypeName("RtString");
  BOOST_REQUIRE(codec != nullptr);
  BOOST_CHECK(codec->dataTypeNodeId() ==
              ::opcua::NodeId(::opcua::DataTypeId::String));
  BOOST_CHECK(codec->valueRank() == ::opcua::ValueRank::Scalar);

  RTT::internal::ValueDataSource<RTT::rt_string>::shared_ptr source =
      new RTT::internal::ValueDataSource<RTT::rt_string>(RTT::rt_string("one"));
  ::opcua::Variant encoded;
  BOOST_REQUIRE(codec->toVariant(source, &encoded));
  BOOST_TEST(encoded.to<std::string>() == "one");
  BOOST_REQUIRE(
      codec->assignVariant(::opcua::Variant(std::string("two")), source));
  BOOST_TEST(std::string(source->get().c_str()) == "two");
  BOOST_TEST(!codec->assignVariant(
      ::opcua::Variant(std::vector<std::string>{"wrong"}), source));

  const auto decoded = codec->makeDataSource(
      ::opcua::Variant(std::string("decoded")));
  const auto typed = boost::dynamic_pointer_cast<
      RTT::internal::DataSource<RTT::rt_string>>(decoded);
  BOOST_REQUIRE(typed);
  BOOST_TEST(std::string(typed->get().c_str()) == "decoded");

  ::opcua::Variant remote(std::string("remote"));
  RTT::opcua::VariantReader reader = [&remote](::opcua::Variant *value) {
    *value = remote;
    return true;
  };
  RTT::opcua::VariantWriter writer =
      [&remote](const ::opcua::Variant &value) {
        remote = value;
        return true;
      };
  const auto writable = boost::dynamic_pointer_cast<
      RTT::internal::AssignableDataSource<RTT::rt_string>>(
      codec->makeProxyDataSource(reader, writer));
  BOOST_REQUIRE(writable);
  BOOST_TEST(std::string(writable->get().c_str()) == "remote");
  writable->set(RTT::rt_string("written"));
  BOOST_TEST(remote.to<std::string>() == "written");

  const auto read_only = boost::dynamic_pointer_cast<
      RTT::internal::DataSource<RTT::rt_string>>(
      codec->makeProxyDataSource(reader));
  BOOST_REQUIRE(read_only);
  BOOST_TEST(std::string(read_only->get().c_str()) == "written");

  RTT::OutputPort<RTT::rt_string> port("text");
  port.write(RTT::rt_string("port"));
  ::opcua::Variant port_value;
  BOOST_CHECK(codec->portValue(&port, &port_value) ==
              RTT::opcua::PortValueStatus::value);
  BOOST_TEST(port_value.to<std::string>() == "port");
}

BOOST_AUTO_TEST_CASE(conn_policy_protocol_round_trips_every_public_field) {
  RTT::ConnPolicy expected;
  expected.type = RTT::ConnPolicy::BUFFER;
  expected.size = 17;
  expected.lock_policy = RTT::ConnPolicy::LOCKED;
  expected.init = true;
  expected.pull = true;
  expected.buffer_policy = RTT::PerInputPort;
  expected.max_threads = 6;
  expected.mandatory = false;
  expected.transport = 42;
  expected.data_size = 4096;
  expected.name_id = "fixture/channel";

  RTT::types::TypeInfo *type_info = RTT::types::Types()->type("ConnPolicy");
  BOOST_REQUIRE(type_info != nullptr);
  BOOST_TEST(type_info->hasProtocol(RTT::opcua::kTransportProtocolId));

  const auto registry = makeRegistry();
  const RTT::opcua::TypeCodec *codec = registry->codecForTypeName("ConnPolicy");
  BOOST_REQUIRE(codec != nullptr);
  BOOST_CHECK(codec->dataTypeNodeId() ==
              ::opcua::NodeId(1, "types/ConnPolicy"));
  BOOST_CHECK(codec->valueRank() == ::opcua::ValueRank::Scalar);
  BOOST_TEST(codec->hasValue());

  bool found_custom_type = false;
  for (const ::opcua::DataType &data_type : registry->customDataTypes()) {
    if (data_type.typeId() == ::opcua::NodeId(1, "types/ConnPolicy")) {
      found_custom_type = true;
      BOOST_CHECK(data_type.binaryEncodingId() ==
                  ::opcua::NodeId(1, "encodings/ConnPolicy/Binary"));
    }
  }
  BOOST_TEST(found_custom_type);

  RTT::internal::ValueDataSource<RTT::ConnPolicy>::shared_ptr source =
      new RTT::internal::ValueDataSource<RTT::ConnPolicy>(expected);
  ::opcua::Variant encoded;
  BOOST_REQUIRE(codec->toVariant(source, &encoded));
  BOOST_REQUIRE(codec->assignVariant(encoded, source));
  checkConnPolicy(source->get(), expected);

  const auto decoded = codec->makeDataSource(encoded);
  const auto typed =
      boost::dynamic_pointer_cast<RTT::internal::DataSource<RTT::ConnPolicy>>(
          decoded);
  BOOST_REQUIRE(typed);
  checkConnPolicy(typed->get(), expected);

  ::opcua::Variant remote = encoded;
  RTT::opcua::VariantReader reader = [&remote](::opcua::Variant *value) {
    *value = remote;
    return true;
  };
  RTT::opcua::VariantWriter writer = [&remote](const ::opcua::Variant &value) {
    remote = value;
    return true;
  };
  const auto writable = boost::dynamic_pointer_cast<
      RTT::internal::AssignableDataSource<RTT::ConnPolicy>>(
      codec->makeProxyDataSource(reader, writer));
  BOOST_REQUIRE(writable);
  checkConnPolicy(writable->get(), expected);
  writable->set(expected);
  BOOST_REQUIRE(codec->assignVariant(remote, source));
  checkConnPolicy(source->get(), expected);

  const auto read_only =
      boost::dynamic_pointer_cast<RTT::internal::DataSource<RTT::ConnPolicy>>(
          codec->makeProxyDataSource(reader));
  BOOST_REQUIRE(read_only);
  checkConnPolicy(read_only->get(), expected);

  RTT::OutputPort<RTT::ConnPolicy> port("policy");
  port.write(expected);
  ::opcua::Variant port_value;
  BOOST_CHECK(codec->portValue(&port, &port_value) ==
              RTT::opcua::PortValueStatus::value);
  BOOST_REQUIRE(codec->assignVariant(port_value, source));
  checkConnPolicy(source->get(), expected);
}

BOOST_AUTO_TEST_SUITE_END()
