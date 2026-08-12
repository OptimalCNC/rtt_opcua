#pragma once

#include <rtt/opcua/datatype_registry.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <open62541pp/datatype.hpp>

#include <rtt/OutputPort.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace RTT::opcua::test {

inline constexpr std::string_view kFixtureTypeName = "FixtureValue";
inline constexpr std::string_view kFixtureProviderName = "proxy-fixture";
inline constexpr std::string_view kFixtureNamespaceUri =
    "urn:test:rtt-opcua:proxy-types";

struct FixtureValue {
  std::int32_t count{0};
  double scale{0.0};

  auto operator<=>(const FixtureValue &) const = default;
};

inline const LogicalDataTypeId &fixtureDataTypeId() {
  static const LogicalDataTypeId id{std::string(kFixtureNamespaceUri),
                                    "types/FixtureValue",
                                    "encodings/FixtureValue/Binary"};
  return id;
}

inline bool decodeFixtureValue(const ::opcua::Variant &variant,
                               const ::opcua::NodeId &data_type,
                               FixtureValue *value) noexcept {
  if (value == nullptr || !variant.isScalar() || !variant.isType(data_type) ||
      variant.data() == nullptr) {
    return false;
  }
  *value = *static_cast<const FixtureValue *>(variant.data());
  return true;
}

class FixtureProxyDataSource final
    : public RTT::internal::DataSource<FixtureValue> {
public:
  FixtureProxyDataSource(VariantReader reader, ::opcua::NodeId data_type)
      : reader_(std::move(reader)), data_type_(std::move(data_type)) {}

  FixtureValue get() const override {
    refresh();
    return last_value_;
  }

  FixtureValue value() const override { return last_value_; }

  const_reference_t rvalue() const override { return last_value_; }

  bool evaluate() const override { return refresh(); }

  FixtureProxyDataSource *clone() const override {
    return new FixtureProxyDataSource(reader_, data_type_);
  }

  FixtureProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<FixtureProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant variant;
    FixtureValue decoded;
    if (!reader_ || !reader_(&variant) ||
        !decodeFixtureValue(variant, data_type_, &decoded)) {
      return false;
    }
    last_value_ = decoded;
    return true;
  }

  VariantReader reader_;
  ::opcua::NodeId data_type_;
  mutable FixtureValue last_value_;
};

class FixtureAssignableProxyDataSource final
    : public RTT::internal::AssignableDataSource<FixtureValue> {
public:
  FixtureAssignableProxyDataSource(VariantReader reader, VariantWriter writer,
                                   ::opcua::NodeId data_type,
                                   const UA_DataType *native_type)
      : reader_(std::move(reader)), writer_(std::move(writer)),
        data_type_(std::move(data_type)), native_type_(native_type) {}

  FixtureValue get() const override {
    refresh();
    return last_value_;
  }

  FixtureValue value() const override { return last_value_; }

  const_reference_t rvalue() const override { return last_value_; }

  bool evaluate() const override { return refresh(); }

  void set(param_t value) override {
    if (writer_ != nullptr && native_type_ != nullptr &&
        writer_(::opcua::Variant(value, *native_type_))) {
      last_value_ = value;
    }
  }

  reference_t set() override {
    refresh();
    return last_value_;
  }

  void updated() override { set(last_value_); }

  FixtureAssignableProxyDataSource *clone() const override {
    return new FixtureAssignableProxyDataSource(reader_, writer_, data_type_,
                                                native_type_);
  }

  FixtureAssignableProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<FixtureAssignableProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant variant;
    FixtureValue decoded;
    if (!reader_ || !reader_(&variant) ||
        !decodeFixtureValue(variant, data_type_, &decoded)) {
      return false;
    }
    last_value_ = decoded;
    return true;
  }

  VariantReader reader_;
  VariantWriter writer_;
  ::opcua::NodeId data_type_;
  const UA_DataType *native_type_;
  mutable FixtureValue last_value_;
};

