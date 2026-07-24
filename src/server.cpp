#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/server.hpp>

#include <open62541/types_generated.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace RTT::opcua {
namespace {

constexpr auto kStartupTimeout = std::chrono::seconds(5);
constexpr auto kMaximumIterateSleep = std::chrono::milliseconds(1);

std::string exceptionMessage(std::exception_ptr exception) {
  if (exception == nullptr) {
    return {};
  }
  try {
    std::rethrow_exception(exception);
  } catch (const std::exception &caught) {
    return caught.what();
  } catch (...) {
    return "unknown server task exception";
  }
}

void setServerUrl(::opcua::ServerConfig &config, const std::string &url) {
  UA_Array_delete(config->serverUrls, config->serverUrlsSize,
                  &UA_TYPES[UA_TYPES_STRING]);
  config->serverUrls = nullptr;
  config->serverUrlsSize = 0U;

  auto *urls =
      static_cast<UA_String *>(UA_Array_new(1U, &UA_TYPES[UA_TYPES_STRING]));
  if (urls == nullptr) {
    throw std::bad_alloc();
  }
  urls[0] = UA_String_fromChars(url.c_str());
  if (urls[0].data == nullptr && !url.empty()) {
    UA_Array_delete(urls, 1U, &UA_TYPES[UA_TYPES_STRING]);
    throw std::bad_alloc();
  }
  config->serverUrls = urls;
  config->serverUrlsSize = 1U;
}

} // namespace

class Server::Impl {
public:
  struct Completion {
    std::mutex mutex;
    std::condition_variable condition;
    bool done{false};
    std::string error;
  };

  struct QueuedTask {
    Task callback;
    std::shared_ptr<Completion> completion;
    std::atomic_bool cancelled{false};
  };

  explicit Impl(ServerOptions configured_options)
      : options(std::move(configured_options)) {}

  ~Impl() { stop(); }

  bool start(std::string *output_error) {
    if (const auto validation_error = validateServerOptions(options)) {
      setFailure(*validation_error);
      copyError(output_error);
      return false;
    }

    std::unique_lock<std::mutex> lock(lifecycle_mutex);
    if (current_state.load() == ServerState::running) {
      return true;
    }
    if (server_thread.joinable()) {
      std::thread stale_thread = std::move(server_thread);
      lock.unlock();
      stale_thread.join();
      lock.lock();
    }

    stop_requested.store(false);
    namespace_index.store(0U);
    current_error.clear();
    current_state.store(ServerState::starting);
    server_thread = std::thread(&Impl::run, this);

    const bool started =
        lifecycle_condition.wait_for(lock, kStartupTimeout, [this] {
          return current_state.load() != ServerState::starting;
        });
    if (!started) {
      current_error = "OPC UA server startup timed out";
      current_state.store(ServerState::failed);
      stop_requested.store(true);
    }

    const bool running = current_state.load() == ServerState::running;
    std::thread failed_thread;
    if (!running && server_thread.joinable()) {
      failed_thread = std::move(server_thread);
    }
    lock.unlock();
    if (failed_thread.joinable()) {
      failed_thread.join();
    }
    copyError(output_error);
    return running;
  }

  void stop() noexcept {
    std::thread thread_to_join;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      const ServerState before = current_state.load();
      if (before == ServerState::stopped && !server_thread.joinable()) {
        return;
      }

      stop_requested.store(true);
      if (before == ServerState::running || before == ServerState::starting) {
        current_state.store(ServerState::stopping);
      }

      if (std::this_thread::get_id() == server_thread_id) {
        return;
      }
      if (server_thread.joinable()) {
        thread_to_join = std::move(server_thread);
      }
    }
    if (thread_to_join.joinable()) {
      thread_to_join.join();
    }

    failPendingTasks("OPC UA server stopped");
    namespace_index.store(0U);
    current_state.store(ServerState::stopped);
  }

  bool enqueue(Task task, const std::shared_ptr<Completion> &completion) {
    if (!task || current_state.load() != ServerState::running) {
      return false;
    }
    auto queued = std::make_shared<QueuedTask>();
    queued->callback = std::move(task);
    queued->completion = completion;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex);
      if (current_state.load() != ServerState::running) {
        return false;
      }
      tasks.push_back(std::move(queued));
    }
    return true;
  }

  bool invoke(Task task, std::chrono::milliseconds timeout,
              std::string *output_error) {
    if (!task) {
      assignError(output_error, "server task must not be empty");
      return false;
    }
    if (timeout <= std::chrono::milliseconds::zero()) {
      assignError(output_error, "server task timeout must be positive");
      return false;
    }

    if (isServerThread() && native_server != nullptr) {
      try {
        task(*native_server);
        assignError(output_error, {});
        return true;
      } catch (...) {
        assignError(output_error, exceptionMessage(std::current_exception()));
        return false;
      }
    }

    auto completion = std::make_shared<Completion>();
    auto queued = std::make_shared<QueuedTask>();
    queued->callback = std::move(task);
    queued->completion = completion;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex);
      if (current_state.load() != ServerState::running) {
        assignError(output_error, "OPC UA server is not running");
        return false;
      }
      tasks.push_back(queued);
    }

    std::unique_lock<std::mutex> completion_lock(completion->mutex);
    if (!completion->condition.wait_for(
            completion_lock, timeout,
            [&completion] { return completion->done; })) {
      queued->cancelled.store(true);
      assignError(output_error, "OPC UA server task timed out");
      return false;
    }
    assignError(output_error, completion->error);
    return completion->error.empty();
  }

  ServerOptions options;
  std::atomic<ServerState> current_state{ServerState::stopped};
  std::atomic<std::uint16_t> namespace_index{0U};

  mutable std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_condition;
  std::string current_error;
  std::thread server_thread;
  std::thread::id server_thread_id;
  std::atomic_bool stop_requested{false};
  std::unique_ptr<::opcua::Server> native_server;

  std::mutex tasks_mutex;
  std::deque<std::shared_ptr<QueuedTask>> tasks;

