#pragma once

#include <open62541pp/types.hpp>

#include <rtt/base/DataSourceBase.hpp>
#include <rtt/types/TypeTransporter.hpp>

#include <functional>
#include <memory>
#include <string_view>

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

class TypeProtocol : public RTT::types::TypeTransporter {
public:
  using VariantReader = std::function<bool(::opcua::Variant *)>;
  using VariantWriter = std::function<bool(const ::opcua::Variant &)>;

  ~TypeProtocol() override = default;

  RTT::base::ChannelElementBase::shared_ptr
  createStream(RTT::base::PortInterface *port, const RTT::ConnPolicy &policy,
               bool is_sender) const override;

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
  virtual ::opcua::NodeId dataTypeNodeId() const = 0;
  virtual ::opcua::ValueRank valueRank() const = 0;
  virtual bool hasValue() const noexcept = 0;
};

bool registerTypeProtocol(RTT::types::TypeInfo *type_info,
                          std::unique_ptr<TypeProtocol> protocol);
bool registerCanonicalTypeProtocol(std::string_view type_name,
                                   RTT::types::TypeInfo *type_info);
bool registerCanonicalTypeProtocols();

const TypeProtocol *protocolForTypeInfo(const RTT::types::TypeInfo *type_info);
const TypeProtocol *protocolForTypeName(std::string_view type_name);
const TypeProtocol *
protocolForDataSource(const RTT::base::DataSourceBase::shared_ptr &source);

} // namespace RTT::opcua
