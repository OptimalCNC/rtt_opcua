#pragma once

#include <open62541pp/types.hpp>

#include <string_view>
#include <vector>

namespace RTT::opcua {

struct TypeDescriptor {
  std::string_view rtt_name;
  ::opcua::NodeId data_type;
  bool has_value;
};

const std::vector<TypeDescriptor>& canonicalTypeDescriptors();
const TypeDescriptor* descriptorForType(std::string_view rtt_name);

}  // namespace RTT::opcua
