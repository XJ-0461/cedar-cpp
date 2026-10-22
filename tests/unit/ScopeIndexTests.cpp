// PolicySelection::ScopeIndex gives the same Response as EvaluateEveryPolicy: decision, determining
// policy ids, erroring policy ids and error kinds. Checked on every case in
// tests/parity/authorization and, when the benchmark workloads are generated
// (CEDAR_BENCHMARK_WORKLOADS_DIRECTORY), on each application's policy sets with its own requests and
// with random ones, and on random scope-heavy policy sets over its entities.

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Json.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>
#include <cedar/Syntax.hpp>

#include <json/JsonReader.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory_resource>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
    if (!uid) { throw std::runtime_error{"bad entity uid: " + std::string{text}}; }
    return *std::move(uid);
}

auto Describe(const cedar::Response& response) -> std::string
{
    std::string text = response.decision == cedar::Decision::Allow ? "decision: allow\n" : "decision: deny\n";
    for (const cedar::PolicyId& id : response.reason) { text += std::format("reason: {}\n", id.Text()); }
    for (const cedar::PolicyEvaluationError& error : response.errors)
    {
        text += std::format("error: {} {}\n", error.policyId.Text(), static_cast<int>(error.error.Kind()));
    }
    return text;
}

// The two selections' responses to one request.
struct Comparison
{
    std::string everyPolicy;
    std::string scopeIndex;
};

auto Compare(const cedar::Request& request, const cedar::PolicySet& policies, const cedar::EntityStoreView entities)
    -> Comparison
{
    return Comparison{
        .everyPolicy = Describe(cedar::IsAuthorized(
            request, policies, entities,
            cedar::AuthorizerOptions{.policySelection = cedar::PolicySelection::EvaluateEveryPolicy})),
        .scopeIndex = Describe(cedar::IsAuthorized(
            request, policies, entities, cedar::AuthorizerOptions{.policySelection = cedar::PolicySelection::ScopeIndex})),
    };
}

auto LinkPolicy(cedar::PolicySet& policies, const std::string_view templateId, const std::string& linkId,
                cedar::SlotBindings bindings) -> void
{
    auto linked = policies.Link(templateId, cedar::PolicyId{linkId}, std::move(bindings));
    if (!linked) { throw std::runtime_error{"bad link " + linkId + ":\n" + linked.error().Render()}; }
}

// ---- The authorization parity cases ----

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
auto ReadCaseRequest(const std::string& name) -> cedar::Request
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

// `<Name>.cedar`, with the links of `<Name>.links` (`<link id> <template id> ?slot=<uid> ...`).
auto ReadCasePolicies(const std::string& name) -> cedar::PolicySet
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
            bindings.Bind(word.substr(0, equals) == "?principal" ? cedar::SlotId::Principal : cedar::SlotId::Resource,
                          Uid(word.substr(equals + 1)));
        }
        LinkPolicy(*policies, templateId, linkId, std::move(bindings));
    }
    return *std::move(policies);
}

class ScopeIndexCase : public testing::TestWithParam<std::string>
{
};

TEST_P(ScopeIndexCase, GivesTheSameResponse)
{
    const std::string& name = GetParam();
    auto entities = cedar::EntitiesFromJson(ReadFile(CasesDirectory / "Entities.json"));
    ASSERT_TRUE(entities) << entities.error().Render();
    const Comparison comparison = Compare(ReadCaseRequest(name), ReadCasePolicies(name), *entities);
    EXPECT_EQ(comparison.scopeIndex, comparison.everyPolicy);
}

INSTANTIATE_TEST_SUITE_P(Cases, ScopeIndexCase, testing::ValuesIn(CaseNames()),
                         [](const testing::TestParamInfo<std::string>& parameter) { return parameter.param; });

TEST(ScopeIndex, KeepsWorkingAfterTheSetChanges)
{
    auto entities = cedar::EntitiesFromJson(ReadFile(CasesDirectory / "Entities.json"));
    ASSERT_TRUE(entities) << entities.error().Render();
    auto policies = cedar::ParsePolicySet(R"(permit(principal, action, resource);)");
    ASSERT_TRUE(policies);
    const cedar::Request request{Uid(R"(User::"alice")"), Uid(R"(Action::"view")"), Uid(R"(Photo::"a")")};
    const cedar::AuthorizerOptions index{.policySelection = cedar::PolicySelection::ScopeIndex};
    EXPECT_EQ(cedar::IsAuthorized(request, *policies, *entities, index).decision, cedar::Decision::Allow);

    const cedar::PolicySet before = *policies;
    auto forbid = cedar::ParsePolicySet(R"(forbid(principal == User::"alice", action, resource);)");
    ASSERT_TRUE(forbid);
    ASSERT_TRUE(policies->Merge(*forbid, true));
    EXPECT_EQ(cedar::IsAuthorized(request, *policies, *entities, index).decision, cedar::Decision::Deny);
    EXPECT_EQ(cedar::IsAuthorized(request, before, *entities, index).decision, cedar::Decision::Allow);
    ASSERT_TRUE(policies->RemoveStatic("policy1"));
    EXPECT_EQ(cedar::IsAuthorized(request, *policies, *entities, index).decision, cedar::Decision::Allow);

    cedar::PolicySet moved = std::move(*policies);
    EXPECT_EQ(cedar::IsAuthorized(request, moved, *entities, index).decision, cedar::Decision::Allow);
}

