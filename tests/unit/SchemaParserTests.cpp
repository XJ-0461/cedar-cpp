#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"
#include "schema/SchemaParser.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

namespace schema_type = cedar::detail::schema_type;
using cedar::DiagnosticCode;

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    std::stringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// One row of tests/parity/schemas/cedar/Expected.tsv (scripts/generate-schema-cedar-parity-cases.sh).
struct ParityCase
{
    std::string parse;
    std::string resolved;
    std::string printed;
    std::string warnings;
    std::string name;
};

auto ReadParityTable(const std::string& path) -> std::vector<ParityCase>
{
    std::vector<ParityCase> cases;
    std::istringstream table{ReadFile(path)};
    std::string line;
    while (std::getline(table, line))
    {
        std::vector<std::string> columns;
        std::istringstream fields{line};
        std::string field;
        while (std::getline(fields, field, '\t')) { columns.push_back(field); }
        if (columns.size() != 5) { continue; }
        cases.push_back(ParityCase{columns[0], columns[1], columns[2], columns[3], columns[4]});
    }
    return cases;
}

auto Parse(const std::string_view text) -> cedar::WithWarnings<cedar::SchemaFragment>
{
    auto parsed = cedar::ParseSchemaFragment(text);
    EXPECT_TRUE(parsed.has_value()) << (parsed ? std::string{} : parsed.error().Render());
    if (!parsed) { return *cedar::ParseSchemaFragment(""); }
    return *std::move(parsed);
}

auto Code(const std::string_view text) -> DiagnosticCode
{
    const auto parsed = cedar::ParseSchemaFragment(text);
    return parsed ? DiagnosticCode{} : parsed.error().Primary().code;
}

auto Model(const cedar::SchemaFragment& fragment) -> const cedar::detail::SchemaFragmentModel&
{
    return cedar::detail::SchemaAccess::Model(fragment);
}

auto CountWarnings(const std::vector<cedar::Diagnostic>& warnings, const DiagnosticCode code) -> long
{
    return std::ranges::count_if(warnings, [&](const cedar::Diagnostic& warning) { return warning.code == code; });
}

auto Snippet(const cedar::detail::SchemaFragmentModel& model, const cedar::detail::ModelSpan& span) -> std::string
{
    if (!span.value) { return "<no span>"; }
    return model.source->text.substr(span.value->begin, span.value->Length());
}

// Every case under tests/parity/schemas/cedar against what the reference CLI's `translate-schema`
// does with it: the verdict, the JSON (byte for byte), the shadowing warnings, the resolved JSON
// (byte for byte), and the Cedar printer's verdict and text (byte for byte).
TEST(SchemaParser, MatchesTheOracleOnEveryParityCase)
{
    const std::string directory = std::string{CEDAR_SCHEMA_CEDAR_PARITY_DIRECTORY} + "/";
    const auto cases = ReadParityTable(directory + "Expected.tsv");
    ASSERT_GE(cases.size(), 40U);
    for (const ParityCase& parityCase : cases)
    {
        SCOPED_TRACE(parityCase.name);
        const std::string stem = directory + parityCase.name.substr(0, parityCase.name.size() - 12);
        const auto parsed = cedar::ParseSchemaFragment(ReadFile(directory + parityCase.name), parityCase.name);
        ASSERT_EQ(parsed.has_value(), parityCase.parse == "accept")
            << (parsed ? std::string{} : parsed.error().Render());
        if (!parsed) { continue; }

        const std::string expectedJson = ReadFile(stem + ".expected.json");
        EXPECT_EQ(cedar::ToJson(parsed->value), expectedJson);
        const auto fromJson = cedar::SchemaFragmentFromJson(expectedJson);
        ASSERT_TRUE(fromJson.has_value());
        EXPECT_TRUE(*fromJson == parsed->value);

        const std::string warnings =
            std::format("{}/{}", CountWarnings(parsed->warnings, DiagnosticCode::ShadowsBuiltin),
                        CountWarnings(parsed->warnings, DiagnosticCode::ShadowsEntity));
        EXPECT_EQ(warnings, parityCase.warnings);
        for (const cedar::Diagnostic& warning : parsed->warnings) { EXPECT_EQ(warning.severity, cedar::Severity::Warning); }

        const auto resolved = cedar::ToJsonWithResolvedTypes(parsed->value);
        ASSERT_EQ(resolved.has_value(), parityCase.resolved == "accept")
            << (resolved ? std::string{} : resolved.error().Render());
        if (resolved) { EXPECT_EQ(*resolved, ReadFile(stem + ".expected-resolved.json")); }

        const auto printed = cedar::ToCedar(parsed->value);
        if (parityCase.printed == "accept")
        {
            ASSERT_TRUE(printed.has_value()) << printed.error().Render();
            EXPECT_EQ(*printed, ReadFile(stem + ".expected-printed.txt"));
            // What the printer writes reads back.
            EXPECT_TRUE(cedar::ParseSchemaFragment(*printed).has_value()) << *printed;
        }
        else
        {
            ASSERT_FALSE(printed.has_value());
            EXPECT_EQ(printed.error().Primary().code, parityCase.printed == "collision"
                                                          ? DiagnosticCode::SchemaNameCollisions
                                                          : DiagnosticCode::UnconvertibleEntityTypeShape);
        }
    }
}

