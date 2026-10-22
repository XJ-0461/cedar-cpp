#include "json/JsonReader.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <array>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace std::string_view_literals;

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

struct ParityCase
{
    bool accepted;
    std::string kind;
    std::string path;
};

auto ReadParityTable(const std::string& directory) -> std::vector<ParityCase>
{
    std::vector<ParityCase> cases;
    std::istringstream table{ReadFile(directory + "/Expected.tsv")};
    std::string line;
    while (std::getline(table, line))
    {
        const auto first = line.find('\t');
        const auto second = line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos) { continue; }
        cases.push_back(ParityCase{line.substr(0, first) == "accept", line.substr(first + 1, second - first - 1),
                                   line.substr(second + 1)});
    }
    return cases;
}

auto TestSchema() -> const cedar::Schema&
{
    static const cedar::Schema schema = [] {
        auto parsed = cedar::ParseSchema(ReadFile(std::string{CEDAR_JSON_SCHEMA_PARITY_DIRECTORY} + "/Schema.cedarschema"));
        if (!parsed) { throw std::runtime_error{parsed.error().Render()}; }
        return std::move(parsed->value);
    }();
    return schema;
}

auto Uid(const std::string_view type, const std::string_view id) -> cedar::EntityUid
{
    return *cedar::EntityUid::Create(type, id);
}

auto Code(const cedar::DiagnosticCode code) -> std::string_view { return cedar::CodeName(code); }

/// A request case: a `--request-json` file whose `context` is its last member, read as the CLI
/// reads it with request validation: the context by the action's context type, then the request
/// checked against the schema. Nothing on success, else the failure.
auto ReadRequestCase(const std::string& text) -> std::optional<std::string>
{
    std::pmr::monotonic_buffer_resource scratch;
    auto document = cedar::detail::ReadJson(text, scratch);
    if (!document) { return "not JSON"; }
    std::array<std::optional<cedar::EntityUid>, 3> uids{};
    std::optional<std::string> context;
    for (const cedar::detail::JsonMember& member : document->Root().Members())
    {
        const std::array names{"principal"sv, "action"sv, "resource"sv};
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (member.key != names[index]) { continue; }
            auto uid = cedar::ParseEntityUid(member.value.text);
            if (!uid) { return uid.error().Render(); }
            uids[index] = *uid;
        }
        if (member.key == "context")
        {
            // The context runs from its opening brace to the brace before the document's last one.
            const std::size_t end = text.rfind('}', text.rfind('}') - 1);
            context = text.substr(member.value.offset, end + 1 - member.value.offset);
        }
    }
    if (!uids[0] || !uids[1] || !uids[2] || !context) { return "incomplete request"; }
    auto parsed = cedar::ContextFromJson(*context, TestSchema(), *uids[1]);
    if (!parsed) { return parsed.error().Render(); }
    auto request = cedar::Request::Create(*uids[0], *uids[1], *uids[2], *parsed, TestSchema());
    if (!request) { return request.error().Render(); }
    return std::nullopt;
}

// Every case under tests/parity/json-schema, accepted or rejected as the reference CLI does
// (scripts/generate-json-schema-parity-table.sh writes the table).
TEST(SchemaJsonConformance, MatchesTheOracleOnEveryParityCase)
{
    const std::string directory = CEDAR_JSON_SCHEMA_PARITY_DIRECTORY;
    const std::vector<ParityCase> cases = ReadParityTable(directory);
    ASSERT_GT(cases.size(), 120U);
    const cedar::EntityUid view = Uid("App::Action", "view");
    for (const ParityCase& parityCase : cases)
    {
        SCOPED_TRACE(parityCase.path);
        const std::string text = ReadFile(directory + "/" + parityCase.path);
        std::optional<std::string> failure;
        if (parityCase.kind == "entities")
        {
            const auto entities = cedar::EntitiesFromJson(text, TestSchema(), parityCase.path);
            if (!entities) { failure = entities.error().Render(); }
        }
        else if (parityCase.kind == "context")
        {
            const auto context = cedar::ContextFromJson(text, TestSchema(), view, parityCase.path);
            if (!context) { failure = context.error().Render(); }
        }
        else { failure = ReadRequestCase(text); }
        if (parityCase.accepted && failure) { ADD_FAILURE() << "rejected:\n" << *failure; }
        if (!parityCase.accepted) { EXPECT_TRUE(failure.has_value()) << "accepted"; }
    }
}

