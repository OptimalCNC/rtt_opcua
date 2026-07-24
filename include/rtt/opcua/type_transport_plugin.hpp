#pragma once

#include <rtt/types/TransportPlugin.hpp>

#include <string>

namespace RTT::opcua {

class TypeTransportPlugin final : public RTT::types::TransportPlugin {
public:
  bool registerTransport(std::string type_name,
                         RTT::types::TypeInfo *type_info) override;
  std::string getTransportName() const override;
  std::string getTypekitName() const override;
  std::string getName() const override;
};

} // namespace RTT::opcua
