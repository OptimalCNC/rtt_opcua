#pragma once

#include <cstdint>

namespace RTT::opcua {

enum class PortDirection : std::int32_t {
  input = 0,
  output = 1,
};

} // namespace RTT::opcua
