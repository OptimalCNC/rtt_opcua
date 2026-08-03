#pragma once

#include <rtt/opcua/datatype_registry.hpp>

#include <open62541pp/types.hpp>

#include <rtt/base/DataSourceBase.hpp>
#include <rtt/types/TypeTransporter.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace RTT {
namespace base {
class OutputPortInterface;
}
namespace types {
class TypeInfo;
}
} // namespace RTT

namespace RTT::opcua {

inline constexpr int kTransportProtocolId = 1042;

using DataTypeReference = std::variant<::opcua::NodeId, LogicalDataTypeId>;
using VariantReader = std::function<bool(::opcua::Variant *)>;
using VariantWriter = std::function<bool(const ::opcua::Variant &)>;

class TypeCodec {
public:
  virtual ~TypeCodec() = default;

  virtual bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                         ::opcua::Variant *value) const = 0;
  virtual bool assignVariant(
      const ::opcua::Variant &value,
      const RTT::base::DataSourceBase::shared_ptr &destination) const = 0;
  virtual RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &value) const = 0;
  virtual RTT::base::DataSourceBase::shared_ptr
  makeProxyDataSource(VariantReader reader,
                      VariantWriter writer = {}) const = 0;
  virtual bool portValue(const RTT::base::OutputPortInterface *port,
                         ::opcua::Variant *value) const = 0;

  const ::opcua::NodeId &dataTypeNodeId() const noexcept;
  ::opcua::ValueRank valueRank() const noexcept;
  bool hasValue() const noexcept;

protected:
  TypeCodec(::opcua::NodeId data_type, ::opcua::ValueRank value_rank,
            bool has_value);

private:
  ::opcua::NodeId data_type_;
  ::opcua::ValueRank value_rank_;
  bool has_value_;
};

class TypeProtocol : public RTT::types::TypeTransporter {
public:
  ~TypeProtocol() override = default;

  RTT::base::ChannelElementBase::shared_ptr
  createStream(RTT::base::PortInterface *port, const RTT::ConnPolicy &policy,
               bool is_sender) const override;

  virtual DataTypeReference dataType() const = 0;
  virtual std::string registrationFingerprint() const = 0;
  virtual std::unique_ptr<TypeCodec>
  bind(const ::opcua::NodeId &data_type, const UA_DataType &native_type,
       std::string *error = nullptr) const = 0;
};

bool registerTypeProtocol(RTT::types::TypeInfo *type_info,
                          std::unique_ptr<TypeProtocol> protocol,
                          std::string *error = nullptr);
bool registerCanonicalTypeProtocol(std::string_view type_name,
                                   RTT::types::TypeInfo *type_info,
                                   std::string *error = nullptr);
bool registerCanonicalTypeProtocols(std::string *error = nullptr);

} // namespace RTT::opcua
