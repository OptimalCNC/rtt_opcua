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
#include <map>
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

template <typename T, typename Wire>
bool decodeScalar(const ::opcua::Variant &value,
                  const ::opcua::NodeId &data_type, T *decoded) noexcept {
  if (decoded == nullptr || !value.isScalar() || !value.isType(data_type)) {
    return false;
  }
  try {
    *decoded = static_cast<T>(value.to<Wire>());
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

template <typename T, typename Wire>
class ScalarProxyDataSource final : public RTT::internal::DataSource<T> {
public:
  ScalarProxyDataSource(TypeProtocol::VariantReader reader,
                        ::opcua::NodeId data_type)
      : reader_(std::move(reader)), data_type_(std::move(data_type)) {}

  T get() const override {
    refresh();
    return last_value_;
  }

  T value() const override { return last_value_; }

  typename RTT::internal::DataSource<T>::const_reference_t
  rvalue() const override {
    return last_value_;
  }

  bool evaluate() const override { return refresh(); }

  ScalarProxyDataSource *clone() const override {
    return new ScalarProxyDataSource(reader_, data_type_);
  }

  ScalarProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ScalarProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    T decoded{};
    try {
      if (!reader_ || !reader_(&value) ||
          !decodeScalar<T, Wire>(value, data_type_, &decoded)) {
        return false;
      }
      last_value_ = std::move(decoded);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  TypeProtocol::VariantReader reader_;
  ::opcua::NodeId data_type_;
  mutable T last_value_{};
};

template <typename T, typename Wire>
class ScalarAssignableProxyDataSource final
    : public RTT::internal::AssignableDataSource<T> {
public:
  ScalarAssignableProxyDataSource(TypeProtocol::VariantReader reader,
                                  TypeProtocol::VariantWriter writer,
                                  ::opcua::NodeId data_type)
      : reader_(std::move(reader)), writer_(std::move(writer)),
        data_type_(std::move(data_type)) {}

  T get() const override {
    refresh();
    return last_value_;
  }

  T value() const override { return last_value_; }

  typename RTT::internal::AssignableDataSource<T>::const_reference_t
  rvalue() const override {
    return last_value_;
  }

  bool evaluate() const override { return refresh(); }

  void
  set(typename RTT::internal::AssignableDataSource<T>::param_t value) override {
    try {
      const ::opcua::Variant encoded(static_cast<Wire>(value));
      if (writer_ && writer_(encoded)) {
        last_value_ = value;
      }
    } catch (const std::exception &) {
    }
  }

  typename RTT::internal::AssignableDataSource<T>::reference_t set() override {
    refresh();
    return last_value_;
  }

  void updated() override { set(last_value_); }

  ScalarAssignableProxyDataSource *clone() const override {
    return new ScalarAssignableProxyDataSource(reader_, writer_, data_type_);
  }

  ScalarAssignableProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ScalarAssignableProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    T decoded{};
    try {
      if (!reader_ || !reader_(&value) ||
          !decodeScalar<T, Wire>(value, data_type_, &decoded)) {
        return false;
      }
      last_value_ = std::move(decoded);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  TypeProtocol::VariantReader reader_;
  TypeProtocol::VariantWriter writer_;
  ::opcua::NodeId data_type_;
  mutable T last_value_{};
};

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

  RTT::base::DataSourceBase::shared_ptr
  makeProxyDataSource(VariantReader reader,
                      VariantWriter writer) const override {
    if (!reader) {
      return {};
    }
    if (writer) {
      return new ScalarAssignableProxyDataSource<T, Wire>(
          std::move(reader), std::move(writer), data_type_);
    }
    return new ScalarProxyDataSource<T, Wire>(std::move(reader), data_type_);
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

  RTT::base::DataSourceBase::shared_ptr
  makeProxyDataSource(VariantReader, VariantWriter) const override {
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