TEST(SchemaParser, BuildsTheModel)
{
    const auto parsed = Parse(R"(
        @doc("ns") namespace App {
          type Context = { ip?: ipaddr };
          @doc entity User in [Group] { name: String } tags Set<Long>;
          entity Group;
          action read in [Action::"all", "x"] appliesTo { principal: User, resource: [Group], context: Context };
          external action legacy;
          action all;
          action x;
        }
        entity Top;
    )");
    const auto& model = Model(parsed.value);
    ASSERT_EQ(model.namespaces.size(), 2U);
    EXPECT_FALSE(model.namespaces[0].name.has_value());
    const auto& app = model.namespaces[1];
    ASSERT_TRUE(app.name.has_value());
    EXPECT_EQ(app.name->ToString(), "App");
    EXPECT_EQ(app.annotations.Find("doc"), "ns");

    ASSERT_EQ(app.entityTypes.size(), 2U);
    const auto& user = app.entityTypes[1];
    EXPECT_EQ(user.name.Text(), "User");
    // `@doc` without a value is the empty string, as Rust's Cedar-syntax reader stores it.
    ASSERT_EQ(user.annotations.Size(), 1U);
    EXPECT_EQ(user.annotations.begin()->value, std::optional<std::string>{""});
    const auto& standard = std::get<cedar::detail::StandardEntityType>(user.kind);
    ASSERT_EQ(standard.memberOfTypes.size(), 1U);
    EXPECT_EQ(standard.memberOfTypes[0].name.ToString(), "Group");
    ASSERT_TRUE(standard.tags.has_value());
    EXPECT_TRUE(std::holds_alternative<schema_type::Set>(standard.tags->kind));
    const auto& shape = std::get<schema_type::Record>(standard.shape.kind);
    ASSERT_EQ(shape.attributes.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<schema_type::EntityOrCommon>(shape.attributes[0].type->kind));

    ASSERT_EQ(app.actions.size(), 4U);
    const auto& legacy = app.actions[1];
    EXPECT_TRUE(legacy.external);
    EXPECT_FALSE(legacy.appliesTo.has_value());
    EXPECT_FALSE(legacy.memberOf.has_value());
    const auto& read = app.actions[2];
    ASSERT_TRUE(read.appliesTo.has_value());
    // A named context is a common-type reference, not an entity-or-common one.
    EXPECT_TRUE(std::holds_alternative<schema_type::CommonTypeReference>(read.appliesTo->context.kind));
    ASSERT_TRUE(read.memberOf.has_value());
    ASSERT_EQ(read.memberOf->size(), 2U);
    EXPECT_EQ((*read.memberOf)[0].type->name.ToString(), "Action");
    EXPECT_FALSE((*read.memberOf)[1].type.has_value());
    // An action without `appliesTo` applies to nothing, which is not the same as absent.
    ASSERT_TRUE(app.actions[0].appliesTo.has_value());
    EXPECT_TRUE(app.actions[0].appliesTo->principalTypes.empty());

    // Spans point at the declaration, the type and the reference.
    EXPECT_EQ(Snippet(model, app.entityTypes[0].span), "entity Group;");
    EXPECT_EQ(Snippet(model, standard.memberOfTypes[0].span), "Group");
    EXPECT_EQ(Snippet(model, standard.tags->span), "Set<Long>");
    EXPECT_EQ(Snippet(model, (*read.memberOf)[0].span), "Action::\"all\"");
    EXPECT_EQ(Snippet(model, app.commonTypes[0].span), "type Context = { ip?: ipaddr };");
}

