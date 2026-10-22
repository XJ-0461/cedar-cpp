#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
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

auto Attribute(const cedar::Entities& entities, const std::string_view type, const std::string_view id,
               const std::string_view attribute) -> std::string
{
    const auto uid = cedar::EntityUid::Create(type, id);
    if (!uid) { return "<bad uid>"; }
    const cedar::Entity* entity = entities.Find(*uid);
    if (entity == nullptr) { return "<no entity>"; }
    const cedar::Value* value = entity->FindAttribute(attribute);
    return value == nullptr ? "<no attribute>" : cedar::ToCedar(*value);
}

// Every case under tests/parity/json, accepted or rejected as the reference CLI does
// (scripts/generate-json-parity-table.sh writes the table).
TEST(EntityJson, MatchesTheOracleOnEveryParityCase)
{
    const std::string directory = CEDAR_JSON_PARITY_DIRECTORY;
    const std::vector<ParityCase> cases = ReadParityTable(directory);
    ASSERT_GT(cases.size(), 200U);
    for (const ParityCase& parityCase : cases)
    {
        SCOPED_TRACE(parityCase.path);
        const std::string text = ReadFile(directory + "/" + parityCase.path);
        if (parityCase.kind == "entities")
        {
            const auto entities = cedar::EntitiesFromJson(text, parityCase.path);
            if (parityCase.accepted && !entities) { ADD_FAILURE() << "rejected:\n" << entities.error().Render(); }
            if (!parityCase.accepted) { EXPECT_FALSE(entities.has_value()) << "accepted"; }
        }
        else
        {
            const auto context = cedar::ContextFromJson(text, parityCase.path);
            if (parityCase.accepted && !context) { ADD_FAILURE() << "rejected:\n" << context.error().Render(); }
            if (!parityCase.accepted)
            {
                EXPECT_FALSE(context.has_value()) << "accepted: " << cedar::ToCedar(context->AsValue());
            }
        }
    }
}

TEST(EntityJson, ReadsValues)
{
    const auto entities = cedar::EntitiesFromJson(R"([
        {"uid": {"type": "A", "id": "b"}, "parents": [{"type": "A", "id": "c"}],
         "attrs": {"x": 1, "s": "hé", "e": {"__entity": {"type": "A", "id": "c"}},
                   "i": {"__extn": {"fn": "ip", "arg": "10.0.0.1/24"}}, "set": [3, 1, 2, 1],
                   "r": {"z": true, "a": false}, "q": {"__extn": {"fn": "ip"}},
                   "d": {"__extn": {"fn": "offset", "args": [{"__extn": {"fn": "datetime", "arg": "1970-01-02"}},
                                                             {"__extn": {"fn": "duration", "arg": "1h"}}]}}},
         "tags": {"t": {"__extn": {"fn": "decimal", "arg": "1.5"}}}},
        {"uid": {"type": "A", "id": "c"}, "attrs": {}, "parents": [{"type": "A", "id": "d"}]}
    ])");
    ASSERT_TRUE(entities) << entities.error().Render();
    EXPECT_EQ(Attribute(*entities, "A", "b", "x"), "1");
    EXPECT_EQ(Attribute(*entities, "A", "b", "s"), R"("hé")");
    EXPECT_EQ(Attribute(*entities, "A", "b", "e"), R"(A::"c")");
    EXPECT_EQ(Attribute(*entities, "A", "b", "i"), R"(ip("10.0.0.1/24"))");
    EXPECT_EQ(Attribute(*entities, "A", "b", "set"), "[1, 2, 3]");
    EXPECT_EQ(Attribute(*entities, "A", "b", "r"), "{a: false, z: true}");
    // An `__extn` object without `arg` or `args` is a record, not an escape.
    EXPECT_EQ(Attribute(*entities, "A", "b", "q"), R"({__extn: {fn: "ip"}})");
    EXPECT_EQ(Attribute(*entities, "A", "b", "d"), R"(datetime("1970-01-01").offset(duration("90000000ms")))");
    const auto b = cedar::EntityUid::Create("A", "b");
    const auto d = cedar::EntityUid::Create("A", "d");
    ASSERT_TRUE(b && d);
    const cedar::Entity* entity = entities->Find(*b);
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(entity->FindTag("t"), nullptr);
    EXPECT_EQ(cedar::ToCedar(*entity->FindTag("t")), R"(decimal("1.5000"))");
    // The store closes the ancestor sets.
    EXPECT_TRUE(entity->IsDescendantOf(*d));
}

