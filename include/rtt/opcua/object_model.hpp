#pragma once

#include <rtt/opcua/server.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace RTT {
class TaskContext;
}

namespace RTT::opcua {

namespace detail {
class ObjectModelImpl;
} // namespace detail

struct UnsupportedResource {
  std::string component;
  std::string path;
  std::string kind;
  std::string type_name;
  std::string reason;

  std::string message() const;
  auto operator<=>(const UnsupportedResource &) const = default;
};

enum class PublicationDiagnosticKind {
  malformed_selector,
  unmatched_selector,
  inventory_failure,
  unsupported_resource,
  mandatory_resource,
  publication_conflict,
};

struct PublicationDiagnostic {
  PublicationDiagnosticKind kind;
  std::string component;
  std::string selector;
  std::string resource_path;
  std::string reason;

  std::string message() const;
  auto operator<=>(const PublicationDiagnostic &) const = default;
};

struct ObjectModelOptions {
  std::chrono::milliseconds operation_timeout{std::chrono::seconds(5)};
  std::function<void(const std::string &)> warning_sink;
};

class ObjectModel final {
public:
  explicit ObjectModel(Server &server, ObjectModelOptions options = {});
  ~ObjectModel();

  ObjectModel(const ObjectModel &) = delete;
  ObjectModel &operator=(const ObjectModel &) = delete;
  ObjectModel(ObjectModel &&) = delete;
  ObjectModel &operator=(ObjectModel &&) = delete;

  // Close callback/publication admission without waiting for existing leases.
  // Destruction still drains admitted operations before releasing components.
  void beginShutdown() noexcept;

  bool publishComponent(
      RTT::TaskContext &component, std::string *error = nullptr,
      std::vector<UnsupportedResource> *unsupported = nullptr);
  bool publishComponentSelected(
      RTT::TaskContext &component, const std::vector<std::string> &selectors,
      std::string *error = nullptr,
      std::vector<PublicationDiagnostic> *diagnostics = nullptr);

  std::uint64_t revision() const noexcept;
  std::size_t componentCount() const noexcept;
  std::size_t pendingOperationCount() const noexcept;
  std::vector<UnsupportedResource>
  unsupportedResources(std::string_view component) const;
  std::vector<PublicationDiagnostic>
  publicationDiagnostics(std::string_view component) const;
  std::string lastError() const;

private:
  std::shared_ptr<detail::ObjectModelImpl> impl_;
};

} // namespace RTT::opcua
