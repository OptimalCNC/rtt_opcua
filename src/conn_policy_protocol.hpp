#pragma once

#include <string>

namespace RTT::types {
class TypeInfo;
}

namespace RTT::opcua {

bool registerConnPolicyProtocol(RTT::types::TypeInfo *type_info,
                                std::string *error = nullptr);

} // namespace RTT::opcua