TEST(SchemaJsonConformance, ReadsValuesByTheirDeclaredTypes)
{
    const auto entities = cedar::EntitiesFromJson(
        R"([{"uid": {"type": "App::User", "id": "alice"},
             "attrs": {"name": "Alice", "age": 30, "ip": "10.0.0.1", "score": "1.5", "timeout": "1h",
                       "manager": {"type": "App::User", "id": "bob"}, "ranges": ["::1"],
                       "joined": {"fn": "offset", "args": ["1970-01-01", "1s"]}},
             "parents": [], "tags": {"t": 1}}])",
        TestSchema());
    ASSERT_TRUE(entities.has_value()) << entities.error().Render();
    const cedar::Entity* alice = entities->Find(Uid("App::User", "alice"));
    ASSERT_NE(alice, nullptr);
    const auto attribute = [&](const std::string_view name) {
        const cedar::Value* value = alice->FindAttribute(name);
        return value == nullptr ? std::string{"<none>"} : cedar::ToCedar(*value);
    };
    EXPECT_EQ(attribute("ip"), R"(ip("10.0.0.1/32"))");
    EXPECT_EQ(attribute("score"), R"(decimal("1.5000"))");
    EXPECT_EQ(attribute("timeout"), R"(duration("3600000ms"))");
    EXPECT_EQ(attribute("manager"), R"(App::User::"bob")");
    EXPECT_EQ(attribute("ranges"), R"([ip("::1/128")])");
    EXPECT_EQ(attribute("joined"), R"(datetime("1970-01-01").offset(duration("1000ms")))");
}

TEST(SchemaJsonConformance, AddsTheSchemaActionsToTheStore)
{
    const auto entities = cedar::EntitiesFromJson("[]", TestSchema());
    ASSERT_TRUE(entities.has_value()) << entities.error().Render();
    EXPECT_EQ(entities->Size(), 3U); // view, edit and audit; the external `sync` is not an entity.
    EXPECT_TRUE(entities->IsAncestorOf(Uid("App::Action", "view"), Uid("App::Action", "audit")));
    EXPECT_FALSE(entities->Contains(Uid("App::Action", "sync")));
}

TEST(SchemaJsonConformance, LeavesUndeclaredEntityTypesUninterned)
{
    for (const std::string_view json :
         {R"([{"uid": {"type": "App::NeverInternedOne", "id": "x"}, "attrs": {}, "parents": []}])"sv,
          R"([{"uid": {"type": "App::User", "id": "a"}, "attrs": {"name": "A", "age": 1},
               "parents": [{"type": "App::NeverInternedTwo", "id": "x"}]}])"sv,
          R"([{"uid": {"type": "App::User", "id": "a"},
               "attrs": {"name": "A", "age": 1, "manager": {"type": "App::NeverInternedThree", "id": "x"}},
               "parents": []}])"sv})
    {
        const auto entities = cedar::EntitiesFromJson(json, TestSchema());
        ASSERT_FALSE(entities.has_value());
        EXPECT_EQ(Code(entities.error().Primary().code), Code(cedar::DiagnosticCode::UnexpectedEntityType));
    }
    EXPECT_FALSE(cedar::EntityType::Find("App::NeverInternedOne").has_value());
    EXPECT_FALSE(cedar::EntityType::Find("App::NeverInternedTwo").has_value());
    EXPECT_FALSE(cedar::EntityType::Find("App::NeverInternedThree").has_value());
}

TEST(SchemaJsonConformance, ChecksOneEntity)
{
    EXPECT_TRUE(cedar::EntityFromJson(R"({"uid": {"type": "App::Color", "id": "red"}, "attrs": {}, "parents": []})",
                                      TestSchema())
                    .has_value());
    const auto blue = cedar::EntityFromJson(
        R"({"uid": {"type": "App::Color", "id": "blue"}, "attrs": {}, "parents": []})", TestSchema());
    ASSERT_FALSE(blue.has_value());
    EXPECT_EQ(Code(blue.error().Primary().code), Code(cedar::DiagnosticCode::InvalidEnumEntity));
    // Alone, an action entity's parents stand for its ancestors.
    const auto audit = cedar::EntityFromJson(
        R"({"uid": {"type": "App::Action", "id": "audit"}, "attrs": {}, "parents": [{"type": "App::Action", "id": "edit"}]})",
        TestSchema());
    ASSERT_FALSE(audit.has_value());
    EXPECT_EQ(Code(audit.error().Primary().code), Code(cedar::DiagnosticCode::ActionDeclarationMismatch));
}