private:
  void run() noexcept {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      server_thread_id = std::this_thread::get_id();
    }
    try {
      ::opcua::ServerConfig config(options.port);
      config.setLogger(
          [](::opcua::LogLevel, ::opcua::LogCategory, std::string_view) {});
      config.setApplicationName(options.application_name);
      config.setApplicationUri("urn:orocos:rtt:server");
      config.setProductUri("urn:orocos:rtt");
      setServerUrl(config, RTT::opcua::endpointUrl(options));

      native_server = std::make_unique<::opcua::Server>(std::move(config));
      const ::opcua::NamespaceIndex registered =
          native_server->registerNamespace(kNamespaceUri);
      namespace_index.store(registered);
      native_server->runIterate();

      {
        std::lock_guard<std::mutex> lock(lifecycle_mutex);
        current_state.store(ServerState::running);
      }
      lifecycle_condition.notify_all();

      while (!stop_requested.load()) {
        drainTasks();
        const std::uint16_t suggested_wait = native_server->runIterate();
        const auto sleep = std::min(std::chrono::milliseconds(suggested_wait),
                                    kMaximumIterateSleep);
        if (sleep > std::chrono::milliseconds::zero()) {
          std::this_thread::sleep_for(sleep);
        }
      }
      drainTasks();
      native_server->stop();
      native_server.reset();
      failPendingTasks("OPC UA server stopped");
      namespace_index.store(0U);
      current_state.store(ServerState::stopped);
    } catch (...) {
      native_server.reset();
      failPendingTasks("OPC UA server failed");
      namespace_index.store(0U);
      setFailure(exceptionMessage(std::current_exception()));
    }
    lifecycle_condition.notify_all();
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      server_thread_id = {};
    }
  }

  void drainTasks() {
    std::deque<std::shared_ptr<QueuedTask>> pending;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex);
      pending.swap(tasks);
    }

    for (const auto &queued : pending) {
      std::string task_error;
      if (queued->cancelled.load()) {
        task_error = "OPC UA server task cancelled";
      } else {
        try {
          queued->callback(*native_server);
        } catch (...) {
          task_error = exceptionMessage(std::current_exception());
        }
      }
      complete(queued->completion, std::move(task_error));
    }
  }

  void failPendingTasks(const std::string &error) {
    std::deque<std::shared_ptr<QueuedTask>> pending;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex);
      pending.swap(tasks);
    }
    for (const auto &queued : pending) {
      complete(queued->completion, error);
    }
  }

  static void complete(const std::shared_ptr<Completion> &completion,
                       std::string error) {
    if (!completion) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(completion->mutex);
      completion->error = std::move(error);
      completion->done = true;
    }
    completion->condition.notify_all();
  }

  void setFailure(std::string error) {
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex);
      current_error = error.empty() ? "OPC UA server failed" : std::move(error);
      current_state.store(ServerState::failed);
    }
    lifecycle_condition.notify_all();
  }

  void copyError(std::string *output_error) const {
    if (output_error == nullptr) {
      return;
    }
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    *output_error = current_error;
  }

  static void assignError(std::string *output_error, std::string error) {
    if (output_error != nullptr) {
      *output_error = std::move(error);
    }
  }

  bool isServerThread() const {
    std::lock_guard<std::mutex> lock(lifecycle_mutex);
    return std::this_thread::get_id() == server_thread_id;
  }
};

Server::Server(ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Server::~Server() = default;

bool Server::start(std::string *error) { return impl_->start(error); }

void Server::stop() noexcept { impl_->stop(); }

ServerState Server::state() const noexcept {
  return impl_->current_state.load();
}

bool Server::isRunning() const noexcept {
  return state() == ServerState::running;
}

const ServerOptions &Server::options() const noexcept { return impl_->options; }

std::string Server::endpointUrl() const {
  return RTT::opcua::endpointUrl(impl_->options);
}

std::optional<std::uint16_t> Server::namespaceIndex() const noexcept {
  const std::uint16_t index = impl_->namespace_index.load();
  return index == 0U ? std::nullopt : std::optional<std::uint16_t>(index);
}

std::string Server::lastError() const {
  std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
  return impl_->current_error;
}

bool Server::post(Task task) {
  return impl_->enqueue(std::move(task), nullptr);
}

bool Server::invoke(Task task, std::chrono::milliseconds timeout,
                    std::string *error) {
  return impl_->invoke(std::move(task), timeout, error);
}

} // namespace RTT::opcua
