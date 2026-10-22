#include <cedar/Cedar.hpp>

#include "model/FuzzyMatch.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace
{

using cedar::detail::FuzzySearch;
using cedar::detail::LevenshteinDistance;

// Rows are the tests in cedar-policy-core/src/fuzzy_match.rs.
TEST(FuzzyMatch, RustRows)
{
    const std::vector<std::string> principals{"principal", "Principal", "principality", "prince", "principle"};
    EXPECT_EQ(FuzzySearch("user::Alice", std::vector<std::string>{"User::Alice", "user::alice", "user", "alice"}),
              "User::Alice");
    EXPECT_EQ(FuzzySearch("princpal", principals), "principal");
    EXPECT_EQ(FuzzySearch("prncpal", principals), "principal");
    EXPECT_EQ(FuzzySearch("user::\"Alice\"",
                          std::vector<std::string>{"User::\"Alice\"", "user::\"alice\"", "user", "alice"}),
              "User::\"Alice\"");
    EXPECT_EQ(FuzzySearch("User::\"Alice\"",
                          std::vector<std::string>{"User::'Alice'", "user::\"alice\"", "user", "alice"}),
              "User::'Alice'");
    EXPECT_EQ(FuzzySearch("prncpal", std::vector<std::string>{"principal", "Principal", "principality", "principal",
                                                              "prince", "principle", "principal"}),
              "principal");
}

TEST(FuzzyMatch, EmptyKeyOrEmptyListSuggestsNothing)
{
    EXPECT_EQ(FuzzySearch("", std::vector<std::string>{"user", "alice"}), std::nullopt);
    EXPECT_EQ(FuzzySearch("user::Alice", std::vector<std::string>{}), std::nullopt);
}

TEST(FuzzyMatch, FirstOfEquallyNearWordsWins)
{
    EXPECT_EQ(FuzzySearch("cat", std::vector<std::string>{"bat", "hat", "cot"}), "bat");
    EXPECT_EQ(FuzzySearch("cat", std::vector<std::string>{"hat", "bat", "cot"}), "hat");
}

TEST(FuzzyMatch, UnlimitedSearchSuggestsAnyNearest)
{
    EXPECT_EQ(FuzzySearch("abcxyz", std::vector<std::string>{"qqqqqq", "abcdef"}), "abcdef");
}

TEST(FuzzyMatch, LimitIsInclusive)
{
    const std::vector<std::string> words{"abcdef"};
    EXPECT_EQ(LevenshteinDistance("abc", "abcdef"), 3U);
    EXPECT_EQ(FuzzySearch("abc", words, 3), "abcdef");
    EXPECT_EQ(FuzzySearch("abc", words, 2), std::nullopt);
}

TEST(FuzzyMatch, DistanceCountsScalarValues)
{
    EXPECT_EQ(LevenshteinDistance("caf\xC3\xA9", "cafe"), 1U);
    EXPECT_EQ(LevenshteinDistance("kitten", "sitting"), 3U);
    EXPECT_EQ(LevenshteinDistance("", "abc"), 3U);
    EXPECT_EQ(LevenshteinDistance("abc", ""), 3U);
}

// ---- The suggestions reach the diagnostics' help text ----

constexpr const char* TinyTodoSchema = R"(
entity List in [Application] = { "name": String, "owner": User };
entity Application;
entity User in [Application] = { "joblevel": Long, "location": String };
action GetList, GetLists appliesTo { principal: [User], resource: [List] };
)";

auto HelpOf(const cedar::Diagnostic& diagnostic) -> std::string { return diagnostic.help.value_or("<none>"); }

TEST(FuzzyMatchHelp, UnknownMethodAndFunction)
{
    const auto method = cedar::ParseExpression("ip(\"1.2.3.4\").isInRnge(ip(\"1.0.0.0/8\"))");
    ASSERT_FALSE(method.has_value());
    EXPECT_EQ(HelpOf(method.error().Diagnostics().front()), "did you mean `isInRange`?");

    const auto function = cedar::ParseExpression("ipp(\"1.2.3.4\")");
    ASSERT_FALSE(function.has_value());
    EXPECT_EQ(HelpOf(function.error().Diagnostics().front()), "did you mean `ip`?");

    const auto far = cedar::ParseExpression("completelyUnrelatedName(\"1.2.3.4\")");
    ASSERT_FALSE(far.has_value());
    EXPECT_EQ(HelpOf(far.error().Diagnostics().front()), "<none>");
}

TEST(FuzzyMatchHelp, Validation)
{
    auto schema = cedar::ParseSchema(TinyTodoSchema);
    ASSERT_TRUE(schema.has_value());
    const cedar::Validator validator{schema->value};
    const auto firstError = [&](const char* policy) {
        auto policies = cedar::ParsePolicySet(policy);
        EXPECT_TRUE(policies.has_value());
        const cedar::ValidationResult result = validator.Validate(*policies);
        EXPECT_FALSE(result.errors.empty()) << policy;
        return result.errors.empty() ? std::string{"<no error>"} : HelpOf(result.errors.front().diagnostic);
    };
    EXPECT_EQ(firstError("permit(principal is User, action, resource) when { principal.joblvl > 1 };"),
              "did you mean `joblevel`?");
    EXPECT_EQ(firstError("permit(principal is Usr, action, resource);"), "did you mean `User`?");
    // The key is the bare id, compared with each action's written `Action::\"id\"`; the first of the
    // equally near candidates is suggested.
    EXPECT_EQ(firstError("permit(principal, action == Action::\"GetLsts\", resource);"),
              "did you mean `Action::\"GetList\"`?");
}

TEST(FuzzyMatchHelp, UnknownExtensionTypeInSchema)
{
    auto schema = cedar::SchemaFromJson(
        R"({"": {"entityTypes": {"User": {"shape": {"type": "Record", "attributes": )"
        R"({"ip": {"type": "Extension", "name": "ipadr"}}}}}, "actions": {}}})");
    ASSERT_FALSE(schema.has_value());
    EXPECT_EQ(HelpOf(schema.error().Diagnostics().front()), "did you mean `ipaddr`?");
}

} // namespace
