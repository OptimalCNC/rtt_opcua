#pragma once

#include "client_session.hpp"

#include <memory>

namespace RTT {
class OperationInterfacePart;
}

namespace RTT::opcua::detail {

RTT::OperationInterfacePart *
makeRemoteOperation(std::shared_ptr<ClientSession> session,
                    RemoteOperationDescription description);

} // namespace RTT::opcua::detail