#ifdef CEDAR_BENCHMARK_WORKLOADS_DIRECTORY

// ---- The benchmark workloads ----

const std::filesystem::path WorkloadsDirectory{CEDAR_BENCHMARK_WORKLOADS_DIRECTORY};
constexpr std::size_t RandomRequestCount = 150;
constexpr std::size_t RandomPolicyCount = 200;
constexpr std::size_t RandomTemplateCount = 5;
constexpr std::size_t RandomLinkCount = 60;

// The text of the JSON value that starts at `offset`: an object, array or string, by bracket
// matching outside strings.
auto ValueText(const std::string_view document, std::size_t offset) -> std::string_view
{
    const std::size_t start = offset;
    std::size_t depth = 0;
    bool inString = false;
    for (; offset < document.size(); ++offset)
    {
        const char character = document[offset];
        if (inString)
        {
            if (character == '\\') { ++offset; }
            else if (character == '"') { inString = false; }
            continue;
        }
        if (character == '"') { inString = true; }
        else if (character == '{' || character == '[') { ++depth; }
        else if ((character == '}' || character == ']') && --depth == 0) { break; }
    }
    return document.substr(start, offset + 1 - start);
}

auto Member(const cedar::detail::JsonNode& object, const std::string_view key) -> const cedar::detail::JsonNode&
{
    for (const cedar::detail::JsonMember& member : object.Members())
    {
        if (member.key == key) { return member.value; }
    }
    throw std::runtime_error{"missing JSON member " + std::string{key}};
}

struct Application
{
    cedar::Schema schema;
    cedar::Entities entities;
    std::vector<cedar::Request> requests;
    std::vector<cedar::EntityUid> uids;
    std::vector<cedar::EntityUid> actions;
};

auto Require(auto result, const std::string_view what)
{
    if (!result) { throw std::runtime_error{std::string{what} + ":\n" + result.error().Render()}; }
    return *std::move(result);
}

auto LoadApplication(const std::string& name) -> Application
{
    const std::filesystem::path directory = WorkloadsDirectory / name;
    cedar::Schema schema = Require(cedar::ParseSchema(ReadFile(directory / "schema.cedarschema")), "schema").value;
    cedar::Entities entities = Require(cedar::EntitiesFromJson(ReadFile(directory / "entities.json"), schema), "entities");
    std::vector<cedar::Request> requests;
    const std::string text = ReadFile(directory / "requests.json");
    std::pmr::monotonic_buffer_resource resource;
    auto document = cedar::detail::ReadJson(text, resource);
    if (!document) { throw std::runtime_error{"bad requests.json: " + document.error().message}; }
    for (const cedar::detail::JsonNode& request : document->Root().Elements())
    {
        cedar::EntityUid action = Uid(Member(request, "action").text);
        cedar::Context context = Require(
            cedar::ContextFromJson(ValueText(text, Member(request, "context").offset), schema, action), "context");
        requests.emplace_back(Uid(Member(request, "principal").text), std::move(action),
                              Uid(Member(request, "resource").text), std::move(context));
    }
    std::vector<cedar::EntityUid> uids;
    std::vector<cedar::EntityUid> actions;
    for (const cedar::Entity& entity : entities)
    {
        (entity.Uid().IsAction() ? actions : uids).push_back(entity.Uid());
    }
    for (const cedar::Request& request : requests) { actions.push_back(request.Action()); }
    std::ranges::sort(uids);
    std::ranges::sort(actions);
    actions.erase(std::ranges::unique(actions).begin(), actions.end());
    return Application{std::move(schema), std::move(entities), std::move(requests), std::move(uids), std::move(actions)};
}

