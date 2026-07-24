#include <rtt/opcua/type_transport_plugin.hpp>

#include <rtt/opcua/type_protocol.hpp>

#include <rtt/types/TypekitPlugin.hpp>

namespace RTT::opcua {

bool TypeTransportPlugin::registerTransport(std::string type_name,
                                            RTT::types::TypeInfo *type_info) {
  return registerCanonicalTypeProtocol(type_name, type_info);
}

std::string TypeTransportPlugin::getTransportName() const { return "OPCUA"; }

std::string TypeTransportPlugin::getTypekitName() const { return "rtt-types"; }

std::string TypeTransportPlugin::getName() const { return "OPCUA://rtt-types"; }

} // namespace RTT::opcua

ORO_TYPEKIT_PLUGIN(RTT::opcua::TypeTransportPlugin)
