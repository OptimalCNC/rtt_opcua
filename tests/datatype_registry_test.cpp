#define BOOST_TEST_MODULE rtt_opcua_datatype_registry
#include <boost/test/included/unit_test.hpp>

#include <rtt/opcua/datatype_registry.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

RTT::opcua::CustomDataTypeDefinition
definition(std::string name, std::string type_node,
           std::string encoding_node, std::string fingerprint) {
  RTT::opcua::CustomDataTypeDefinition result;
  result.name = std::move(name);
  result.id = {"urn:test:types", std::move(type_node),
               std::move(encoding_node)};
  result.schema_fingerprint = std::move(fingerprint);
  result.materialize = [](const RTT::opcua::DataTypeFactoryContext &) {
    return ::opcua::DataType{};
  };
  return result;
}

RTT::opcua::DataTypeProvider
provider(std::string name, std::vector<std::string> dependencies,
         RTT::opcua::CustomDataTypeDefinition data_type) {
  RTT::opcua::DataTypeProvider result;
  result.name = std::move(name);
  result.namespace_uri = "urn:test:types";
  result.dependencies = std::move(dependencies);
  result.data_types.push_back(std::move(data_type));
  return result;
}

void checkPrefix(const std::string &actual, const std::string &prefix) {
  BOOST_TEST(actual.starts_with(prefix),
             "actual error '" << actual << "' does not start with '" << prefix
                              << "'");
}

} // namespace

BOOST_AUTO_TEST_CASE(lifecycle) {
  auto dependent = provider("dependent", {"base"},
                            definition("Dependent", "DependentType",
                                       "DependentBinary", "dependent-v1"));
  const auto identical_dependent = dependent;
  BOOST_REQUIRE(RTT::opcua::registerDataTypeProvider(std::move(dependent)));
  BOOST_TEST(
      RTT::opcua::registerDataTypeProvider(identical_dependent));

  std::string error;
  auto conflicting = identical_dependent;
  conflicting.data_types.front().schema_fingerprint = "dependent-v2";
  BOOST_TEST(!RTT::opcua::registerDataTypeProvider(std::move(conflicting),
                                                   &error));
  checkPrefix(error, "conflicting OPC UA datatype provider");

  auto collision = provider("collision", {},
                            definition("Collision", "DependentBinary",
                                       "CollisionBinary", "collision-v1"));
  BOOST_TEST(!RTT::opcua::registerDataTypeProvider(std::move(collision),
                                                   &error));
  checkPrefix(error, "conflicting OPC UA datatype provider");

  BOOST_REQUIRE(RTT::opcua::registerDataTypeProvider(provider(
      "base", {}, definition("Base", "BaseType", "BaseBinary", "base-v1"))));

  const auto order = RTT::opcua::freezeDataTypeRegistry(&error);
  BOOST_REQUIRE_MESSAGE(order.has_value(), error);
  BOOST_TEST(*order == std::vector<std::string>({"base", "dependent"}),
             boost::test_tools::per_element());
  BOOST_TEST(RTT::opcua::dataTypeRegistryFrozen());

  auto late = provider("late", {},
                       definition("Late", "LateType", "LateBinary", "late-v1"));
  BOOST_TEST(!RTT::opcua::registerDataTypeProvider(std::move(late), &error));
  checkPrefix(error, "late OPC UA datatype provider registration");
}

BOOST_AUTO_TEST_CASE(missing_dependency) {
  BOOST_REQUIRE(RTT::opcua::registerDataTypeProvider(provider(
      "dependent", {"absent"},
      definition("Dependent", "DependentType", "DependentBinary", "v1"))));
  std::string error;
  BOOST_TEST(!RTT::opcua::freezeDataTypeRegistry(&error).has_value());
  checkPrefix(error, "missing OPC UA datatype provider dependency");
  BOOST_TEST(!RTT::opcua::dataTypeRegistryFrozen());
}

BOOST_AUTO_TEST_CASE(dependency_cycle) {
  BOOST_REQUIRE(RTT::opcua::registerDataTypeProvider(provider(
      "cycle-a", {"cycle-b"},
      definition("CycleA", "CycleAType", "CycleABinary", "a-v1"))));
  BOOST_REQUIRE(RTT::opcua::registerDataTypeProvider(provider(
      "cycle-b", {"cycle-a"},
      definition("CycleB", "CycleBType", "CycleBBinary", "b-v1"))));
  std::string error;
  BOOST_TEST(!RTT::opcua::freezeDataTypeRegistry(&error).has_value());
  checkPrefix(error, "cyclic OPC UA datatype provider dependency");
  BOOST_TEST(!RTT::opcua::dataTypeRegistryFrozen());
}

BOOST_AUTO_TEST_CASE(invalid_definition) {
  auto invalid = provider(
      "invalid", {}, definition("Invalid", "", "Encoding", "invalid-v1"));
  std::string error;
  BOOST_TEST(!RTT::opcua::registerDataTypeProvider(std::move(invalid), &error));
  checkPrefix(error, "invalid OPC UA datatype provider");
}
