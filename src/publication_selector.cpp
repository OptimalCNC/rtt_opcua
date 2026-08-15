#include "publication_selector.hpp"

#include <rtt/opcua/node_id.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace RTT::opcua::detail {
namespace {

struct ParsedSelector {
  std::vector<std::string> segments;
  bool recursive {false};
};

struct ParsedResource {
  std::string_view path;
  std::vector<std::string> segments;
};

bool isUpperHexDigit(char character) {
  return (character >= '0' && character <= '9') ||
         (character >= 'A' && character <= 'F');
}

unsigned char hexValue(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<unsigned char>(character - '0');
  }
  return static_cast<unsigned char>(character - 'A' + 10);
}

std::optional<std::string> decodeCanonicalSegment(std::string_view segment) {
  std::string decoded;
  decoded.reserve(segment.size());
  for (std::size_t index = 0U; index < segment.size(); ++index) {
    if (segment[index] != '%') {
      decoded.push_back(segment[index]);
      continue;
    }
    if (index + 2U >= segment.size() || !isUpperHexDigit(segment[index + 1U]) ||
        !isUpperHexDigit(segment[index + 2U])) {
      return std::nullopt;
    }
    const auto value = static_cast<unsigned char>(
        (hexValue(segment[index + 1U]) << 4U) | hexValue(segment[index + 2U]));
    decoded.push_back(static_cast<char>(value));
    index += 2U;
  }
  if (escapeNodeIdSegment(decoded) != segment) {
    return std::nullopt;
  }
  return decoded;
}

std::vector<std::string_view> splitPath(std::string_view path) {
  std::vector<std::string_view> segments;
  std::size_t begin = 0U;
  while (true) {
    const std::size_t end = path.find('/', begin);
    segments.emplace_back(path.substr(begin, end - begin));
    if (end == std::string_view::npos) {
      return segments;
    }
    begin = end + 1U;
  }
}

std::optional<ParsedSelector> parseSelector(std::string_view selector,
                                            std::string* reason) {
  if (selector.empty()) {
    *reason = "selector is empty";
    return std::nullopt;
  }

  ParsedSelector parsed;
  const auto raw_segments = splitPath(selector);
  parsed.segments.reserve(raw_segments.size());
  for (std::size_t index = 0U; index < raw_segments.size(); ++index) {
    const std::string_view segment = raw_segments[index];
    if (segment.empty()) {
      *reason = "selector contains an empty segment";
      return std::nullopt;
    }
    if (segment == "**") {
      if (index + 1U != raw_segments.size()) {
        *reason = "recursive wildcard must be terminal";
        return std::nullopt;
      }
      parsed.segments.emplace_back(segment);
      parsed.recursive = true;
      continue;
    }
    if (segment == "*") {
      parsed.segments.emplace_back(segment);
      continue;
    }
    if (segment.find('*') != std::string_view::npos) {
      *reason = "wildcard must occupy an entire segment";
      return std::nullopt;
    }
    const auto decoded = decodeCanonicalSegment(segment);
    if (!decoded.has_value()) {
      *reason = "literal segment is not canonically escaped";
      return std::nullopt;
    }
    parsed.segments.push_back(*decoded);
  }
  return parsed;
}

std::optional<ParsedResource> parseResource(std::string_view path) {
  if (path.empty()) {
    return std::nullopt;
  }

  ParsedResource parsed {path, {}};
  const auto raw_segments = splitPath(path);
  parsed.segments.reserve(raw_segments.size());
  for (const std::string_view segment : raw_segments) {
    if (segment.empty()) {
      return std::nullopt;
    }
    const auto decoded = decodeCanonicalSegment(segment);
    if (!decoded.has_value()) {
      return std::nullopt;
    }
    parsed.segments.push_back(*decoded);
  }
  return parsed;
}

bool matches(const ParsedSelector& selector,
             const std::vector<std::string>& resource) {
  const std::size_t fixed = selector.recursive
                                ? selector.segments.size() - 1U
                                : selector.segments.size();
  if ((!selector.recursive && resource.size() != fixed) ||
      (selector.recursive && resource.size() < fixed)) {
    return false;
  }
  for (std::size_t index = 0U; index < fixed; ++index) {
    if (selector.segments[index] != "*" &&
        selector.segments[index] != resource[index]) {
      return false;
    }
  }
  return true;
}

void addIssue(std::vector<SelectorIssue>* issues, SelectorIssueKind kind,
              std::string_view selector, std::string reason) {
  issues->push_back({kind, std::string(selector), std::move(reason)});
}

}  // namespace

SelectorMatch matchPublicationSelectors(
    const std::vector<std::string>& selectors,
    const std::vector<std::string>& resource_paths) {
  std::vector<ParsedResource> resources;
  resources.reserve(resource_paths.size());
  for (const std::string& resource_path : resource_paths) {
    const auto parsed = parseResource(resource_path);
    if (parsed.has_value()) {
      resources.push_back(*parsed);
    }
  }

  SelectorMatch result;
  for (const std::string& selector : selectors) {
    std::string reason;
    const auto parsed = parseSelector(selector, &reason);
    if (!parsed.has_value()) {
      addIssue(&result.issues, SelectorIssueKind::malformed, selector,
               std::move(reason));
      continue;
    }

    bool matched = false;
    for (const ParsedResource& resource : resources) {
      if (!matches(*parsed, resource.segments)) {
        continue;
      }
      result.resource_paths.emplace(resource.path);
      matched = true;
    }
    if (!matched) {
      addIssue(&result.issues, SelectorIssueKind::unmatched, selector,
               "selector did not match a logical resource");
    }
  }

  std::sort(result.issues.begin(), result.issues.end());
  result.issues.erase(
      std::unique(result.issues.begin(), result.issues.end()),
      result.issues.end());
  return result;
}

}  // namespace RTT::opcua::detail
