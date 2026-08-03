#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RTT::opcua {

struct ServerOptions {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{4840};
  std::string endpoint_path{"/rtt"};
  std::string application_name{"Orocos RTT OPC UA"};
  std::vector<std::string> additional_namespace_uris;
};

bool isLoopbackAddress(std::string_view address);
std::optional<std::string> validateServerOptions(const ServerOptions &options);
std::string endpointUrl(const ServerOptions &options);

} // namespace RTT::opcua
