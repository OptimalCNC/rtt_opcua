#include <rtt/opcua/server_options.hpp>

#include <algorithm>
#include <set>

namespace RTT::opcua {

bool isLoopbackAddress(std::string_view address) {
  return address == "127.0.0.1" || address == "::1";
}

std::optional<std::string> validateServerOptions(const ServerOptions &options) {
  if (options.bind_address.empty()) {
    return "bind address must not be empty";
  }
  if (options.port == 0U) {
    return "port must be between 1 and 65535";
  }
  if (options.endpoint_path.empty() || options.endpoint_path.front() != '/') {
    return "endpoint path must start with '/'";
  }
  if (options.endpoint_path.find_first_of("?# \t\r\n") != std::string::npos) {
    return "endpoint path contains an unsupported character";
  }
  if (options.application_name.empty()) {
    return "application name must not be empty";
  }
  if (!isLoopbackAddress(options.bind_address) &&
      options.bind_address != "0.0.0.0") {
    return "non-loopback OPC UA listening is not supported";
  }
  std::set<std::string, std::less<>> namespace_uris;
  for (const std::string &uri : options.additional_namespace_uris) {
    if (uri.empty() || uri.find('\0') != std::string::npos) {
      return "additional namespace URI must not be empty or contain NUL";
    }
    if (!namespace_uris.insert(uri).second) {
      return "additional namespace URIs must be unique";
    }
  }
  return std::nullopt;
}

std::string endpointUrl(const ServerOptions &options) {
  const bool is_ipv6_literal =
      options.bind_address.find(':') != std::string::npos;
  const std::string host =
      is_ipv6_literal ? '[' + options.bind_address + ']' : options.bind_address;
  return "opc.tcp://" + host + ':' + std::to_string(options.port) +
         options.endpoint_path;
}

} // namespace RTT::opcua
