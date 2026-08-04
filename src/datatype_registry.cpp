#include <rtt/opcua/datatype_registry.hpp>

#include "datatype_registry_internal.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace RTT::opcua {
namespace {

struct RegistryState {
  std::mutex mutex;
  std::map<std::string, DataTypeProvider, std::less<>> providers;
  std::vector<std::string> provider_order;
  bool frozen{false};
};

RegistryState &registry() {
  static RegistryState state;
  return state;
}

bool containsNul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

bool validText(std::string_view value) {
  return !value.empty() && !containsNul(value);
}

bool fail(std::string *error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

bool sameDefinition(const CustomDataTypeDefinition &lhs,
                    const CustomDataTypeDefinition &rhs) {
  return lhs.name == rhs.name && lhs.id == rhs.id && lhs.kind == rhs.kind &&
         lhs.schema_fingerprint == rhs.schema_fingerprint &&
         static_cast<bool>(lhs.materialize) ==
             static_cast<bool>(rhs.materialize);
}

bool sameProvider(const DataTypeProvider &lhs, const DataTypeProvider &rhs) {
  return lhs.name == rhs.name && lhs.namespace_uri == rhs.namespace_uri &&
         lhs.dependencies == rhs.dependencies &&
         lhs.data_types.size() == rhs.data_types.size() &&
         std::equal(lhs.data_types.begin(), lhs.data_types.end(),
                    rhs.data_types.begin(), rhs.data_types.end(),
                    sameDefinition);
}

using LogicalNodeId = std::pair<std::string, std::string>;

std::optional<std::string>
validateAndNormalize(DataTypeProvider *provider,
                     const RegistryState &state) {
  if (provider == nullptr || !validText(provider->name) ||
      !validText(provider->namespace_uri) || provider->data_types.empty()) {
    return "invalid OPC UA datatype provider: name, namespace URI, and "
           "datatype list must be nonempty";
  }

  for (const std::string &dependency : provider->dependencies) {
    if (!validText(dependency) || dependency == provider->name) {
      return "invalid OPC UA datatype provider: invalid dependency for '" +
             provider->name + "'";
    }
  }
  std::sort(provider->dependencies.begin(), provider->dependencies.end());
  if (std::adjacent_find(provider->dependencies.begin(),
                         provider->dependencies.end()) !=
      provider->dependencies.end()) {
    return "invalid OPC UA datatype provider: duplicate dependency for '" +
           provider->name + "'";
  }

  std::set<LogicalNodeId> occupied;
  for (const auto &[name, existing] : state.providers) {
    static_cast<void>(name);
    for (const CustomDataTypeDefinition &definition : existing.data_types) {
      occupied.emplace(definition.id.namespace_uri,
                       definition.id.type_node_id);
      occupied.emplace(definition.id.namespace_uri,
                       definition.id.binary_encoding_node_id);
    }
  }

  for (const CustomDataTypeDefinition &definition : provider->data_types) {
    if (!validText(definition.name) ||
        !validText(definition.schema_fingerprint) ||
        !validText(definition.id.namespace_uri) ||
        !validText(definition.id.type_node_id) ||
        !validText(definition.id.binary_encoding_node_id) ||
        !definition.materialize) {
      return "invalid OPC UA datatype provider: invalid datatype definition "
             "in '" +
             provider->name + "'";
    }
    if (definition.id.namespace_uri != provider->namespace_uri) {
      return "invalid OPC UA datatype provider: datatype namespace does not "
             "match provider '" +
             provider->name + "'";
    }
    const LogicalNodeId type_id{definition.id.namespace_uri,
                                definition.id.type_node_id};
    const LogicalNodeId encoding_id{definition.id.namespace_uri,
                                    definition.id.binary_encoding_node_id};
    if (type_id == encoding_id || !occupied.insert(type_id).second ||
        !occupied.insert(encoding_id).second) {
      return "conflicting OPC UA datatype provider: NodeId collision in '" +
             provider->name + "'";
    }
  }
  return std::nullopt;
}

std::optional<std::vector<std::string>>
computeProviderOrder(const RegistryState &state, std::string *error) {
  for (const auto &[name, provider] : state.providers) {
    for (const std::string &dependency : provider.dependencies) {
      if (!state.providers.contains(dependency)) {
        fail(error,
             "missing OPC UA datatype provider dependency: provider '" +
                 name + "' requires '" + dependency + "'");
        return std::nullopt;
      }
    }
  }

  std::map<std::string, std::size_t, std::less<>> indegree;
  std::map<std::string, std::vector<std::string>, std::less<>> dependents;
  for (const auto &[name, provider] : state.providers) {
    indegree.emplace(name, provider.dependencies.size());
    for (const std::string &dependency : provider.dependencies) {
      dependents[dependency].push_back(name);
    }
  }
  for (auto &[dependency, names] : dependents) {
    static_cast<void>(dependency);
    std::sort(names.begin(), names.end());
  }

  std::set<std::string, std::less<>> ready;
  for (const auto &[name, count] : indegree) {
    if (count == 0U) {
      ready.insert(name);
    }
  }

  std::vector<std::string> order;
  order.reserve(state.providers.size());
  while (!ready.empty()) {
    auto first = ready.begin();
    std::string name = *first;
    ready.erase(first);
    order.push_back(name);
    for (const std::string &dependent : dependents[name]) {
      std::size_t &count = indegree[dependent];
      --count;
      if (count == 0U) {
        ready.insert(dependent);
      }
    }
  }

  if (order.size() != state.providers.size()) {
    std::ostringstream message;
    message << "cyclic OPC UA datatype provider dependency:";
    for (const auto &[name, count] : indegree) {
      if (count != 0U) {
        message << " " << name;
      }
    }
    fail(error, message.str());
    return std::nullopt;
  }
  return order;
}

} // namespace

DataTypeFactoryContext::DataTypeFactoryContext(
    std::shared_ptr<const State> state)
    : state_(std::move(state)) {}

std::uint16_t DataTypeFactoryContext::namespaceIndex(
    std::string_view namespace_uri) const {
  if (!state_) {
    throw std::logic_error("OPC UA datatype factory context is not bound");
  }
  const auto found = state_->namespace_indexes.find(namespace_uri);
  if (found == state_->namespace_indexes.end()) {
    throw std::out_of_range("OPC UA namespace URI is not bound: " +
                            std::string(namespace_uri));
  }
  return found->second;
}

::opcua::NodeId
DataTypeFactoryContext::nodeId(const LogicalDataTypeId &id) const {
  return {namespaceIndex(id.namespace_uri), id.type_node_id};
}

const UA_DataType *
DataTypeFactoryContext::dataType(const LogicalDataTypeId &id) const noexcept {
  if (!state_) {
    return nullptr;
  }
  const auto found = state_->data_types.find(id);
  return found == state_->data_types.end() ? nullptr : found->second;
}

bool registerDataTypeProvider(DataTypeProvider provider, std::string *error) {
  RegistryState &state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::sort(provider.dependencies.begin(), provider.dependencies.end());
  const auto existing = state.providers.find(provider.name);
  if (existing != state.providers.end() &&
      sameProvider(existing->second, provider)) {
    if (error != nullptr) {
      error->clear();
    }
    return true;
  }
  if (state.frozen) {
    return fail(error, "late OPC UA datatype provider registration: '" +
                           provider.name + "'");
  }
  if (existing != state.providers.end()) {
    return fail(error, "conflicting OPC UA datatype provider: '" +
                           provider.name + "'");
  }

  if (const auto validation = validateAndNormalize(&provider, state)) {
    return fail(error, *validation);
  }
  state.providers.emplace(provider.name, std::move(provider));
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

bool dataTypeRegistryFrozen() noexcept {
  RegistryState &state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.frozen;
}

std::optional<std::vector<std::string>>
freezeDataTypeRegistry(std::string *error) {
  RegistryState &state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.frozen) {
    if (error != nullptr) {
      error->clear();
    }
    return state.provider_order;
  }

  auto order = computeProviderOrder(state, error);
  if (!order) {
    return std::nullopt;
  }
  state.provider_order = *order;
  state.frozen = true;
  if (error != nullptr) {
    error->clear();
  }
  return order;
}

namespace detail {

std::optional<std::vector<DataTypeProvider>>
frozenDataTypeProviders(std::string *error) {
  if (!freezeDataTypeRegistry(error)) {
    return std::nullopt;
  }

  RegistryState &state = registry();
  std::lock_guard<std::mutex> lock(state.mutex);
  std::vector<DataTypeProvider> providers;
  providers.reserve(state.provider_order.size());
  for (const std::string &name : state.provider_order) {
    providers.push_back(state.providers.at(name));
  }
  return providers;
}

} // namespace detail
} // namespace RTT::opcua