auto LoadWorkloadPolicySet(const std::string& name, const std::size_t size) -> cedar::PolicySet
{
    const std::filesystem::path directory = WorkloadsDirectory / name;
    cedar::PolicySet policies =
        Require(cedar::ParsePolicySet(ReadFile(directory / std::format("policies-{}.cedar", size))), "policies");
    const std::string text = ReadFile(directory / std::format("links-{}.json", size));
    std::pmr::monotonic_buffer_resource resource;
    auto document = cedar::detail::ReadJson(text, resource);
    if (!document) { throw std::runtime_error{"bad links: " + document.error().message}; }
    for (const cedar::detail::JsonNode& link : document->Root().Elements())
    {
        cedar::SlotBindings bindings;
        for (const cedar::detail::JsonMember& argument : Member(link, "args").Members())
        {
            bindings.Bind(argument.key == "?principal" ? cedar::SlotId::Principal : cedar::SlotId::Resource,
                          Uid(argument.value.text));
        }
        LinkPolicy(policies, Member(link, "template_id").text, std::string{Member(link, "link_id").text},
                   std::move(bindings));
    }
    return policies;
}

class RandomSource
{
public:
    explicit RandomSource(const std::string& seed)
        : engine_{static_cast<std::mt19937_64::result_type>(std::hash<std::string>{}(seed))}
    {
    }

    auto Below(const std::size_t bound) -> std::size_t
    {
        return std::uniform_int_distribution<std::size_t>{0, bound - 1}(engine_);
    }
    auto Chance(const double probability) -> bool { return std::bernoulli_distribution{probability}(engine_); }
    template <typename Element>
    auto Pick(const std::vector<Element>& elements) -> const Element&
    {
        return elements[Below(elements.size())];
    }

private:
    std::mt19937_64 engine_;
};

// A uid of `uid`'s type that is not in the store.
auto Absent(const cedar::EntityUid& uid) -> cedar::EntityUid
{
    return Uid(std::format(R"({}::"absent-from-the-store")", uid.Type().ToString()));
}

// An entity, sometimes one missing from the store.
auto PickEntity(RandomSource& random, const std::vector<cedar::EntityUid>& uids) -> cedar::EntityUid
{
    const cedar::EntityUid& uid = random.Pick(uids);
    return random.Chance(0.1) ? Absent(uid) : uid;
}

// The application's requests and random ones: entities drawn from the store (some missing from
// it), and a context taken from one of its requests or empty, often not the one the action expects.
auto Requests(const Application& application, RandomSource& random) -> std::vector<cedar::Request>
{
    std::vector<cedar::Request> requests = application.requests;
    for (std::size_t count = 0; count < RandomRequestCount; ++count)
    {
        cedar::Context context = random.Chance(0.2) ? cedar::Context{} : random.Pick(application.requests).GetContext();
        requests.emplace_back(PickEntity(random, application.uids), PickEntity(random, application.actions),
                              PickEntity(random, application.uids), std::move(context));
    }
    return requests;
}

// A uid to constrain to: often one of a random entity's ancestors, so `in` matches through them.
auto ConstraintTarget(RandomSource& random, const Application& application, const std::vector<cedar::EntityUid>& pool)
    -> cedar::EntityUid
{
    const cedar::EntityUid& uid = random.Pick(pool);
    if (random.Chance(0.5))
    {
        if (const cedar::Entity* entity = application.entities.Find(uid); entity != nullptr && !entity->Ancestors().IsEmpty())
        {
            std::vector<cedar::EntityUid> ancestors{entity->Ancestors().begin(), entity->Ancestors().end()};
            std::ranges::sort(ancestors);
            return random.Pick(ancestors);
        }
    }
    return random.Chance(0.05) ? Absent(uid) : uid;
}

auto PrincipalOrResourceScope(RandomSource& random, const Application& application, const std::string_view variable,
                              const bool slot) -> std::string
{
    const std::string target = slot ? std::format("?{}", variable)
                                    : ConstraintTarget(random, application, application.uids).ToString();
    const std::string type = random.Pick(application.uids).Type().ToString();
    switch (random.Below(slot ? 3 : 5))
    {
    case 0: return std::format("{} == {}", variable, target);
    case 1: return std::format("{} in {}", variable, target);
    case 2: return std::format("{} is {} in {}", variable, type, target);
    case 3: return std::format("{} is {}", variable, type);
    default: return std::string{variable};
    }
}

auto ActionScope(RandomSource& random, const Application& application) -> std::string
{
    switch (random.Below(4))
    {
    case 0: return std::format("action == {}", ConstraintTarget(random, application, application.actions).ToString());
    case 1: return std::format("action in {}", ConstraintTarget(random, application, application.actions).ToString());
    case 2:
        return std::format("action in [{}, {}]", ConstraintTarget(random, application, application.actions).ToString(),
                           ConstraintTarget(random, application, application.actions).ToString());
    default: return "action";
    }
}

