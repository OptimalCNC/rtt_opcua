#include <rtt/opcua/type_protocol.hpp>

#include <rtt/opcua/type_descriptor.hpp>

#include <open62541pp/ua/nodeids.hpp>

#include <rtt/OutputPort.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

namespace RTT::opcua {
namespace {

std::mutex &registrationMutex() {
  static std::mutex mutex;
  return mutex;
}

template <typename T, typename Wire = T>
class ScalarTypeProtocol final : public TypeProtocol {
public:
  explicit ScalarTypeProtocol(::opcua::NodeId data_type)
      : data_type_(std::move(data_type)) {}

  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                 ::opcua::Variant *value) const override {
    typename RTT::internal::DataSource<T>::shared_ptr typed =
        boost::dynamic_pointer_cast<RTT::internal::DataSource<T>>(source);
    if (!typed || value == nullptr) {
      return false;
    }
    typed->evaluate();
    *value = ::opcua::Variant(static_cast<Wire>(typed->value()));
    return true;
  }

  bool assignVariant(
      const ::opcua::Variant &value,
      const RTT::base::DataSourceBase::shared_ptr &destination) const override {
    typename RTT::internal::AssignableDataSource<T>::shared_ptr typed =
        boost::dynamic_pointer_cast<RTT::internal::AssignableDataSource<T>>(
            destination);
    if (!typed || !isCompatible(value)) {
      return false;
    }
    try {
      typed->set(static_cast<T>(value.to<Wire>()));
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &value) const override {
    if (!isCompatible(value)) {
      return {};
    }
    try {
      return new RTT::internal::ValueDataSource<T>(
          static_cast<T>(value.to<Wire>()));
    } catch (const std::exception &) {
      return {};
    }
  }

  bool portValue(const RTT::base::OutputPortInterface *port,
                 ::opcua::Variant *value) const override {
    const auto *typed = dynamic_cast<const RTT::OutputPort<T> *>(port);
    if (typed == nullptr || value == nullptr) {
      return false;
    }
    *value = ::opcua::Variant(static_cast<Wire>(typed->getLastWrittenValue()));
    return true;
  }

  ::opcua::NodeId dataTypeNodeId() const override { return data_type_; }

  ::opcua::ValueRank valueRank() const override {
    return ::opcua::ValueRank::Scalar;
  }

  bool hasValue() const noexcept override { return true; }

private:
  bool isCompatible(const ::opcua::Variant &value) const noexcept {
    return value.isScalar() && value.isType(data_type_);
  }

  ::opcua::NodeId data_type_;
};

class VoidTypeProtocol final : public TypeProtocol {
public:
  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &,
                 ::opcua::Variant *) const override {
    return false;
  }

  bool
  assignVariant(const ::opcua::Variant &,
                const RTT::base::DataSourceBase::shared_ptr &) const override {
    return false;
  }

  RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &) const override {
    return {};
  }

  bool portValue(const RTT::base::OutputPortInterface *,
                 ::opcua::Variant *) const override {
    return false;
  }

  ::opcua::NodeId dataTypeNodeId() const override {
    return ::opcua::DataTypeId::BaseDataType;
  }

  ::opcua::ValueRank valueRank() const override {
    return ::opcua::ValueRank::Scalar;
  }

  bool hasValue() const noexcept override { return false; }
};

