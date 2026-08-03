#pragma once

#include <rtt/opcua/type_protocol.hpp>

#include <open62541pp/datatype.hpp>
#include <open62541pp/span.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RTT::opcua {

class EndpointTypeRegistry {
public:
  static std::shared_ptr<EndpointTypeRegistry>
  create(const std::vector<std::pair<std::string, std::uint16_t>> &namespaces,
         std::string *error = nullptr);

  const TypeCodec *
  codecForTypeInfo(const RTT::types::TypeInfo *type_info) const;
  const TypeCodec *codecForTypeName(std::string_view type_name) const;
  const TypeCodec *codecForDataSource(
      const RTT::base::DataSourceBase::shared_ptr &source) const;

  ::opcua::Span<const ::opcua::DataType>
  customDataTypes() const noexcept;
  const ::opcua::DataType *
  dataType(const LogicalDataTypeId &id) const noexcept;
  std::optional<std::uint16_t>
  namespaceIndex(std::string_view namespace_uri) const noexcept;

private:
  bool bind(std::string *error);

  std::map<std::string, std::uint16_t, std::less<>> namespace_indexes_;
  std::vector<::opcua::DataType> custom_data_types_;
  std::map<LogicalDataTypeId, std::size_t> custom_data_type_indexes_;
  std::map<const RTT::types::TypeInfo *, std::unique_ptr<TypeCodec>> codecs_;
  std::map<std::string, const TypeCodec *, std::less<>> codecs_by_name_;
  std::shared_ptr<DataTypeFactoryContext::State> factory_state_;
};

} // namespace RTT::opcua