class FixtureTypeCodec final : public TypeCodec {
public:
  FixtureTypeCodec(::opcua::NodeId data_type, const UA_DataType *native_type)
      : TypeCodec(std::move(data_type), ::opcua::ValueRank::Scalar, true),
        native_type_(native_type) {}

  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                 ::opcua::Variant *value) const override {
    auto typed =
        boost::dynamic_pointer_cast<RTT::internal::DataSource<FixtureValue>>(
            source);
    if (!typed || value == nullptr || native_type_ == nullptr) {
      return false;
    }
    typed->evaluate();
    *value = ::opcua::Variant(typed->value(), *native_type_);
    return true;
  }

  bool assignVariant(
      const ::opcua::Variant &value,
      const RTT::base::DataSourceBase::shared_ptr &destination) const override {
    auto typed = boost::dynamic_pointer_cast<
        RTT::internal::AssignableDataSource<FixtureValue>>(destination);
    FixtureValue decoded;
    if (!typed || !decodeFixtureValue(value, dataTypeNodeId(), &decoded)) {
      return false;
    }
    typed->set(decoded);
    return true;
  }

  RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &value) const override {
    FixtureValue decoded;
    if (!decodeFixtureValue(value, dataTypeNodeId(), &decoded)) {
      return {};
    }
    return new RTT::internal::ValueDataSource<FixtureValue>(decoded);
  }

  RTT::base::DataSourceBase::shared_ptr
  makeProxyDataSource(VariantReader reader,
                      VariantWriter writer = {}) const override {
    if (!reader) {
      return {};
    }
    if (writer) {
      return new FixtureAssignableProxyDataSource(
          std::move(reader), std::move(writer), dataTypeNodeId(), native_type_);
    }
    return new FixtureProxyDataSource(std::move(reader), dataTypeNodeId());
  }

  PortValueStatus portValue(const RTT::base::OutputPortInterface *port,
                            ::opcua::Variant *value) const override {
    const auto *typed =
        dynamic_cast<const RTT::OutputPort<FixtureValue> *>(port);
    if (typed == nullptr || value == nullptr || native_type_ == nullptr) {
      return PortValueStatus::error;
    }
    FixtureValue sample;
    if (!typed->getLastWrittenValue(sample)) {
      return PortValueStatus::waiting_for_initial_data;
    }
    *value = ::opcua::Variant(sample, *native_type_);
    return PortValueStatus::value;
  }

private:
  const UA_DataType *native_type_;
};

class FixtureTypeProtocol final : public TypeProtocol {
public:
  DataTypeReference dataType() const override { return fixtureDataTypeId(); }

  std::string registrationFingerprint() const override {
    return "proxy-fixture/FixtureValue/v1";
  }

  std::unique_ptr<TypeCodec> bind(const ::opcua::NodeId &data_type,
                                  const UA_DataType &native_type,
                                  std::string *error) const override {
    if (::opcua::NodeId(native_type.typeId) != data_type) {
      if (error != nullptr) {
        *error = "fixture protocol datatype mismatch";
      }
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<FixtureTypeCodec>(data_type, &native_type);
  }
};

inline bool registerFixtureType(std::string *error = nullptr) {
  RTT::types::TypeInfo *type_info =
      RTT::types::Types()->type(std::string(kFixtureTypeName));
  if (type_info == nullptr) {
    if (!RTT::types::Types()->addType(
            new RTT::types::TemplateTypeInfo<FixtureValue, false>(
                std::string(kFixtureTypeName)))) {
      if (error != nullptr) {
        *error = "unable to register fixture RTT type";
      }
      return false;
    }
    type_info = RTT::types::Types()->type(std::string(kFixtureTypeName));
  }

  const LogicalDataTypeId id = fixtureDataTypeId();
  CustomDataTypeDefinition definition;
  definition.name = std::string(kFixtureTypeName);
  definition.id = id;
  definition.schema_fingerprint = "proxy-fixture-value-v1";
  definition.materialize = [id](const DataTypeFactoryContext &context) {
    return ::opcua::DataTypeBuilder<FixtureValue>::createStructure(
               std::string(kFixtureTypeName), context.nodeId(id),
               {context.namespaceIndex(id.namespace_uri),
                id.binary_encoding_node_id})
        .addField<&FixtureValue::count>("count")
        .addField<&FixtureValue::scale>("scale")
        .build();
  };
  DataTypeProvider provider;
  provider.name = std::string(kFixtureProviderName);
  provider.namespace_uri = std::string(kFixtureNamespaceUri);
  provider.data_types.push_back(std::move(definition));
  return registerDataTypeProvider(std::move(provider), error) &&
         registerTypeProtocol(type_info,
                              std::make_unique<FixtureTypeProtocol>(), error);
}

} // namespace RTT::opcua::test