std::unique_ptr<TypeProtocol>
makeCanonicalProtocol(std::string_view type_name) {
  const TypeDescriptor *descriptor = descriptorForType(type_name);
  if (descriptor == nullptr) {
    return {};
  }

  if (type_name == "Bool") {
    return std::make_unique<ScalarTypeProtocol<bool>>(descriptor->data_type);
  }
  if (type_name == "Int8") {
    return std::make_unique<ScalarTypeProtocol<std::int8_t>>(
        descriptor->data_type);
  }
  if (type_name == "UInt8") {
    return std::make_unique<ScalarTypeProtocol<std::uint8_t>>(
        descriptor->data_type);
  }
  if (type_name == "Int16") {
    return std::make_unique<ScalarTypeProtocol<std::int16_t>>(
        descriptor->data_type);
  }
  if (type_name == "UInt16") {
    return std::make_unique<ScalarTypeProtocol<std::uint16_t>>(
        descriptor->data_type);
  }
  if (type_name == "Int32") {
    return std::make_unique<ScalarTypeProtocol<std::int32_t>>(
        descriptor->data_type);
  }
  if (type_name == "UInt32") {
    return std::make_unique<ScalarTypeProtocol<std::uint32_t>>(
        descriptor->data_type);
  }
  if (type_name == "Int64") {
    return std::make_unique<ScalarTypeProtocol<std::int64_t>>(
        descriptor->data_type);
  }
  if (type_name == "UInt64") {
    return std::make_unique<ScalarTypeProtocol<std::uint64_t>>(
        descriptor->data_type);
  }
  if (type_name == "Float32") {
    return std::make_unique<ScalarTypeProtocol<float>>(descriptor->data_type);
  }
  if (type_name == "Float64") {
    return std::make_unique<ScalarTypeProtocol<double>>(descriptor->data_type);
  }
  if (type_name == "Char") {
    using CharWire =
        std::conditional_t<std::is_signed_v<char>, std::int8_t, std::uint8_t>;
    return std::make_unique<ScalarTypeProtocol<char, CharWire>>(
        descriptor->data_type);
  }
  if (type_name == "String") {
    return std::make_unique<ScalarTypeProtocol<std::string>>(
        descriptor->data_type);
  }
  if (type_name == "Void") {
    return std::make_unique<VoidTypeProtocol>();
  }
  return {};
}

bool registerTypeProtocolUnlocked(RTT::types::TypeInfo *type_info,
                                  std::unique_ptr<TypeProtocol> protocol) {
  if (type_info == nullptr || !protocol) {
    return false;
  }
  if (type_info->hasProtocol(kTransportProtocolId)) {
    return dynamic_cast<TypeProtocol *>(
               type_info->getProtocol(kTransportProtocolId)) != nullptr;
  }
  return type_info->addProtocol(kTransportProtocolId, protocol.release());
}

} // namespace

RTT::base::ChannelElementBase::shared_ptr
TypeProtocol::createStream(RTT::base::PortInterface *, const RTT::ConnPolicy &,
                           bool) const {
  return {};
}

bool registerTypeProtocol(RTT::types::TypeInfo *type_info,
                          std::unique_ptr<TypeProtocol> protocol) {
  std::lock_guard<std::mutex> lock(registrationMutex());
  return registerTypeProtocolUnlocked(type_info, std::move(protocol));
}

bool registerCanonicalTypeProtocol(std::string_view type_name,
                                   RTT::types::TypeInfo *type_info) {
  if (type_info == nullptr || type_info->getTypeName() != type_name ||
      descriptorForType(type_name) == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(registrationMutex());
  return registerTypeProtocolUnlocked(type_info,
                                      makeCanonicalProtocol(type_name));
}

bool registerCanonicalTypeProtocols() {
  std::lock_guard<std::mutex> lock(registrationMutex());
  for (const TypeDescriptor &descriptor : canonicalTypeDescriptors()) {
    RTT::types::TypeInfo *type_info =
        RTT::types::Types()->type(std::string(descriptor.rtt_name));
    if (type_info == nullptr ||
        !registerTypeProtocolUnlocked(
            type_info, makeCanonicalProtocol(descriptor.rtt_name))) {
      return false;
    }
  }
  return true;
}

const TypeProtocol *protocolForTypeInfo(const RTT::types::TypeInfo *type_info) {
  if (type_info == nullptr ||
      descriptorForType(type_info->getTypeName()) == nullptr) {
    return nullptr;
  }
  if (!type_info->hasProtocol(kTransportProtocolId) &&
      !registerCanonicalTypeProtocol(
          type_info->getTypeName(),
          const_cast<RTT::types::TypeInfo *>(type_info))) {
    return nullptr;
  }
  return dynamic_cast<const TypeProtocol *>(
      type_info->getProtocol(kTransportProtocolId));
}

const TypeProtocol *protocolForTypeName(std::string_view type_name) {
  if (descriptorForType(type_name) == nullptr) {
    return nullptr;
  }
  return protocolForTypeInfo(RTT::types::Types()->type(std::string(type_name)));
}

const TypeProtocol *
protocolForDataSource(const RTT::base::DataSourceBase::shared_ptr &source) {
  return source ? protocolForTypeInfo(source->getTypeInfo()) : nullptr;
}

} // namespace RTT::opcua
