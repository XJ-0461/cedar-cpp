// IsAuthorized against the reference authorizer on the cases in tests/parity/authorization (the
// expectations are made by scripts/generate-authorization-parity-expectations.py): decision,
// determining policy ids and erroring policy ids with their error kinds.

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Json.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Syntax.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

const std::filesystem::path CasesDirectory{CEDAR_AUTHORIZATION_PARITY_DIRECTORY};

auto ReadFile(const std::filesystem::path& path) -> std::string
{
    std::ifstream file{path};
    if (!file) { throw std::runtime_error{"cannot read " + path.string()}; }
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

auto Lines(const std::string& text) -> std::vector<std::string>
{
    std::vector<std::string> lines;
    std::istringstream stream{text};
    for (std::string line; std::getline(stream, line);)
    {
        if (!line.empty()) { lines.push_back(line); }
    }
    return lines;
}

auto Uid(const std::string_view text) -> cedar::EntityUid
{
    auto uid = cedar::ParseEntityUid(text);
    if (!uid) { throw std::runtime_error{"bad entity uid in a case: " + std::string{text}}; }
    return *std::move(uid);
}

auto KindName(const cedar::EvaluationErrorKind kind) -> std::string_view
{
    using enum cedar::EvaluationErrorKind;
    switch (kind)
    {
    case EntityDoesNotExist: return "EntityDoesNotExist";
    case EntityAttributeDoesNotExist: return "EntityAttributeDoesNotExist";
    case RecordAttributeDoesNotExist: return "RecordAttributeDoesNotExist";
    case EntityTagDoesNotExist: return "EntityTagDoesNotExist";
    case ExtensionFunctionLookup: return "ExtensionFunctionLookup";
    case TypeError: return "TypeError";
    case WrongArgumentCount: return "WrongArgumentCount";
    case IntegerOverflow: return "IntegerOverflow";
    case UnlinkedSlot: return "UnlinkedSlot";
    case ExtensionFunctionExecution: return "ExtensionFunctionExecution";
    case RecursionLimit: return "RecursionLimit";
    }
    return "?";
}

auto CaseNames() -> std::vector<std::string>
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{CasesDirectory})
    {
        if (entry.path().extension() == ".cedar") { names.push_back(entry.path().stem().string()); }
    }
    std::ranges::sort(names);
    return names;
}

// The `<Name>.request` file: `key: value` lines for principal, action, resource and context.
auto ReadRequest(const std::string& name) -> cedar::Request
{
    std::string principal, action, resource, context;
    for (const std::string& line : Lines(ReadFile(CasesDirectory / (name + ".request"))))
    {
        const std::size_t colon = line.find(": ");
        if (colon == std::string::npos) { throw std::runtime_error{"bad request line: " + line}; }
        const std::string key = line.substr(0, colon);
        const std::string value = line.substr(colon + 2);
        if (key == "principal") { principal = value; }
        else if (key == "action") { action = value; }
        else if (key == "resource") { resource = value; }
        else if (key == "context") { context = value; }
    }
    auto loaded = cedar::ContextFromJson(context);
    if (!loaded) { throw std::runtime_error{"bad context in " + name + ":\n" + loaded.error().Render()}; }
    return cedar::Request{Uid(principal), Uid(action), Uid(resource), *std::move(loaded)};
}

// The policy set from `<Name>.cedar`, with the links of `<Name>.links` when there is one
// (`<link id> <template id> ?principal=<uid> ?resource=<uid>` per line).
auto ReadPolicies(const std::string& name) -> cedar::PolicySet
{
    auto policies = cedar::ParsePolicySet(ReadFile(CasesDirectory / (name + ".cedar")), name + ".cedar");
    if (!policies) { throw std::runtime_error{"bad policies in " + name + ":\n" + policies.error().Render()}; }
    const std::filesystem::path linksPath = CasesDirectory / (name + ".links");
    if (!std::filesystem::exists(linksPath)) { return *std::move(policies); }
    for (const std::string& line : Lines(ReadFile(linksPath)))
    {
        std::istringstream words{line};
        std::string linkId, templateId;
        words >> linkId >> templateId;
        cedar::SlotBindings bindings;
        for (std::string word; words >> word;)
        {
            const std::size_t equals = word.find('=');
            const std::string slot = word.substr(0, equals);
            const cedar::EntityUid uid = Uid(word.substr(equals + 1));
            bindings.Bind(slot == "?principal" ? cedar::SlotId::Principal : cedar::SlotId::Resource, uid);
        }
        auto linked = policies->Link(templateId, cedar::PolicyId{linkId}, std::move(bindings));
        if (!linked) { throw std::runtime_error{"bad link in " + name + ":\n" + linked.error().Render()}; }
    }
    return *std::move(policies);
}

// The response written in the `<Name>.expected` format.
auto Describe(const cedar::Response& response) -> std::string
{
    std::string text = response.decision == cedar::Decision::Allow ? "decision: allow\n" : "decision: deny\n";
    for (const cedar::PolicyId& id : response.reason) { text += "reason: " + std::string{id.Text()} + "\n"; }
    for (const cedar::PolicyEvaluationError& error : response.errors)
    {
        text += "error: " + std::string{error.policyId.Text()} + " " + std::string{KindName(error.error.Kind())} + "\n";
    }
    return text;
}

class AuthorizationParity : public testing::TestWithParam<std::string>
{
};

TEST_P(AuthorizationParity, MatchesTheReference)
{
    const std::string& name = GetParam();
    auto entities = cedar::EntitiesFromJson(ReadFile(CasesDirectory / "Entities.json"));
    ASSERT_TRUE(entities) << entities.error().Render();
    const cedar::PolicySet policies = ReadPolicies(name);
    const cedar::Request request = ReadRequest(name);
    const std::string expected = ReadFile(CasesDirectory / (name + ".expected"));

    const cedar::Response everyPolicy = cedar::IsAuthorized(request, policies, *entities);
    EXPECT_EQ(Describe(everyPolicy), expected);
    EXPECT_TRUE(std::ranges::is_sorted(everyPolicy.reason));
    EXPECT_TRUE(std::ranges::is_sorted(everyPolicy.errors, {}, &cedar::PolicyEvaluationError::policyId));

    const cedar::Response scopeIndex = cedar::IsAuthorized(
        request, policies, *entities, cedar::AuthorizerOptions{.policySelection = cedar::PolicySelection::ScopeIndex});
    EXPECT_EQ(Describe(scopeIndex), expected);
}

INSTANTIATE_TEST_SUITE_P(Cases, AuthorizationParity, testing::ValuesIn(CaseNames()),
                         [](const testing::TestParamInfo<std::string>& parameter) { return parameter.param; });

TEST(Authorizer, FindsEveryCase)
{
    EXPECT_EQ(CaseNames().size(), 12U);
}

} // namespace
