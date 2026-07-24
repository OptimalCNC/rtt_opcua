#pragma once

#include <rtt/TaskContext.hpp>

#include <chrono>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace RTT::opcua {

enum class ProxyConnectionState {
  disconnected,
  connecting,
  connected,
  stale,
};

std::string_view toString(ProxyConnectionState state) noexcept;
std::ostream &operator<<(std::ostream &stream, ProxyConnectionState state);

struct TaskContextProxyOptions {
  std::chrono::milliseconds request_timeout{std::chrono::seconds(2)};
  // Output connection transitions are observed at this cadence. RTT clients
  // that require a per-connection initial value should use ConnPolicy::init.
  std::chrono::milliseconds port_poll_interval{std::chrono::milliseconds(10)};
};

class TaskContextProxy final : public RTT::TaskContext {
public:
  static std::unique_ptr<TaskContextProxy>
  create(std::string endpoint_url, std::string component_name,
         TaskContextProxyOptions options = {}, std::string *error = nullptr);

  ~TaskContextProxy() override;

  TaskContextProxy(const TaskContextProxy &) = delete;
  TaskContextProxy &operator=(const TaskContextProxy &) = delete;

  // Rebuilds the mirrored RTT interface. Previously returned interface pointers
  // and existing port connections are invalidated when this succeeds.
  bool synchronize(std::string *error = nullptr);
  bool ready() override;

  bool configure() override;
  bool activate() override;
  bool start() override;
  bool stop() override;
  bool cleanup() override;
  bool recover() override;
  bool isConfigured() const override;
  bool isActive() const override;
  bool isRunning() const override;
  bool inFatalError() const override;
  bool inException() const override;
  bool inRunTimeError() const override;
  TaskState getTaskState() const override;
  TaskState getTargetState() const override;
  Seconds getPeriod() const override;
  bool setPeriod(Seconds period) override;
  unsigned getCpuAffinity() const override;
  bool setCpuAffinity(unsigned cpu) override;
  bool update() override;
  bool trigger() override;
  void error() override;

  ProxyConnectionState connectionState() const noexcept;
  std::string endpointUrl() const;
  std::string lastError() const;

private:
  class Impl;

  TaskContextProxy(std::string endpoint_url, std::string component_name,
                   TaskContextProxyOptions options);

  std::unique_ptr<Impl> impl_;
};

} // namespace RTT::opcua
