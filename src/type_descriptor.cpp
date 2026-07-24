#include <rtt/opcua/type_descriptor.hpp>

#include <open62541pp/ua/nodeids.hpp>

#include <algorithm>
#include <type_traits>

namespace RTT::opcua {

const std::vector<TypeDescriptor>& canonicalTypeDescriptors() {
  static const std::vector<TypeDescriptor> descriptors {
      {"Bool", ::opcua::DataTypeId::Boolean, true},
      {"Int8", ::opcua::DataTypeId::SByte, true},
      {"UInt8", ::opcua::DataTypeId::Byte, true},
      {"Int16", ::opcua::DataTypeId::Int16, true},
      {"UInt16", ::opcua::DataTypeId::UInt16, true},
      {"Int32", ::opcua::DataTypeId::Int32, true},
      {"UInt32", ::opcua::DataTypeId::UInt32, true},
      {"Int64", ::opcua::DataTypeId::Int64, true},
      {"UInt64", ::opcua::DataTypeId::UInt64, true},
      {"Float32", ::opcua::DataTypeId::Float, true},
      {"Float64", ::opcua::DataTypeId::Double, true},
      {"Char",
       std::is_signed_v<char> ? ::opcua::NodeId(::opcua::DataTypeId::SByte)
                              : ::opcua::NodeId(::opcua::DataTypeId::Byte),
       true},
      {"String", ::opcua::DataTypeId::String, true},
      {"Void", ::opcua::DataTypeId::BaseDataType, false},
  };
  return descriptors;
}

const TypeDescriptor* descriptorForType(std::string_view rtt_name) {
  const auto& descriptors = canonicalTypeDescriptors();
  const auto found = std::find_if(
      descriptors.begin(), descriptors.end(),
      [rtt_name](const TypeDescriptor& descriptor) { return descriptor.rtt_name == rtt_name; });
  return found == descriptors.end() ? nullptr : &*found;
}

}  // namespace RTT::opcua