// Conditions that hold, fail or error depending on the request.
auto Condition(RandomSource& random) -> std::string_view
{
    constexpr std::array<std::string_view, 7> conditions{
        "",
        " when { principal has noSuchAttribute }",
        " when { principal.noSuchAttribute == 1 }",
        " when { context.noSuchAttribute }",
        " unless { resource in principal }",
        " when { principal == resource || 9223372036854775807 + 1 == 0 }",
        " when { resource.noSuchAttribute || true }",
    };
    return conditions[random.Below(conditions.size())];
}

// Static policies and linked templates with random scopes over the application's entities.
auto RandomPolicySet(const Application& application, RandomSource& random) -> cedar::PolicySet
{
    std::string text;
    for (std::size_t count = 0; count < RandomPolicyCount + RandomTemplateCount; ++count)
    {
        const bool isTemplate = count >= RandomPolicyCount;
        text += std::format("{}({}, {}, {}){};\n", random.Chance(0.2) ? "forbid" : "permit",
                            PrincipalOrResourceScope(random, application, "principal", isTemplate && random.Chance(0.7)),
                            ActionScope(random, application),
                            PrincipalOrResourceScope(random, application, "resource", isTemplate && random.Chance(0.7)),
                            Condition(random));
    }
    cedar::PolicySet policies = Require(cedar::ParsePolicySet(text), "random policies");
    std::vector<std::string> templateIds;
    for (const auto& body : policies.Templates()) { templateIds.emplace_back(body->Id().Text()); }
    for (std::size_t count = 0; count < RandomLinkCount && !templateIds.empty(); ++count)
    {
        const std::string& templateId = random.Pick(templateIds);
        cedar::SlotBindings bindings;
        for (const cedar::SlotId slot : policies.GetTemplate(templateId)->Slots())
        {
            bindings.Bind(slot, ConstraintTarget(random, application, application.uids));
        }
        LinkPolicy(policies, templateId, std::format("link{}", count), std::move(bindings));
    }
    return policies;
}

auto ApplicationNames() -> std::vector<std::string>
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator{WorkloadsDirectory})
    {
        if (entry.is_directory()) { names.push_back(entry.path().filename().string()); }
    }
    std::ranges::sort(names);
    return names;
}

class ScopeIndexWorkload : public testing::TestWithParam<std::string>
{
};

TEST_P(ScopeIndexWorkload, GivesTheSameResponseOnTheBenchmarkPolicySets)
{
    const Application application = LoadApplication(GetParam());
    RandomSource random{GetParam()};
    const std::vector<cedar::Request> requests = Requests(application, random);
    for (const std::size_t size : {10U, 100U, 1000U})
    {
        const cedar::PolicySet policies = LoadWorkloadPolicySet(GetParam(), size);
        for (std::size_t index = 0; index < requests.size(); ++index)
        {
            const Comparison comparison = Compare(requests[index], policies, application.entities);
            ASSERT_EQ(comparison.scopeIndex, comparison.everyPolicy) << size << " policies, request " << index;
        }
    }
}

TEST_P(ScopeIndexWorkload, GivesTheSameResponseOnRandomScopes)
{
    const Application application = LoadApplication(GetParam());
    RandomSource random{GetParam() + "/random scopes"};
    const cedar::PolicySet policies = RandomPolicySet(application, random);
    const std::vector<cedar::Request> requests = Requests(application, random);
    std::size_t errors = 0;
    std::size_t reasons = 0;
    for (std::size_t index = 0; index < requests.size(); ++index)
    {
        const cedar::Response response = cedar::IsAuthorized(
            requests[index], policies, application.entities,
            cedar::AuthorizerOptions{.policySelection = cedar::PolicySelection::EvaluateEveryPolicy});
        errors += response.errors.size();
        reasons += response.reason.size();
        const Comparison comparison = Compare(requests[index], policies, application.entities);
        ASSERT_EQ(comparison.scopeIndex, comparison.everyPolicy) << "request " << index;
    }
    // The random sets must exercise both outcomes, or the comparison proves little.
    EXPECT_GT(errors, 0U);
    EXPECT_GT(reasons, 0U);
}

INSTANTIATE_TEST_SUITE_P(Applications, ScopeIndexWorkload, testing::ValuesIn(ApplicationNames()),
                         [](const testing::TestParamInfo<std::string>& parameter) { return parameter.param; });

TEST(ScopeIndex, FindsEveryApplication)
{
    EXPECT_EQ(ApplicationNames().size(), 8U);
}

#endif

} // namespace
