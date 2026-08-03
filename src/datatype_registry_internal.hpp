#pragma once

#include <rtt/opcua/datatype_registry.hpp>

#include <optional>
#include <string>
#include <vector>

namespace RTT::opcua::detail {

std::optional<std::vector<DataTypeProvider>>
frozenDataTypeProviders(std::string *error = nullptr);

} // namespace RTT::opcua::detail
