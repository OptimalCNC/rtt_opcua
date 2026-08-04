#include <rtt/opcua/type_protocol.hpp>

#include "conn_policy_protocol.hpp"

#include <rtt/opcua/type_descriptor.hpp>

#include <open62541pp/datatype.hpp>
#include <open62541pp/ua/nodeids.hpp>

#include <rtt/OutputPort.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/rt_string.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace RTT::opcua {
namespace {

std::mutex &registrationMutex() {
  static std::mutex mutex;
  return mutex;
}

bool fail(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

template <typename T, typename Wire> Wire encodeScalar(const T &value) {
  if constexpr (std::is_same_v<T, RTT::rt_string> &&
                std::is_same_v<Wire, std::string>) {
    return std::string(value.c_str());
  } else {
    return static_cast<Wire>(value);
  }
}

template <typename T, typename Wire> T decodeScalarValue(const Wire &value) {
  if constexpr (std::is_same_v<T, RTT::rt_string> &&
                std::is_same_v<Wire, std::string>) {
    return RTT::rt_string(value.c_str());
  } else {
    return static_cast<T>(value);
  }
}

template <typename T, typename Wire>
bool decodeScalar(const ::opcua::Variant &value,
                  const ::opcua::NodeId &data_type, T *decoded) noexcept {
  if (decoded == nullptr || !value.isScalar() || !value.isType(data_type)) {
    return false;
  }
  try {
    *decoded = decodeScalarValue<T, Wire>(value.to<Wire>());
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

template <typename T, typename Wire>
class ScalarProxyDataSource final : public RTT::internal::DataSource<T> {
public:
  ScalarProxyDataSource(VariantReader reader, ::opcua::NodeId data_type)
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

  VariantReader reader_;
  ::opcua::NodeId data_type_;
  mutable T last_value_{};
};

template <typename T, typename Wire>
class ScalarAssignableProxyDataSource final
    : public RTT::internal::AssignableDataSource<T> {
public:
  ScalarAssignableProxyDataSource(VariantReader reader, VariantWriter writer,
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
      const ::opcua::Variant encoded(encodeScalar<T, Wire>(value));
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

  VariantReader reader_;
  VariantWriter writer_;
  ::opcua::NodeId data_type_;
  mutable T last_value_{};
};

bool isCompatibleArray(const ::opcua::Variant &value,
                       const ::opcua::NodeId &element_type) noexcept {
  return value.isArray() && value.isType(element_type) &&
         value.arrayDimensions().size() <= 1U;
}

template <typename T>
class ArrayProxyDataSource final
    : public RTT::internal::DataSource<std::vector<T>> {
public:
  using Value = std::vector<T>;

  ArrayProxyDataSource(VariantReader reader, ::opcua::NodeId element_type)
      : reader_(std::move(reader)), element_type_(std::move(element_type)) {}

  Value get() const override {
    refresh();
    return last_value_;
  }

  Value value() const override { return last_value_; }

  typename RTT::internal::DataSource<Value>::const_reference_t
  rvalue() const override {
    return last_value_;
  }

  bool evaluate() const override { return refresh(); }

  ArrayProxyDataSource *clone() const override {
    return new ArrayProxyDataSource(reader_, element_type_);
  }

  ArrayProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ArrayProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    try {
      if (!reader_ || !reader_(&value) ||
          !isCompatibleArray(value, element_type_)) {
        return false;
      }
      last_value_ = value.to<Value>();
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  VariantReader reader_;
  ::opcua::NodeId element_type_;
  mutable Value last_value_;
};

template <typename T>
class ArrayAssignableProxyDataSource final
    : public RTT::internal::AssignableDataSource<std::vector<T>> {
public:
  using Value = std::vector<T>;

  ArrayAssignableProxyDataSource(VariantReader reader, VariantWriter writer,
                                 ::opcua::NodeId element_type)
      : reader_(std::move(reader)), writer_(std::move(writer)),
        element_type_(std::move(element_type)) {}

  Value get() const override {
    refresh();
    return last_value_;
  }

  Value value() const override { return last_value_; }

  typename RTT::internal::AssignableDataSource<Value>::const_reference_t
  rvalue() const override {
    return last_value_;
  }

  bool evaluate() const override { return refresh(); }

  void set(typename RTT::internal::AssignableDataSource<Value>::param_t value)
      override {
    try {
      const ::opcua::Variant encoded(value);
      if (writer_ && writer_(encoded)) {
        last_value_ = value;
      }
    } catch (const std::exception &) {
    }
  }

  typename RTT::internal::AssignableDataSource<Value>::reference_t
  set() override {
    refresh();
    return last_value_;
  }

  void updated() override { set(last_value_); }

  ArrayAssignableProxyDataSource *clone() const override {
    return new ArrayAssignableProxyDataSource(reader_, writer_, element_type_);
  }

  ArrayAssignableProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ArrayAssignableProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    try {
      if (!reader_ || !reader_(&value) ||
          !isCompatibleArray(value, element_type_)) {
        return false;
      }
      last_value_ = value.to<Value>();
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  VariantReader reader_;
  VariantWriter writer_;
  ::opcua::NodeId element_type_;
  mutable Value last_value_;
};

template <typename T, typename Wire = T>
class ScalarTypeCodec final : public TypeCodec {
public:
  explicit ScalarTypeCodec(::opcua::NodeId data_type)
      : TypeCodec(std::move(data_type), ::opcua::ValueRank::Scalar, true) {}

  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                 ::opcua::Variant *value) const override {
    typename RTT::internal::DataSource<T>::shared_ptr typed =
        boost::dynamic_pointer_cast<RTT::internal::DataSource<T>>(source);
    if (!typed || value == nullptr) {
      return false;
    }
    typed->evaluate();
    *value = ::opcua::Variant(encodeScalar<T, Wire>(typed->value()));
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
      typed->set(decodeScalarValue<T, Wire>(value.to<Wire>()));
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
          decodeScalarValue<T, Wire>(value.to<Wire>()));
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
          std::move(reader), std::move(writer), dataTypeNodeId());
    }
    return new ScalarProxyDataSource<T, Wire>(std::move(reader),
                                              dataTypeNodeId());
  }

  bool portValue(const RTT::base::OutputPortInterface *port,
                 ::opcua::Variant *value) const override {
    const auto *typed = dynamic_cast<const RTT::OutputPort<T> *>(port);
    if (typed == nullptr || value == nullptr) {
      return false;
    }
    *value =
        ::opcua::Variant(encodeScalar<T, Wire>(typed->getLastWrittenValue()));
    return true;
  }

private:
  bool isCompatible(const ::opcua::Variant &value) const noexcept {
    return value.isScalar() && value.isType(dataTypeNodeId());
  }
};

template <typename T> class ArrayTypeCodec final : public TypeCodec {
public:
  using Value = std::vector<T>;

  explicit ArrayTypeCodec(::opcua::NodeId element_type)
      : TypeCodec(std::move(element_type), ::opcua::ValueRank::OneDimension,
                  true) {}

  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                 ::opcua::Variant *value) const override {
    typename RTT::internal::DataSource<Value>::shared_ptr typed =
        boost::dynamic_pointer_cast<RTT::internal::DataSource<Value>>(source);
    if (!typed || value == nullptr) {
      return false;
    }
    typed->evaluate();
    *value = ::opcua::Variant(typed->value());
    return true;
  }

  bool assignVariant(
      const ::opcua::Variant &value,
      const RTT::base::DataSourceBase::shared_ptr &destination) const override {
    typename RTT::internal::AssignableDataSource<Value>::shared_ptr typed =
        boost::dynamic_pointer_cast<RTT::internal::AssignableDataSource<Value>>(
            destination);
    if (!typed || !isCompatibleArray(value, dataTypeNodeId())) {
      return false;
    }
    try {
      typed->set(value.to<Value>());
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &value) const override {
    if (!isCompatibleArray(value, dataTypeNodeId())) {
      return {};
    }
    try {
      return new RTT::internal::ValueDataSource<Value>(value.to<Value>());
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
      return new ArrayAssignableProxyDataSource<T>(
          std::move(reader), std::move(writer), dataTypeNodeId());
    }
    return new ArrayProxyDataSource<T>(std::move(reader), dataTypeNodeId());
  }

  bool portValue(const RTT::base::OutputPortInterface *port,
                 ::opcua::Variant *value) const override {
    const auto *typed = dynamic_cast<const RTT::OutputPort<Value> *>(port);
    if (typed == nullptr || value == nullptr) {
      return false;
    }
    *value = ::opcua::Variant(typed->getLastWrittenValue());
    return true;
  }
};

class VoidTypeCodec final : public TypeCodec {
public:
  explicit VoidTypeCodec(::opcua::NodeId data_type)
      : TypeCodec(std::move(data_type), ::opcua::ValueRank::Scalar, false) {}

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
};

template <typename T, typename Wire = T>
class ScalarTypeProtocol final : public TypeProtocol {
public:
  ScalarTypeProtocol(::opcua::NodeId data_type, std::string fingerprint)
      : data_type_(std::move(data_type)), fingerprint_(std::move(fingerprint)) {
  }

  DataTypeReference dataType() const override { return data_type_; }

  std::string registrationFingerprint() const override { return fingerprint_; }

  std::unique_ptr<TypeCodec> bind(const ::opcua::NodeId &data_type,
                                  const UA_DataType &native_type,
                                  std::string *error) const override {
    if (data_type != data_type_ ||
        ::opcua::NodeId(native_type.typeId) != data_type) {
      fail(error, "OPC UA scalar protocol datatype mismatch");
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<ScalarTypeCodec<T, Wire>>(data_type);
  }

private:
  ::opcua::NodeId data_type_;
  std::string fingerprint_;
};

template <typename T> class ArrayTypeProtocol final : public TypeProtocol {
public:
  ArrayTypeProtocol(::opcua::NodeId element_type, std::string fingerprint)
      : element_type_(std::move(element_type)),
        fingerprint_(std::move(fingerprint)) {}

  DataTypeReference dataType() const override { return element_type_; }

  std::string registrationFingerprint() const override { return fingerprint_; }

  std::unique_ptr<TypeCodec> bind(const ::opcua::NodeId &data_type,
                                  const UA_DataType &native_type,
                                  std::string *error) const override {
    if (data_type != element_type_ ||
        ::opcua::NodeId(native_type.typeId) != data_type) {
      fail(error, "OPC UA array protocol element datatype mismatch");
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<ArrayTypeCodec<T>>(data_type);
  }

private:
  ::opcua::NodeId element_type_;
  std::string fingerprint_;
};

class VoidTypeProtocol final : public TypeProtocol {
public:
  DataTypeReference dataType() const override {
    return ::opcua::NodeId(::opcua::DataTypeId::BaseDataType);
  }

  std::string registrationFingerprint() const override {
    return "rtt-opcua/builtin/Void/v1";
  }

  std::unique_ptr<TypeCodec> bind(const ::opcua::NodeId &data_type,
                                  const UA_DataType &native_type,
                                  std::string *error) const override {
    if (data_type != ::opcua::NodeId(::opcua::DataTypeId::BaseDataType) ||
        ::opcua::NodeId(native_type.typeId) != data_type) {
      fail(error, "OPC UA void protocol datatype mismatch");
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<VoidTypeCodec>(data_type);
  }
};

std::unique_ptr<TypeProtocol>
makeCanonicalProtocol(std::string_view type_name) {
  const TypeDescriptor *descriptor = descriptorForType(type_name);
  if (descriptor == nullptr) {
    return {};
  }
  const std::string fingerprint =
      "rtt-opcua/builtin/" + std::string(type_name) + "/v1";

  if (type_name == "Bool") {
    return std::make_unique<ScalarTypeProtocol<bool>>(descriptor->data_type,
                                                      fingerprint);
  }
  if (type_name == "Int8") {
    return std::make_unique<ScalarTypeProtocol<std::int8_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "UInt8") {
    return std::make_unique<ScalarTypeProtocol<std::uint8_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Int16") {
    return std::make_unique<ScalarTypeProtocol<std::int16_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "UInt16") {
    return std::make_unique<ScalarTypeProtocol<std::uint16_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Int32") {
    return std::make_unique<ScalarTypeProtocol<std::int32_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "UInt32") {
    return std::make_unique<ScalarTypeProtocol<std::uint32_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Int64") {
    return std::make_unique<ScalarTypeProtocol<std::int64_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "UInt64") {
    return std::make_unique<ScalarTypeProtocol<std::uint64_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Float32") {
    return std::make_unique<ScalarTypeProtocol<float>>(descriptor->data_type,
                                                       fingerprint);
  }
  if (type_name == "Float64") {
    return std::make_unique<ScalarTypeProtocol<double>>(descriptor->data_type,
                                                        fingerprint);
  }
  if (type_name == "Char") {
    using CharWire =
        std::conditional_t<std::is_signed_v<char>, std::int8_t, std::uint8_t>;
    return std::make_unique<ScalarTypeProtocol<char, CharWire>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "String") {
    return std::make_unique<ScalarTypeProtocol<std::string>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Float64Array") {
    return std::make_unique<ArrayTypeProtocol<double>>(descriptor->data_type,
                                                       fingerprint);
  }
  if (type_name == "Int32Array") {
    return std::make_unique<ArrayTypeProtocol<std::int32_t>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "StringArray") {
    return std::make_unique<ArrayTypeProtocol<std::string>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "RtString") {
    return std::make_unique<ScalarTypeProtocol<RTT::rt_string, std::string>>(
        descriptor->data_type, fingerprint);
  }
  if (type_name == "Void") {
    return std::make_unique<VoidTypeProtocol>();
  }
  return {};
}

bool sameRegistration(const TypeProtocol &lhs, const TypeProtocol &rhs) {
  return lhs.dataType() == rhs.dataType() &&
         lhs.registrationFingerprint() == rhs.registrationFingerprint();
}

bool registerTypeProtocolUnlocked(RTT::types::TypeInfo *type_info,
                                  std::unique_ptr<TypeProtocol> protocol,
                                  std::string *error) {
  if (type_info == nullptr || !protocol ||
      protocol->registrationFingerprint().empty()) {
    return fail(error, "invalid OPC UA type protocol registration");
  }
  if (type_info->hasProtocol(kTransportProtocolId)) {
    const auto *existing = dynamic_cast<const TypeProtocol *>(
        type_info->getProtocol(kTransportProtocolId));
    if (existing != nullptr && sameRegistration(*existing, *protocol)) {
      if (error != nullptr) {
        error->clear();
      }
      return true;
    }
    return fail(error, "conflicting OPC UA type protocol registration for '" +
                           type_info->getTypeName() + "'");
  }
  if (dataTypeRegistryFrozen()) {
    return fail(error, "late OPC UA type protocol registration for '" +
                           type_info->getTypeName() + "'");
  }
  if (!type_info->addProtocol(kTransportProtocolId, protocol.release())) {
    return fail(error, "invalid OPC UA type protocol registration for '" +
                           type_info->getTypeName() + "'");
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace

TypeCodec::TypeCodec(::opcua::NodeId data_type, ::opcua::ValueRank value_rank,
                     bool has_value)
    : data_type_(std::move(data_type)), value_rank_(value_rank),
      has_value_(has_value) {}

const ::opcua::NodeId &TypeCodec::dataTypeNodeId() const noexcept {
  return data_type_;
}

::opcua::ValueRank TypeCodec::valueRank() const noexcept { return value_rank_; }

bool TypeCodec::hasValue() const noexcept { return has_value_; }

RTT::base::ChannelElementBase::shared_ptr
TypeProtocol::createStream(RTT::base::PortInterface *, const RTT::ConnPolicy &,
                           bool) const {
  return {};
}

bool registerTypeProtocol(RTT::types::TypeInfo *type_info,
                          std::unique_ptr<TypeProtocol> protocol,
                          std::string *error) {
  std::lock_guard<std::mutex> lock(registrationMutex());
  return registerTypeProtocolUnlocked(type_info, std::move(protocol), error);
}

bool registerCanonicalTypeProtocol(std::string_view type_name,
                                   RTT::types::TypeInfo *type_info,
                                   std::string *error) {
  if (type_name == "ConnPolicy") {
    return registerConnPolicyProtocol(type_info, error);
  }
  if (type_info == nullptr || type_info->getTypeName() != type_name ||
      descriptorForType(type_name) == nullptr) {
    return fail(error, "invalid canonical OPC UA type protocol registration");
  }
  std::lock_guard<std::mutex> lock(registrationMutex());
  return registerTypeProtocolUnlocked(type_info,
                                      makeCanonicalProtocol(type_name), error);
}

bool registerCanonicalTypeProtocols(std::string *error) {
  for (const TypeDescriptor &descriptor : canonicalTypeDescriptors()) {
    RTT::types::TypeInfo *type_info =
        RTT::types::Types()->type(std::string(descriptor.rtt_name));
    if (!registerCanonicalTypeProtocol(descriptor.rtt_name, type_info, error)) {
      return false;
    }
  }
  return registerConnPolicyProtocol(RTT::types::Types()->type("ConnPolicy"),
                                    error);
}

} // namespace RTT::opcua