TEST(EntityJson, ReportsTheCondition)
{
    const auto code = [](const std::string_view json) {
        const auto entities = cedar::EntitiesFromJson(json);
        return entities ? cedar::DiagnosticCode{} : entities.error().Primary().code;
    };
    const std::string_view prefix = R"([{"uid": {"type": "A", "id": "b"}, "parents": [], "attrs": )";
    const auto attributes = [&](const std::string_view attrs) {
        return code(std::string{prefix} + std::string{attrs} + "}]");
    };
    EXPECT_EQ(code("[1"), cedar::DiagnosticCode::JsonSyntax);
    EXPECT_EQ(code("{}"), cedar::DiagnosticCode::JsonUnexpectedShape);
    EXPECT_EQ(attributes(R"({"x": 1, "x": 2})"), cedar::DiagnosticCode::DuplicateKey);
    EXPECT_EQ(attributes(R"({"x": null})"), cedar::DiagnosticCode::NullValue);
    EXPECT_EQ(attributes(R"({"x": 1.5})"), cedar::DiagnosticCode::JsonUnexpectedShape);
    EXPECT_EQ(attributes(R"({"x": {"__expr": "1"}})"), cedar::DiagnosticCode::ExpressionTag);
    EXPECT_EQ(attributes(R"({"x": {"__entity": {"type": "if", "id": "1"}}})"), cedar::DiagnosticCode::InvalidEscapeValue);
    EXPECT_EQ(attributes(R"({"x": {"__extn": {"fn": "ip", "arg": "1.2.3.x"}}})"),
              cedar::DiagnosticCode::EntityAttributeEvaluation);
    EXPECT_EQ(code(R"([{"uid": "A::\"b\"", "parents": [], "attrs": {}}])"),
              cedar::DiagnosticCode::ExpectedLiteralEntityReference);
    EXPECT_EQ(code(R"([{"uid": {"type": "Action", "id": "a"}, "parents": [{"type": "A", "id": "c"}], "attrs": {}}])"),
              cedar::DiagnosticCode::ActionParentIsNotAction);
    EXPECT_EQ(code(R"([{"uid": {"type": "A", "id": "b"}, "parents": [{"type": "A", "id": "b"}], "attrs": {}}])"),
              cedar::DiagnosticCode::TransitiveClosureCycle);

    const auto context = cedar::ContextFromJson(R"({"__entity": {"type": "A", "id": "b"}})");
    ASSERT_FALSE(context);
    EXPECT_EQ(context.error().Primary().code, cedar::DiagnosticCode::ContextNotARecord);
    const auto failedCall = cedar::ContextFromJson(R"({"a": {"__extn": {"fn": "nope", "arg": "1"}}})");
    ASSERT_FALSE(failedCall);
    EXPECT_EQ(failedCall.error().Primary().code, cedar::DiagnosticCode::ExtensionFunctionLookupFailed);
}

// Rust reads an `__extn` escape calling `unknown` as an unknown value for partial evaluation when it
// is an attribute's whole value (`ValueParser::val_into_restricted_expr`, `parse_as_unknown`).
// cedar-cpp has no partial evaluation, so `unknown` is an extension function that does not exist.
TEST(EntityJson, RefusesUnknownValues)
{
    const auto entities = cedar::EntitiesFromJson(
        R"([{"uid": {"type": "A", "id": "b"}, "parents": [], "attrs": {"x": {"__extn": {"fn": "unknown", "arg": "u"}}}}])");
    ASSERT_FALSE(entities);
    EXPECT_EQ(entities.error().Primary().code, cedar::DiagnosticCode::EntityAttributeEvaluation);
}

// Rust's readers take `&str`, so their input is UTF-8 by type; ours checks it, everywhere in the
// document (the CLI's file reader lets invalid UTF-8 through in a skipped entity field).
TEST(EntityJson, RefusesInvalidUtf8)
{
    EXPECT_FALSE(cedar::EntitiesFromJson("[{\"uid\": {\"type\": \"A\", \"id\": \"b\"}, \"parents\": [], \"attrs\": {}, "
                                         "\"note\": \"\xff\"}]"));
    EXPECT_FALSE(cedar::ContextFromJson("{\"a\": \"\xc3\"}"));
    EXPECT_FALSE(cedar::EntityUidFromJson("{\"type\": \"A\", \"id\": \"\xed\xa0\x80\"}"));
}

