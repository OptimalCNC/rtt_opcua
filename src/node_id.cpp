#include <rtt/opcua/node_id.hpp>

namespace RTT::opcua {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

bool isUnreserved(unsigned char value) {
  return (value >= static_cast<unsigned char>('a') &&
          value <= static_cast<unsigned char>('z')) ||
         (value >= static_cast<unsigned char>('A') &&
          value <= static_cast<unsigned char>('Z')) ||
         (value >= static_cast<unsigned char>('0') &&
          value <= static_cast<unsigned char>('9')) ||
         value == static_cast<unsigned char>('-') ||
         value == static_cast<unsigned char>('.') ||
         value == static_cast<unsigned char>('_') ||
         value == static_cast<unsigned char>('~');
}

}  // namespace

std::string escapeNodeIdSegment(std::string_view segment) {
  if (segment.empty()) {
    return "%00";
  }

  std::string escaped;
  escaped.reserve(segment.size());
  for (const char character : segment) {
    const auto value = static_cast<unsigned char>(character);
    if (isUnreserved(value)) {
      escaped.push_back(character);
      continue;
    }
    escaped.push_back('%');
    escaped.push_back(kHexDigits[value >> 4U]);
    escaped.push_back(kHexDigits[value & 0x0FU]);
  }
  return escaped;
}

std::string makeNodePath(std::span<const std::string_view> segments) {
  std::string path {"rtt"};
  for (const std::string_view segment : segments) {
    path += '/';
    path += escapeNodeIdSegment(segment);
  }
  return path;
}

}  // namespace RTT::opcua