TEST(SchemaParser, ReportsTheShadowingWarnings)
{
    const auto parsed = Parse("entity ipaddr; type decimal = Long; namespace N { entity A; type A = String; }");
    EXPECT_EQ(CountWarnings(parsed.warnings, DiagnosticCode::ShadowsBuiltin), 2);
    EXPECT_EQ(CountWarnings(parsed.warnings, DiagnosticCode::ShadowsEntity), 1);
    EXPECT_TRUE(Parse("entity Boolean; entity Set; type Bools = Long;").warnings.empty());
}

TEST(SchemaParser, RefusesWithTheRightCode)
{
    EXPECT_EQ(Code("entity A"), DiagnosticCode::UnexpectedEndOfInput);
    EXPECT_EQ(Code("entity A }"), DiagnosticCode::UnexpectedToken);
    EXPECT_EQ(Code("entity A & B;"), DiagnosticCode::InvalidToken);
    EXPECT_EQ(Code("entity true;"), DiagnosticCode::ReservedIdentifier);
    EXPECT_EQ(Code("entity A enum [\"\\q\"];"), DiagnosticCode::InvalidEscape);
    EXPECT_EQ(Code("@a @a entity A;"), DiagnosticCode::DuplicateAnnotation);
    EXPECT_EQ(Code("entity A; entity A;"), DiagnosticCode::DuplicateDeclarations);
    EXPECT_EQ(Code("namespace N {} namespace N {}"), DiagnosticCode::DuplicateNamespaces);
    EXPECT_EQ(Code("type String = Long;"), DiagnosticCode::ReservedSchemaKeyword);
    EXPECT_EQ(Code("entity __cedar;"), DiagnosticCode::ReservedNamespace);
    EXPECT_EQ(Code("namespace __cedar {}"), DiagnosticCode::ReservedNamespace);
    EXPECT_EQ(Code("action a appliesTo { principal: [], resource: A };"), DiagnosticCode::EmptyList);
    EXPECT_EQ(Code("action a appliesTo { resource: A };"), DiagnosticCode::NoPrincipalOrResource);
    EXPECT_EQ(Code("action a appliesTo { principal: A, resource: A, resource: A };"),
              DiagnosticCode::DuplicatePrincipalOrResource);
    EXPECT_EQ(Code("action a appliesTo { context: {}, context: {}, principal: A, resource: A };"),
              DiagnosticCode::DuplicateContext);
    EXPECT_EQ(Code("entity Action;"), DiagnosticCode::ActionEntityTypeDeclared);
}

TEST(SchemaParser, LimitsTypeNesting)
{
    const auto nested = [](const std::size_t depth)
    {
        std::string text = "type T = ";
        for (std::size_t level = 0; level < depth; ++level) { text += level % 2 == 0 ? "Set<" : "{ a: "; }
        text += "Long";
        for (std::size_t level = depth; level > 0; --level) { text += (level - 1) % 2 == 0 ? ">" : " }"; }
        return text + ";";
    };
    const auto deepest = cedar::ParseSchemaFragment(nested(cedar::detail::MaximumSchemaTypeDepth - 1));
    ASSERT_TRUE(deepest.has_value()) << deepest.error().Render();
    EXPECT_FALSE(cedar::ToJson(deepest->value).empty());
    ASSERT_TRUE(cedar::ToCedar(deepest->value).has_value());
    EXPECT_EQ(Code(nested(cedar::detail::MaximumSchemaTypeDepth + 1)), DiagnosticCode::NestingTooDeep);
}

