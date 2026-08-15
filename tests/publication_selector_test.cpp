#define BOOST_TEST_MODULE rtt_opcua_publication_selector
#include <boost/test/included/unit_test.hpp>

#include "publication_selector.hpp"

#include <set>
#include <string>
#include <vector>

namespace {

using RTT::opcua::detail::SelectorIssue;
using RTT::opcua::detail::SelectorIssueKind;
using RTT::opcua::detail::matchPublicationSelectors;

const std::vector<std::string> kInventory {
    "operations/start",
    "ports/command",
    "services/automatic",
    "services/automatic/operations/execute",
    "services/automatic/services/diagnostics",
    "services/automatic/services/diagnostics/operations/reset",
    "services/auto%2Fmanual/operations/run%2Anow",
};

std::set<std::string, std::less<>> paths(std::initializer_list<const char*> values) {
  std::set<std::string, std::less<>> result;
  for (const char* value : values) {
    result.emplace(value);
  }
  return result;
}

void checkUnmatched(const SelectorIssue& issue, const char* selector) {
  BOOST_TEST(static_cast<int>(issue.kind) ==
             static_cast<int>(SelectorIssueKind::unmatched));
  BOOST_TEST(issue.selector == selector);
  BOOST_TEST(!issue.reason.empty());
}

void checkMalformed(const SelectorIssue& issue, const char* selector) {
  BOOST_TEST(static_cast<int>(issue.kind) ==
             static_cast<int>(SelectorIssueKind::malformed));
  BOOST_TEST(issue.selector == selector);
  BOOST_TEST(!issue.reason.empty());
}

}  // namespace

BOOST_AUTO_TEST_CASE(exact_selector_selects_one_resource) {
  const auto result = matchPublicationSelectors({"ports/command"}, kInventory);

  BOOST_TEST(result.resource_paths == paths({"ports/command"}),
             boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(single_wildcard_selects_only_direct_operations) {
  const auto result = matchPublicationSelectors({"operations/*"}, kInventory);

  BOOST_TEST(result.resource_paths == paths({"operations/start"}),
             boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(recursive_selector_selects_service_and_descendants) {
  const auto result =
      matchPublicationSelectors({"services/automatic/**"}, kInventory);

  BOOST_TEST(
      result.resource_paths ==
          paths({"services/automatic",
                 "services/automatic/operations/execute",
                 "services/automatic/services/diagnostics",
                 "services/automatic/services/diagnostics/operations/reset"}),
      boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(wildcard_recursive_selector_selects_direct_service_subtrees) {
  const auto result = matchPublicationSelectors({"services/*/**"}, kInventory);

  BOOST_TEST(
      result.resource_paths ==
          paths({"services/automatic",
                 "services/automatic/operations/execute",
                 "services/automatic/services/diagnostics",
                 "services/automatic/services/diagnostics/operations/reset",
                 "services/auto%2Fmanual/operations/run%2Anow"}),
      boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(overlapping_selectors_deduplicate_into_an_ordered_set) {
  const auto result = matchPublicationSelectors(
      {"services/automatic/**", "services/automatic", "services/automatic/**"},
      kInventory);

  BOOST_TEST(
      result.resource_paths ==
          paths({"services/automatic",
                 "services/automatic/operations/execute",
                 "services/automatic/services/diagnostics",
                 "services/automatic/services/diagnostics/operations/reset"}),
      boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(selector_order_does_not_change_paths_or_issues) {
  const std::vector<std::string> forward {
      "operations/missing", "services/automatic/**", "operations/missing",
      "services//automatic", "ports/command"};
  const std::vector<std::string> reverse {
      "ports/command", "services//automatic", "operations/missing",
      "services/automatic/**", "operations/missing"};

  const auto first = matchPublicationSelectors(forward, kInventory);
  const auto second = matchPublicationSelectors(reverse, kInventory);

  BOOST_TEST(first.resource_paths == second.resource_paths,
             boost::test_tools::per_element());
  BOOST_CHECK(first.issues == second.issues);
  BOOST_REQUIRE_EQUAL(first.issues.size(), 2U);
  checkMalformed(first.issues[0], "services//automatic");
  checkUnmatched(first.issues[1], "operations/missing");
}

BOOST_AUTO_TEST_CASE(canonical_escapes_match_literal_separators_and_wildcards) {
  const auto result = matchPublicationSelectors(
      {"services/auto%2Fmanual/operations/run%2Anow"}, kInventory);

  BOOST_TEST(result.resource_paths ==
                 paths({"services/auto%2Fmanual/operations/run%2Anow"}),
             boost::test_tools::per_element());
  BOOST_TEST(result.issues.empty());
}

BOOST_AUTO_TEST_CASE(noncanonical_or_invalid_wildcard_selectors_are_malformed) {
  const auto result = matchPublicationSelectors(
      {"services/auto%2fmanual/operations/run%2Anow", "operations/run*now",
       "services//automatic", "/ports/command", "ports/command/",
       "services/**/operations"},
      kInventory);

  BOOST_REQUIRE_EQUAL(result.issues.size(), 6U);
  checkMalformed(result.issues[0], "/ports/command");
  checkMalformed(result.issues[1], "operations/run*now");
  checkMalformed(result.issues[2], "ports/command/");
  checkMalformed(result.issues[3], "services/**/operations");
  checkMalformed(result.issues[4], "services//automatic");
  checkMalformed(result.issues[5], "services/auto%2fmanual/operations/run%2Anow");
  BOOST_TEST(result.resource_paths.empty());
}

BOOST_AUTO_TEST_CASE(unmatched_selector_reports_a_well_formed_issue) {
  const auto result = matchPublicationSelectors({"operations/missing"}, kInventory);

  BOOST_TEST(result.resource_paths.empty());
  BOOST_REQUIRE_EQUAL(result.issues.size(), 1U);
  checkUnmatched(result.issues.front(), "operations/missing");
}

BOOST_AUTO_TEST_CASE(empty_selector_is_malformed) {
  const auto result = matchPublicationSelectors({""}, kInventory);

  BOOST_TEST(result.resource_paths.empty());
  BOOST_REQUIRE_EQUAL(result.issues.size(), 1U);
  checkMalformed(result.issues.front(), "");
}

BOOST_AUTO_TEST_CASE(category_selectors_are_well_formed_but_unmatched) {
  const auto result = matchPublicationSelectors({"operations", "*"}, kInventory);

  BOOST_TEST(result.resource_paths.empty());
  BOOST_REQUIRE_EQUAL(result.issues.size(), 2U);
  checkUnmatched(result.issues[0], "*");
  checkUnmatched(result.issues[1], "operations");
}
