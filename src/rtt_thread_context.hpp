#pragma once

#include <rtt/rtt-config.h>

#ifdef OROPKG_OS_XENOMAI
#include <alchemy/task.h>

#include <cerrno>
#include <system_error>
#endif

namespace RTT::opcua::detail {

inline void initializeRttThreadContext() {
#ifdef OROPKG_OS_XENOMAI
  if (rt_task_self() != nullptr) {
    return;
  }
  const int result = rt_task_shadow(nullptr, nullptr, 0, 0);
  if (result != 0 && result != -EBUSY) {
    throw std::system_error(-result, std::generic_category(),
                            "failed to initialize the OPC UA RTT thread context");
  }
#endif
}

} // namespace RTT::opcua::detail