// The printers on what only the JSON format can say: `@k` with a null value, the explicit
// built-in, `Entity` and `Extension` types, `additionalAttributes`, and a shape that is not a record.
TEST(SchemaParser, PrintsJsonOnlyForms)
{
    const auto fragment = cedar::SchemaFragmentFromJson(R"({"N": {
        "entityTypes": {"E": {"shape": {"type": "Record", "additionalAttributes": true, "attributes": {
            "b": {"type": "Boolean"}, "e": {"type": "Entity", "name": "E"},
            "x": {"type": "Extension", "name": "ipaddr"}, "s": {"type": "String", "required": false}}}}},
        "actions": {"a": {"memberOf": [], "annotations": {"k": null}}}}})");
    ASSERT_TRUE(fragment.has_value()) << fragment.error().Render();
    const auto printed = cedar::ToCedar(*fragment);
    ASSERT_TRUE(printed.has_value()) << printed.error().Render();
    EXPECT_EQ(*printed, "namespace N {\n"
                        "  entity E = {\n"
                        "    b: __cedar::Bool,\n"
                        "    e: E,\n"
                        "    s?: __cedar::String,\n"
                        "    x: __cedar::ipaddr\n"
                        "  };\n"
                        "\n"
                        "  @k\n"
                        "  action \"a\";\n"
                        "}\n");
    EXPECT_EQ(cedar::ToJson(*fragment),
              R"({"N":{"entityTypes":{"E":{"shape":{"type":"Record","attributes":{"b":{"type":"Boolean"},)"
              R"("e":{"type":"Entity","name":"E"},"s":{"type":"String","required":false},)"
              R"("x":{"type":"Extension","name":"ipaddr"}},"additionalAttributes":true}}},)"
              R"("actions":{"a":{"memberOf":[],"annotations":{"k":null}}}}})");

    const auto unconvertible = cedar::SchemaFragmentFromJson(
        R"({"": {"commonTypes": {"S": {"type": "Record", "attributes": {}}}, "entityTypes": {"E": {"shape": {"type": "S"}}}, "actions": {}}})");
    ASSERT_TRUE(unconvertible.has_value());
    const auto refused = cedar::ToCedar(*unconvertible);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().Primary().code, DiagnosticCode::UnconvertibleEntityTypeShape);
}

TEST(SchemaParser, ResolvesReferences)
{
    const auto parsed = Parse(R"(
        entity Shared;
        namespace N { entity Local; type Name = String;
          entity E { s: Shared, l: Local, n: Name, b: Bool };
          action a in ["b"] appliesTo { principal: Local, resource: Shared };
          action b; })");
    const auto resolved = cedar::ToJsonWithResolvedTypes(parsed.value);
    ASSERT_TRUE(resolved.has_value()) << resolved.error().Render();
    EXPECT_NE(resolved->find(R"("name": "Shared")"), std::string::npos);
    EXPECT_NE(resolved->find(R"("name": "N::Local")"), std::string::npos);
    EXPECT_NE(resolved->find(R"("type": "N::Name")"), std::string::npos);
    EXPECT_NE(resolved->find(R"("type": "Bool")"), std::string::npos);
    EXPECT_NE(resolved->find(R"("type": "N::Action")"), std::string::npos);

    const auto undefined = cedar::ToJsonWithResolvedTypes(Parse("entity E { a: Missing };").value);
    ASSERT_FALSE(undefined.has_value());
    EXPECT_EQ(undefined.error().Primary().code, DiagnosticCode::TypeNotDefined);
    const auto undefinedAction = cedar::ToJsonWithResolvedTypes(Parse("action a in b;").value);
    ASSERT_FALSE(undefinedAction.has_value());
    EXPECT_EQ(undefinedAction.error().Primary().code, DiagnosticCode::ActionNotDefined);
}

} // namespace
