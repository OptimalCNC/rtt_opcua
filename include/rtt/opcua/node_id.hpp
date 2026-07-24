#pragma once

#include <span>
#include <string>
#include <string_view>

namespace RTT::opcua {

inline constexpr std::string_view kNamespaceUri = "urn:orocos:rtt";

std::string escapeNodeIdSegment(std::string_view segment);
std::string makeNodePath(std::span<const std::string_view> segments);

}  // namespace RTT::opcua