TEST(SchemaJsonConformance, ChecksEntitiesBuiltInMemory)
{
    auto arena = std::make_shared<cedar::ValueArena>();
    const std::array entries{cedar::RecordEntry{"age", cedar::Value::FromString("thirty")},
                             cedar::RecordEntry{"name", cedar::Value::FromString("Alice")}};
    const auto attributes = arena->MakeRecord(entries);
    ASSERT_TRUE(attributes.has_value());
    std::vector<cedar::Entity> entities;
    entities.emplace_back(Uid("App::User", "alice"), std::vector<cedar::EntityUid>{}, attributes->AsRecord(),
                          cedar::Record{}, arena);
    const auto store = cedar::Entities::Create(std::move(entities), cedar::TransitiveClosureMode::ComputeNow, TestSchema());
    ASSERT_FALSE(store.has_value());
    EXPECT_EQ(Code(store.error().Primary().code), Code(cedar::DiagnosticCode::TypeMismatch));
}

TEST(SchemaJsonConformance, ValidatesRequests)
{
    const auto context = cedar::ContextFromJson(R"({"ip": "10.0.0.1"})", TestSchema(), Uid("App::Action", "view"));
    ASSERT_TRUE(context.has_value()) << context.error().Render();
    const auto code = [&](const cedar::EntityUid& principal, const cedar::EntityUid& action,
                          const cedar::EntityUid& resource, const cedar::Context& requestContext) -> std::string_view {
        const auto request = cedar::Request::Create(principal, action, resource, requestContext, TestSchema());
        return request ? "valid"sv : Code(request.error().Primary().code);
    };
    const cedar::EntityUid alice = Uid("App::User", "alice");
    const cedar::EntityUid photo = Uid("App::Photo", "p");
    const cedar::EntityUid view = Uid("App::Action", "view");
    EXPECT_EQ(code(alice, view, photo, *context), "valid");
    EXPECT_EQ(code(Uid("App::Group", "g"), view, photo, *context), Code(cedar::DiagnosticCode::InvalidPrincipalType));
    EXPECT_EQ(code(alice, view, alice, *context), Code(cedar::DiagnosticCode::InvalidResourceType));
    EXPECT_EQ(code(Uid("App::Nobody", "x"), view, photo, *context),
              Code(cedar::DiagnosticCode::UndeclaredPrincipalType));
    EXPECT_EQ(code(Uid("App::Color", "blue"), view, photo, *context),
              Code(cedar::DiagnosticCode::RequestInvalidEnumEntity));
    EXPECT_EQ(code(alice, Uid("App::Action", "nope"), photo, *context),
              Code(cedar::DiagnosticCode::RequestUndeclaredAction));
    EXPECT_EQ(code(alice, Uid("App::Action", "sync"), photo, *context),
              Code(cedar::DiagnosticCode::RequestExternalAction));
    EXPECT_EQ(code(alice, view, photo, cedar::Context{}), Code(cedar::DiagnosticCode::InvalidContext));

    const auto wrongLevel =
        cedar::ContextFromJson(R"({"ip": "10.0.0.1", "level": "high"})", TestSchema(), Uid("App::Action", "view"));
    ASSERT_TRUE(wrongLevel.has_value()) << "reading by the context type does not check primitive types";
    const auto validated = wrongLevel->Validate(TestSchema(), view);
    ASSERT_FALSE(validated.has_value());
    EXPECT_EQ(Code(validated.error().Primary().code), Code(cedar::DiagnosticCode::InvalidContext));

    for (const std::string_view action : {"nope"sv, "sync"sv})
    {
        const auto missing = cedar::ContextFromJson("{}", TestSchema(), Uid("App::Action", action));
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(Code(missing.error().Primary().code), Code(cedar::DiagnosticCode::ContextMissingAction));
    }
}

// Rust reads `{"__extn": {"fn": "unknown", ...}}` as a partial-evaluation unknown of any type; we
// have no partial evaluation and refuse it, as the reader without a schema does.
TEST(SchemaJsonConformance, RefusesUnknownValues)
{
    const auto entities = cedar::EntitiesFromJson(
        R"([{"uid": {"type": "App::User", "id": "a"},
             "attrs": {"name": "A", "age": 1, "ip": {"__extn": {"fn": "unknown", "arg": "u"}}}, "parents": []}])",
        TestSchema());
    EXPECT_FALSE(entities.has_value());
}

} // namespace
