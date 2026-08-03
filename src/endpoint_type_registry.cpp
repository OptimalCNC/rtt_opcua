#include <rtt/opcua/endpoint_type_registry.hpp>

#include "datatype_registry_internal.hpp"

#include <open62541pp/datatype.hpp>
#include <open62541pp/server.hpp>
#include <open62541pp/services/nodemanagement.hpp>
#include <open62541pp/ua/nodeids.hpp>
#include <open62541pp/ua/types.hpp>

#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <algorithm>
#include <exception>
#include <set>
#include <string>
#include <utility>

namespace RTT::opcua {
namespace {

bool fail(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

bool kindMatches(CustomDataTypeKind expected, std::uint8_t actual) {
  switch (expected) {
  case CustomDataTypeKind::structure:
    return actual == UA_DATATYPEKIND_STRUCTURE ||
           actual == UA_DATATYPEKIND_OPTSTRUCT;
  case CustomDataTypeKind::enumeration:
    return actual == UA_DATATYPEKIND_ENUM;
  case CustomDataTypeKind::union_type:
    return actual == UA_DATATYPEKIND_UNION;
  }
  return false;
}

::opcua::NodeId parentDataType(CustomDataTypeKind kind) {
  switch (kind) {
  case CustomDataTypeKind::structure:
    return ::opcua::DataTypeId::Structure;
  case CustomDataTypeKind::enumeration:
    return ::opcua::DataTypeId::Enumeration;
  case CustomDataTypeKind::union_type:
    return ::opcua::DataTypeId::Union;
  }
  return ::opcua::DataTypeId::BaseDataType;
}

std::string statusName(::opcua::StatusCode status) {
  return std::string(status.name());
}

} // namespace

std::shared_ptr<EndpointTypeRegistry> EndpointTypeRegistry::create(
    const std::vector<std::pair<std::string, std::uint16_t>> &namespaces,
    std::string *error) {
  auto registry =
      std::shared_ptr<EndpointTypeRegistry>(new EndpointTypeRegistry());
  std::set<std::uint16_t> indexes;
  for (const auto &[uri, index] : namespaces) {
    if (uri.empty() || uri.find('\0') != std::string::npos ||
        !registry->namespace_indexes_.emplace(uri, index).second ||
        !indexes.insert(index).second) {
      fail(error, "invalid OPC UA endpoint namespace table");
      return {};
    }
  }
  const auto namespace_zero =
      registry->namespace_indexes_.find("http://opcfoundation.org/UA/");
  if (namespace_zero == registry->namespace_indexes_.end() ||
      namespace_zero->second != 0U) {
    fail(error, "invalid OPC UA endpoint namespace zero");
    return {};
  }
  if (!registry->bind(error)) {
    return {};
  }
  if (error != nullptr) {
    error->clear();
  }
  return registry;
}

bool EndpointTypeRegistry::bind(std::string *error) {
  const auto providers = detail::frozenDataTypeProviders(error);
  if (!providers) {
    return false;
  }

  factory_state_ = std::make_shared<DataTypeFactoryContext::State>();
  factory_state_->namespace_indexes = namespace_indexes_;
  std::size_t custom_type_count = 0U;
  for (const DataTypeProvider &provider : *providers) {
    if (!namespace_indexes_.contains(provider.namespace_uri)) {
      return fail(error, "missing OPC UA endpoint namespace URI: '" +
                             provider.namespace_uri + "'");
    }
    custom_type_count += provider.data_types.size();
  }
  custom_data_types_.reserve(custom_type_count);
  data_type_nodes_.reserve(custom_type_count);

  const DataTypeFactoryContext context(factory_state_);
  for (const DataTypeProvider &provider : *providers) {
    for (const CustomDataTypeDefinition &definition : provider.data_types) {
      ::opcua::DataType materialized;
      try {
        materialized = definition.materialize(context);
      } catch (const std::exception &exception) {
        return fail(error, "unable to materialize OPC UA datatype '" +
                               definition.name + "': " + exception.what());
      } catch (...) {
        return fail(error, "unable to materialize OPC UA datatype '" +
                               definition.name + "': unknown exception");
      }

      const auto namespace_index =
          namespace_indexes_.at(definition.id.namespace_uri);
      const ::opcua::NodeId expected_type(namespace_index,
                                          definition.id.type_node_id);
      const ::opcua::NodeId expected_encoding(
          namespace_index, definition.id.binary_encoding_node_id);
      if (materialized.typeId() != expected_type ||
          materialized.binaryEncodingId() != expected_encoding ||
          !kindMatches(definition.kind, materialized.typeKind())) {
        return fail(error, "materialized OPC UA datatype does not match '" +
                               definition.name + "'");
      }

      custom_data_types_.push_back(std::move(materialized));
      const std::size_t index = custom_data_types_.size() - 1U;
      custom_data_type_indexes_.emplace(definition.id, index);
      data_type_nodes_.push_back({definition.name, definition.kind, index});
      factory_state_->data_types.emplace(definition.id,
                                         custom_data_types_[index].handle());
    }
  }

  for (const std::string &name : RTT::types::Types()->getTypes()) {
    RTT::types::TypeInfo *type_info = RTT::types::Types()->type(name);
    if (type_info == nullptr || !type_info->hasProtocol(kTransportProtocolId)) {
      continue;
    }
    const auto *protocol = dynamic_cast<const TypeProtocol *>(
        type_info->getProtocol(kTransportProtocolId));
    if (protocol == nullptr) {
      return fail(error,
                  "invalid OPC UA type protocol for RTT type '" + name + "'");
    }

    const DataTypeReference reference = protocol->dataType();
    ::opcua::NodeId node_id;
    const UA_DataType *native_type = nullptr;
    if (const auto *builtin = std::get_if<::opcua::NodeId>(&reference)) {
      if (builtin->namespaceIndex() != 0U) {
        return fail(error, "nonzero namespace in builtin OPC UA protocol for "
                           "RTT type '" +
                               name + "'");
      }
      node_id = *builtin;
      native_type = ::opcua::findDataType(node_id);
    } else {
      const auto &logical = std::get<LogicalDataTypeId>(reference);
      const ::opcua::DataType *custom = dataType(logical);
      if (custom != nullptr) {
        node_id = custom->typeId();
        native_type = custom->handle();
      }
    }
    if (native_type == nullptr) {
      return fail(error,
                  "unresolved OPC UA datatype for RTT type '" + name + "'");
    }

    std::string bind_error;
    std::unique_ptr<TypeCodec> codec =
        protocol->bind(node_id, *native_type, &bind_error);
    if (!codec || codec->dataTypeNodeId() != node_id) {
      return fail(error, "unable to bind OPC UA codec for RTT type '" + name +
                             "': " + bind_error);
    }
    TypeCodec *codec_ptr = codec.get();
    codecs_.emplace(type_info, std::move(codec));
    codecs_by_name_.emplace(name, codec_ptr);
  }

  if (error != nullptr) {
    error->clear();
  }
  return true;
}

const TypeCodec *EndpointTypeRegistry::codecForTypeInfo(
    const RTT::types::TypeInfo *type_info) const {
  const auto found = codecs_.find(type_info);
  return found == codecs_.end() ? nullptr : found->second.get();
}

const TypeCodec *
EndpointTypeRegistry::codecForTypeName(std::string_view type_name) const {
  const auto found = codecs_by_name_.find(type_name);
  return found == codecs_by_name_.end() ? nullptr : found->second;
}

const TypeCodec *EndpointTypeRegistry::codecForDataSource(
    const RTT::base::DataSourceBase::shared_ptr &source) const {
  return source ? codecForTypeInfo(source->getTypeInfo()) : nullptr;
}

::opcua::Span<const ::opcua::DataType>
EndpointTypeRegistry::customDataTypes() const noexcept {
  return custom_data_types_;
}

const ::opcua::DataType *
EndpointTypeRegistry::dataType(const LogicalDataTypeId &id) const noexcept {
  const auto found = custom_data_type_indexes_.find(id);
  return found == custom_data_type_indexes_.end()
             ? nullptr
             : &custom_data_types_[found->second];
}

std::optional<std::uint16_t> EndpointTypeRegistry::namespaceIndex(
    std::string_view namespace_uri) const noexcept {
  const auto found = namespace_indexes_.find(namespace_uri);
  return found == namespace_indexes_.end()
             ? std::nullopt
             : std::optional<std::uint16_t>(found->second);
}

bool EndpointTypeRegistry::publishDataTypeNodes(::opcua::Server &server,
                                                std::string *error) const {
  for (const DataTypeNode &node : data_type_nodes_) {
    const ::opcua::DataType &data_type =
        custom_data_types_[node.data_type_index];

    ::opcua::DataTypeAttributes type_attributes;
    type_attributes.setDisplayName(::opcua::LocalizedText("en-US", node.name));
    type_attributes.setIsAbstract(false);
    const auto type_result = ::opcua::services::addDataType(
        server, parentDataType(node.kind), data_type.typeId(), node.name,
        type_attributes, ::opcua::ReferenceTypeId::HasSubtype);
    if (!type_result) {
      return fail(error, "failed to publish OPC UA datatype node '" +
                             node.name +
                             "': " + statusName(type_result.code()));
    }

    ::opcua::ObjectAttributes encoding_attributes;
    encoding_attributes.setDisplayName(
        ::opcua::LocalizedText("en-US", "Default Binary"));
    const auto encoding_result = ::opcua::services::addObject(
        server, ::opcua::NodeId{}, data_type.binaryEncodingId(),
        "Default Binary", encoding_attributes,
        ::opcua::ObjectTypeId::DataTypeEncodingType, ::opcua::NodeId{});
    if (!encoding_result) {
      return fail(error, "failed to publish OPC UA binary encoding node for '" +
                             node.name +
                             "': " + statusName(encoding_result.code()));
    }
    const ::opcua::StatusCode reference_result =
        ::opcua::services::addReference(
            server, data_type.typeId(), data_type.binaryEncodingId(),
            ::opcua::ReferenceTypeId::HasEncoding, true);
    if (!reference_result.isGood()) {
      return fail(error, "failed to link OPC UA binary encoding node for '" +
                             node.name + "': " + statusName(reference_result));
    }
  }
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace RTT::opcua
