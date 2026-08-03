#pragma once

#include <open62541pp/datatype.hpp>
#include <open62541pp/types.hpp>

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RTT::opcua {

struct LogicalDataTypeId {
  std::string namespace_uri;
  std::string type_node_id;
  std::string binary_encoding_node_id;

  auto operator<=>(const LogicalDataTypeId &) const = default;
};

enum class CustomDataTypeKind { structure, enumeration, union_type };

class EndpointTypeRegistry;

class DataTypeFactoryContext {
public:
  std::uint16_t namespaceIndex(std::string_view namespace_uri) const;
  ::opcua::NodeId nodeId(const LogicalDataTypeId &id) const;
  const UA_DataType *dataType(const LogicalDataTypeId &id) const noexcept;

private:
  struct State {
    std::map<std::string, std::uint16_t, std::less<>> namespace_indexes;
    std::map<LogicalDataTypeId, const UA_DataType *> data_types;
  };

  explicit DataTypeFactoryContext(std::shared_ptr<const State> state);

  std::shared_ptr<const State> state_;

  friend class EndpointTypeRegistry;
};

using DataTypeFactory =
    std::function<::opcua::DataType(const DataTypeFactoryContext &)>;

struct CustomDataTypeDefinition {
  std::string name;
  LogicalDataTypeId id;
  CustomDataTypeKind kind{CustomDataTypeKind::structure};
  std::string schema_fingerprint;
  DataTypeFactory materialize;
};

struct DataTypeProvider {
  std::string name;
  std::string namespace_uri;
  std::vector<std::string> dependencies;
  std::vector<CustomDataTypeDefinition> data_types;
};

bool registerDataTypeProvider(DataTypeProvider provider,
                              std::string *error = nullptr);
bool dataTypeRegistryFrozen() noexcept;
std::optional<std::vector<std::string>>
freezeDataTypeRegistry(std::string *error = nullptr);

} // namespace RTT::opcua