TEST(EntityJson, ReadsOneEntity)
{
    const auto entity = cedar::EntityFromJson(
        R"({"uid": {"type": "A", "id": "b"}, "parents": [["A", "c"]], "attrs": {"n": 2}, "tags": {"t": "x"}})");
    ASSERT_TRUE(entity) << entity.error().Render();
    EXPECT_EQ(entity->Uid().ToString(), R"(A::"b")");
    const auto c = cedar::EntityUid::Create("A", "c");
    ASSERT_TRUE(c);
    EXPECT_TRUE(entity->Parents().Contains(*c));
    ASSERT_NE(entity->FindAttribute("n"), nullptr);
    EXPECT_EQ(entity->FindAttribute("n")->AsLong(), 2);
    ASSERT_NE(entity->FindTag("t"), nullptr);
    EXPECT_EQ(entity->FindTag("t")->AsString(), "x");

    const auto sequence = cedar::EntityFromJson(R"([{"type": "A", "id": "b"}, {}, []])");
    ASSERT_TRUE(sequence) << sequence.error().Render();
    EXPECT_FALSE(cedar::EntityFromJson("[]"));
    EXPECT_FALSE(cedar::EntityFromJson(R"({"uid": {"type": "A", "id": "b"}, "attrs": {}})"));
}

// Rust's `EntityUid::from_json` reads a `serde_json::Value`, so a repeated key keeps its last value
// (unlike an entity's `uid` field, where a repeated `type` fails).
TEST(EntityJson, ReadsEntityUids)
{
    const auto read = [](const std::string_view json) -> std::string {
        const auto uid = cedar::EntityUidFromJson(json);
        return uid ? uid->ToString() : "error " + std::string{cedar::CodeName(uid.error().Primary().code)};
    };
    EXPECT_EQ(read(R"({"type": "A::B", "id": "x"})"), R"(A::B::"x")");
    EXPECT_EQ(read(R"({"__entity": {"type": "A", "id": "x"}})"), R"(A::"x")");
    EXPECT_EQ(read(R"(["A", "x"])"), R"(A::"x")");
    EXPECT_EQ(read(R"({"type": "A", "id": "x", "other": 1.5})"), R"(A::"x")");
    EXPECT_EQ(read(R"({"type": "A", "type": "B", "id": "x"})"), R"(B::"x")");
    EXPECT_EQ(read(R"({"__expr": "A::\"x\""})"), "error entities.expression-tag");
    EXPECT_EQ(read(R"({"type": "A"})"), "error entities.expected-literal-entity-reference");
    EXPECT_EQ(read(R"("A::\"x\"")"), "error entities.expected-literal-entity-reference");
    EXPECT_EQ(read(R"({"type": "A b", "id": "x"})"), "error entities.invalid-escape-value");
    EXPECT_EQ(read(R"({"type": "A", "id": "x"} 1)"), "error parse.json-syntax");
}

TEST(EntityJson, WritesWhatItReads)
{
    const std::string json =
        R"([{"uid":{"type":"A","id":"b"},"attrs":{"__extn":1,"d":{"__extn":{"fn":"decimal","arg":"-1.2300"}},)"
        R"("e":{"__entity":{"type":"A","id":"c"}},"i":{"__extn":{"fn":"ip","arg":"10.0.0.1/24"}},)"
        R"("s":"q\"\n\u0001","set":[1,2],"t":{"__extn":{"fn":"offset","args":[{"__extn":{"fn":"datetime","arg":"1970-01-01"}},)"
        R"({"__extn":{"fn":"duration","arg":"5ms"}}]}},"u":{"__extn":{"fn":"duration","arg":"-7ms"}}},)"
        R"("parents":[{"type":"A","id":"c"},{"type":"A","id":"d"}],"tags":{"k":true}},)"
        R"({"uid":{"type":"A","id":"c"},"attrs":{},"parents":[{"type":"A","id":"d"}]}])";
    const auto entities = cedar::EntitiesFromJson(json);
    ASSERT_TRUE(entities) << entities.error().Render();
    const auto written = cedar::ToJson(*entities);
    ASSERT_TRUE(written) << written.error().Render();
    EXPECT_EQ(*written, json);

    const auto context = cedar::ContextFromJson(R"({"b":[{"__entity":{"type":"A","id":"x"}}],"a":-3})");
    ASSERT_TRUE(context) << context.error().Render();
    const auto contextJson = cedar::ToJson(*context);
    ASSERT_TRUE(contextJson);
    EXPECT_EQ(*contextJson, R"({"a":-3,"b":[{"__entity":{"type":"A","id":"x"}}]})");

    const auto uid = cedar::EntityUid::Create("A::B", "x\"y");
    ASSERT_TRUE(uid);
    EXPECT_EQ(cedar::ToJson(*uid), R"({"type":"A::B","id":"x\"y"})");
}

TEST(EntityJson, RefusesToWriteAReservedRecordKey)
{
    const auto context = cedar::ContextFromJson(R"({"a": {"__entity": 1, "b": 2}})");
    ASSERT_TRUE(context) << context.error().Render();
    const auto written = cedar::ToJson(*context);
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().Primary().code, cedar::DiagnosticCode::SerializationReservedKey);
}

} // namespace
