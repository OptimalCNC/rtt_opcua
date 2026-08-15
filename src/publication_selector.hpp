#pragma once

#include <compare>
#include <set>
#include <string>
#include <vector>

namespace RTT::opcua::detail {

enum class SelectorIssueKind { malformed, unmatched };

struct SelectorIssue {
  SelectorIssueKind kind;
  std::string selector;
  std::string reason;
  auto operator<=>(const SelectorIssue&) const = default;
};

struct SelectorMatch {
  std::set<std::string, std::less<>> resource_paths;
  std::vector<SelectorIssue> issues;
};

SelectorMatch matchPublicationSelectors(
    const std::vector<std::string>& selectors,
    const std::vector<std::string>& resource_paths);

}  // namespace RTT::opcua::detail
