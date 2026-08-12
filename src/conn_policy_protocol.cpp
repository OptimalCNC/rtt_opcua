#include "conn_policy_protocol.hpp"

#include <rtt/opcua/datatype_registry.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <open62541pp/datatype.hpp>
#include <open62541pp/types.hpp>

#include <rtt/ConnPolicy.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/internal/DataSource.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace RTT::opcua {
namespace {

struct ConnPolicyWire {
  std::int32_t type;
  std::int32_t size;
  std::int32_t lock_policy;
  bool init;
  bool pull;
  std::int32_t buffer_policy;
  std::int32_t max_threads;
  bool mandatory;
  std::int32_t transport;
  std::int32_t data_size;
  ::opcua::String name_id;
};

const LogicalDataTypeId &connPolicyDataTypeId() {
  static const LogicalDataTypeId id{"urn:orocos:rtt", "types/ConnPolicy",
                                    "encodings/ConnPolicy/Binary"};
  return id;
}

bool fail(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

ConnPolicyWire toWire(const RTT::ConnPolicy &policy) {
  return ConnPolicyWire{
      static_cast<std::int32_t>(policy.type),
      static_cast<std::int32_t>(policy.size),
      static_cast<std::int32_t>(policy.lock_policy),
      policy.init,
      policy.pull,
      static_cast<std::int32_t>(policy.buffer_policy),
      static_cast<std::int32_t>(policy.max_threads),
      policy.mandatory,
      static_cast<std::int32_t>(policy.transport),
      static_cast<std::int32_t>(policy.data_size),
      ::opcua::String(policy.name_id),
  };
}

RTT::ConnPolicy fromWire(const ConnPolicyWire &wire) {
  RTT::ConnPolicy policy;
  policy.type = wire.type;
  policy.size = wire.size;
  policy.lock_policy = wire.lock_policy;
  policy.init = wire.init;
  policy.pull = wire.pull;
  policy.buffer_policy = wire.buffer_policy;
  policy.max_threads = wire.max_threads;
  policy.mandatory = wire.mandatory;
  policy.transport = wire.transport;
  policy.data_size = wire.data_size;
  policy.name_id = std::string(std::string_view(wire.name_id));
  return policy;
}

bool decodeConnPolicy(const ::opcua::Variant &value,
                      const ::opcua::NodeId &data_type,
                      RTT::ConnPolicy *policy) noexcept {
  if (policy == nullptr || !value.isScalar() || !value.isType(data_type) ||
      value.data() == nullptr) {
    return false;
  }
  try {
    *policy = fromWire(*static_cast<const ConnPolicyWire *>(value.data()));
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

class ConnPolicyProxyDataSource final
    : public RTT::internal::DataSource<RTT::ConnPolicy> {
public:
  ConnPolicyProxyDataSource(VariantReader reader, ::opcua::NodeId data_type)
      : reader_(std::move(reader)), data_type_(std::move(data_type)) {}

  RTT::ConnPolicy get() const override {
    refresh();
    return last_value_;
  }

  RTT::ConnPolicy value() const override { return last_value_; }

  const_reference_t rvalue() const override { return last_value_; }

  bool evaluate() const override { return refresh(); }

  ConnPolicyProxyDataSource *clone() const override {
    return new ConnPolicyProxyDataSource(reader_, data_type_);
  }

  ConnPolicyProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ConnPolicyProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    RTT::ConnPolicy decoded;
    if (!reader_ || !reader_(&value) ||
        !decodeConnPolicy(value, data_type_, &decoded)) {
      return false;
    }
    last_value_ = std::move(decoded);
    return true;
  }

  VariantReader reader_;
  ::opcua::NodeId data_type_;
  mutable RTT::ConnPolicy last_value_;
};

class ConnPolicyAssignableProxyDataSource final
    : public RTT::internal::AssignableDataSource<RTT::ConnPolicy> {
public:
  ConnPolicyAssignableProxyDataSource(VariantReader reader,
                                      VariantWriter writer,
                                      ::opcua::NodeId data_type,
                                      const UA_DataType *native_type)
      : reader_(std::move(reader)), writer_(std::move(writer)),
        data_type_(std::move(data_type)), native_type_(native_type) {}

  RTT::ConnPolicy get() const override {
    refresh();
    return last_value_;
  }

  RTT::ConnPolicy value() const override { return last_value_; }

  const_reference_t rvalue() const override { return last_value_; }

  bool evaluate() const override { return refresh(); }

  void set(param_t value) override {
    try {
      if (writer_ != nullptr && native_type_ != nullptr &&
          writer_(::opcua::Variant(toWire(value), *native_type_))) {
        last_value_ = value;
      }
    } catch (const std::exception &) {
    }
  }

  reference_t set() override {
    refresh();
    return last_value_;
  }

  void updated() override { set(last_value_); }

  ConnPolicyAssignableProxyDataSource *clone() const override {
    return new ConnPolicyAssignableProxyDataSource(reader_, writer_, data_type_,
                                                   native_type_);
  }

  ConnPolicyAssignableProxyDataSource *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &already_cloned) const override {
    auto *self = const_cast<ConnPolicyAssignableProxyDataSource *>(this);
    already_cloned[this] = self;
    return self;
  }

private:
  bool refresh() const {
    ::opcua::Variant value;
    RTT::ConnPolicy decoded;
    if (!reader_ || !reader_(&value) ||
        !decodeConnPolicy(value, data_type_, &decoded)) {
      return false;
    }
    last_value_ = std::move(decoded);
    return true;
  }

  VariantReader reader_;
  VariantWriter writer_;
  ::opcua::NodeId data_type_;
  const UA_DataType *native_type_;
  mutable RTT::ConnPolicy last_value_;
};

class ConnPolicyTypeCodec final : public TypeCodec {
public:
  ConnPolicyTypeCodec(::opcua::NodeId data_type, const UA_DataType *native_type)
      : TypeCodec(std::move(data_type), ::opcua::ValueRank::Scalar, true),
        native_type_(native_type) {}

  bool toVariant(const RTT::base::DataSourceBase::shared_ptr &source,
                 ::opcua::Variant *value) const override {
    const auto typed =
        boost::dynamic_pointer_cast<RTT::internal::DataSource<RTT::ConnPolicy>>(
            source);
    if (!typed || value == nullptr || native_type_ == nullptr) {
      return false;
    }
    try {
      typed->evaluate();
      *value = ::opcua::Variant(toWire(typed->value()), *native_type_);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  bool assignVariant(
      const ::opcua::Variant &value,
      const RTT::base::DataSourceBase::shared_ptr &destination) const override {
    const auto typed = boost::dynamic_pointer_cast<
        RTT::internal::AssignableDataSource<RTT::ConnPolicy>>(destination);
    RTT::ConnPolicy decoded;
    if (!typed || !decodeConnPolicy(value, dataTypeNodeId(), &decoded)) {
      return false;
    }
    typed->set(decoded);
    return true;
  }

  RTT::base::DataSourceBase::shared_ptr
  makeDataSource(const ::opcua::Variant &value) const override {
    RTT::ConnPolicy decoded;
    if (!decodeConnPolicy(value, dataTypeNodeId(), &decoded)) {
      return {};
    }
    return new RTT::internal::ValueDataSource<RTT::ConnPolicy>(
        std::move(decoded));
  }

  RTT::base::DataSourceBase::shared_ptr
  makeProxyDataSource(VariantReader reader,
                      VariantWriter writer) const override {
    if (!reader) {
      return {};
    }
    if (writer) {
      return new ConnPolicyAssignableProxyDataSource(
          std::move(reader), std::move(writer), dataTypeNodeId(), native_type_);
    }
    return new ConnPolicyProxyDataSource(std::move(reader), dataTypeNodeId());
  }

  PortValueStatus portValue(const RTT::base::OutputPortInterface *port,
                            ::opcua::Variant *value) const override {
    const auto *typed =
        dynamic_cast<const RTT::OutputPort<RTT::ConnPolicy> *>(port);
    if (typed == nullptr || value == nullptr || native_type_ == nullptr) {
      return PortValueStatus::error;
    }
    RTT::ConnPolicy sample;
    if (!typed->getLastWrittenValue(sample)) {
      return PortValueStatus::waiting_for_initial_data;
    }
    try {
      *value = ::opcua::Variant(toWire(sample), *native_type_);
      return PortValueStatus::value;
    } catch (const std::exception &) {
      return PortValueStatus::error;
    }
  }

private:
  const UA_DataType *native_type_;
};

class ConnPolicyTypeProtocol final : public TypeProtocol {
public:
  explicit ConnPolicyTypeProtocol(LogicalDataTypeId data_type)
      : data_type_(std::move(data_type)) {}

  DataTypeReference dataType() const override { return data_type_; }

  std::string registrationFingerprint() const override {
    return "rtt-opcua/ConnPolicy/v1";
  }

  std::unique_ptr<TypeCodec> bind(const ::opcua::NodeId &data_type,
                                  const UA_DataType &native_type,
                                  std::string *error) const override {
    if (::opcua::NodeId(native_type.typeId) != data_type) {
      fail(error, "OPC UA ConnPolicy protocol datatype mismatch");
      return {};
    }
    if (error != nullptr) {
      error->clear();
    }
    return std::make_unique<ConnPolicyTypeCodec>(data_type, &native_type);
  }

private:
  LogicalDataTypeId data_type_;
};

DataTypeProvider makeConnPolicyProvider() {
  CustomDataTypeDefinition definition;
  definition.name = "ConnPolicy";
  definition.id = connPolicyDataTypeId();
  definition.schema_fingerprint = "rtt-opcua/ConnPolicy/v1";
  definition.materialize = [](const DataTypeFactoryContext &context) {
    return ::opcua::DataTypeBuilder<ConnPolicyWire>::createStructure(
               "ConnPolicy", context.nodeId(connPolicyDataTypeId()),
               {context.namespaceIndex("urn:orocos:rtt"),
                "encodings/ConnPolicy/Binary"})
        .addField<&ConnPolicyWire::type>("type")
        .addField<&ConnPolicyWire::size>("size")
        .addField<&ConnPolicyWire::lock_policy>("lock_policy")
        .addField<&ConnPolicyWire::init>("init")
        .addField<&ConnPolicyWire::pull>("pull")
        .addField<&ConnPolicyWire::buffer_policy>("buffer_policy")
        .addField<&ConnPolicyWire::max_threads>("max_threads")
        .addField<&ConnPolicyWire::mandatory>("mandatory")
        .addField<&ConnPolicyWire::transport>("transport")
        .addField<&ConnPolicyWire::data_size>("data_size")
        .addField<&ConnPolicyWire::name_id>("name_id")
        .build();
  };
  DataTypeProvider provider;
  provider.name = "rtt-foundation";
  provider.namespace_uri = "urn:orocos:rtt";
  provider.data_types.push_back(std::move(definition));
  return provider;
}

} // namespace

bool registerConnPolicyProtocol(RTT::types::TypeInfo *type_info,
                                std::string *error) {
  if (type_info == nullptr || type_info->getTypeName() != "ConnPolicy") {
    return fail(error, "invalid OPC UA ConnPolicy protocol registration");
  }
  if (!registerDataTypeProvider(makeConnPolicyProvider(), error)) {
    return false;
  }
  return registerTypeProtocol(
      type_info,
      std::make_unique<ConnPolicyTypeProtocol>(connPolicyDataTypeId()), error);
}

} // namespace RTT::opcua
