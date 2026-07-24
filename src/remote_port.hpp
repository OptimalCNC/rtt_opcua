#pragma once

#include "client_session.hpp"

#include <rtt/base/DataSourceBase.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace RTT {
namespace base {
class PortInterface;
}
namespace types {
class TypeInfo;
}
} // namespace RTT

namespace RTT::opcua {
class TypeProtocol;
}

namespace RTT::opcua::detail {

class RemotePortAdapter final {
public:
  static std::shared_ptr<RemotePortAdapter>
  create(std::shared_ptr<ClientSession> session,
         RemotePortDescription description, std::string *error = nullptr);

  ~RemotePortAdapter() noexcept;

  RemotePortAdapter(const RemotePortAdapter &) = delete;
  RemotePortAdapter &operator=(const RemotePortAdapter &) = delete;

  RTT::base::PortInterface &port() const;
  void pump() noexcept;
  bool hasPendingState() const noexcept;
  bool canTransferStateTo(const RemotePortAdapter &replacement) const noexcept;
  bool transferPendingStateTo(RemotePortAdapter &replacement);
  const std::string &name() const noexcept;
  std::string lastError() const;

private:
  RemotePortAdapter(std::shared_ptr<ClientSession> session,
                    RemotePortDescription description,
                    const RTT::types::TypeInfo *type_info,
                    const TypeProtocol *protocol,
                    std::unique_ptr<RTT::base::PortInterface> port);

  void pumpInput();
  void pumpOutput();
  void setError(std::string error);
  void clearError();
  bool hasSameIdentity(const RemotePortAdapter &other) const noexcept;

  std::shared_ptr<ClientSession> session_;
  RemotePortDescription description_;
  const RTT::types::TypeInfo *type_info_;
  const TypeProtocol *protocol_;
  std::unique_ptr<RTT::base::PortInterface> port_;
  RTT::base::DataSourceBase::shared_ptr pending_input_source_;
  std::optional<::opcua::Variant> pending_input_;
  RTT::base::DataSourceBase::shared_ptr pending_output_;
  bool output_was_connected_{false};
  mutable std::mutex error_mutex_;
  std::string last_error_;
};

} // namespace RTT::opcua::detail
