#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace
{

using cedar::detail::ValidatorSchema;
namespace types = cedar::detail::types;

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
    std::string path;
};

/// A table from scripts/generate-schema-link-parity-table.sh: `accept|reject <TAB> path`. A
/// `.cedarschema` path is read in Cedar syntax, any other as JSON.
auto ReadParityTable(const std::string& path) -> std::vector<ParityCase>
{
    std::vector<ParityCase> cases;
    std::istringstream table{ReadFile(path)};
    std::string line;
    while (std::getline(table, line))
    {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) { continue; }
        cases.push_back(ParityCase{line.substr(0, tab) == "accept", line.substr(tab + 1)});
    }
    return cases;
}

void ExpectOracleAgreement(const std::vector<ParityCase>& cases, const std::string& prefix)
{
    for (const ParityCase& parityCase : cases)
    {
        const std::string text = ReadFile(prefix + parityCase.path);
        if (parityCase.path.ends_with(".cedarschema"))
        {
            const auto schema = cedar::ParseSchema(text, parityCase.path);
            EXPECT_EQ(schema.has_value(), parityCase.accepted)
                << parityCase.path << (schema ? std::string{} : "\n" + schema.error().Render());
            continue;
        }
        const auto schema = cedar::SchemaFromJson(text, parityCase.path);
        EXPECT_EQ(schema.has_value(), parityCase.accepted)
            << parityCase.path << (schema ? std::string{} : "\n" + schema.error().Render());
    }
}

auto Link(const std::string_view json) -> cedar::Schema
{
    auto schema = cedar::SchemaFromJson(json);
    EXPECT_TRUE(schema.has_value()) << (schema ? std::string{} : schema.error().Render());
    if (!schema) { return *cedar::SchemaFromJson("{}"); }
    return *std::move(schema);
}

auto Validator(const cedar::Schema& schema) -> const ValidatorSchema& { return cedar::detail::SchemaAccess::Validator(schema); }

auto Code(const std::string_view json) -> cedar::DiagnosticCode
{
    const auto schema = cedar::SchemaFromJson(json);
    return schema ? cedar::DiagnosticCode{} : schema.error().Primary().code;
}

auto Uid(const std::string_view type, const std::string_view id) -> cedar::EntityUid
{
    return *cedar::EntityUid::Create(type, id);
}

auto Type(const std::string_view name) -> cedar::EntityType { return *cedar::EntityType::Parse(name); }

template <typename Element>
auto Text(const std::vector<Element>& elements) -> std::string
{
    std::string text;
    for (const Element& element : elements)
    {
        if (!text.empty()) { text += ","; }
        if constexpr (std::is_same_v<Element, cedar::EntityUid>) { text += element.ToString(); }
        else { text += element.GetName().QualifiedText(); }
    }
    return text;
}

// Every case under tests/parity/schemas/link, accepted or rejected as the reference CLI's
// `check-parse --schema` links it.
TEST(SchemaLinker, MatchesTheOracleOnEveryParityCase)
{
    const std::string directory = CEDAR_SCHEMA_LINK_PARITY_DIRECTORY;
    const auto cases = ReadParityTable(directory + "/Expected.tsv");
    ASSERT_GE(cases.size(), 40U);
    ExpectOracleAgreement(cases, directory + "/");
}

// A table over the same generated corpus of schema fragments, written with absolute paths by the
// same script. CEDAR_SCHEMA_LINK_SWEEP_TABLE names the table; the build sets it when the reference
// checkout is configured.
TEST(SchemaLinker, MatchesTheOracleOnTheSweepTable)
{
    const char* table = std::getenv("CEDAR_SCHEMA_LINK_SWEEP_TABLE");
    if (table == nullptr) { GTEST_SKIP() << "CEDAR_SCHEMA_LINK_SWEEP_TABLE is not set"; }
    const auto cases = ReadParityTable(table);
    ASSERT_FALSE(cases.empty());
    ExpectOracleAgreement(cases, "");
}

TEST(SchemaLinker, ClosesTheActionHierarchy)
{
    const cedar::Schema schema = Link(R"({"NS": {"entityTypes": {"User": {}, "Photo": {}}, "actions": {
        "view": {"memberOf": [{"id": "read"}], "appliesTo": {"principalTypes": ["User"], "resourceTypes": ["Photo"]}},
        "read": {"memberOf": [{"id": "all"}]},
        "all": {}}}})");
    const ValidatorSchema& linked = Validator(schema);
    const auto* view = linked.FindAction(Uid("NS::Action", "view"));
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(Text(view->ancestors), R"(NS::Action::"all",NS::Action::"read")");
    EXPECT_TRUE(view->descendants.empty());
    const auto* all = linked.FindAction(Uid("NS::Action", "all"));
    ASSERT_NE(all, nullptr);
    EXPECT_EQ(Text(all->descendants), R"(NS::Action::"read",NS::Action::"view")");
    ASSERT_TRUE(view->context.has_value());
    EXPECT_EQ(*view->context, cedar::detail::EmptyRecord());

    const auto entities = schema.ActionEntities();
    ASSERT_TRUE(entities.has_value()) << entities.error().Render();
    EXPECT_EQ(entities->Size(), 3U);
    const cedar::Entity* viewEntity = entities->Find(Uid("NS::Action", "view"));
    ASSERT_NE(viewEntity, nullptr);
    EXPECT_TRUE(viewEntity->IsDescendantOf(Uid("NS::Action", "all")));
    EXPECT_EQ(viewEntity, view->entity.get()) << "the store shares the schema's action entities";

    EXPECT_EQ(Text(schema.ActionGroups()), R"(NS::Action::"all",NS::Action::"read")");
    const auto environments = schema.RequestEnvironments();
    ASSERT_EQ(environments.size(), 1U);
    EXPECT_EQ(environments[0].action, Uid("NS::Action", "view"));
    EXPECT_EQ(environments[0].principal, Type("NS::User"));
    EXPECT_TRUE(schema.IsComplete());
}

TEST(SchemaLinker, InlinesCommonTypesAndResolvesNames)
{
    const cedar::Schema schema = Link(R"({
        "": {"entityTypes": {"Group": {}}, "actions": {}},
        "NS": {"commonTypes": {"A": {"type": "B"}, "B": {"type": "Record", "attributes": {
                   "x": {"type": "Long"}, "y": {"type": "Set", "element": {"type": "ipaddr"}, "required": false}}}},
               "entityTypes": {
                   "Long": {},
                   "User": {"memberOfTypes": ["Group"], "shape": {"type": "Record", "attributes": {
                       "a": {"type": "A"}, "n": {"type": "EntityOrCommon", "name": "Long"},
                       "s": {"type": "EntityOrCommon", "name": "String"}}}}},
               "actions": {}}})");
    const ValidatorSchema& linked = Validator(schema);
    const auto* user = linked.FindEntityType(Type("NS::User"));
    ASSERT_NE(user, nullptr);
    EXPECT_EQ(cedar::detail::ToString(cedar::detail::Type{types::Record{user->attributes}}),
              "{a: {x: Long, y?: Set<ipaddr>}, n: NS::Long, s: String}");
    const auto* a = user->attributes.Find("a");
    ASSERT_NE(a, nullptr);
    const auto* record = cedar::detail::AsRecord(*a->type);
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(record->attributes.Find("x")->required);
    EXPECT_FALSE(record->attributes.Find("y")->required);
    EXPECT_EQ(Text(user->ancestors), "Group");
    EXPECT_EQ(Text(linked.FindEntityType(Type("Group"))->descendants), "NS::User");
    EXPECT_EQ(Text(*schema.Ancestors(Type("NS::User"))), "Group");
    EXPECT_FALSE(schema.Ancestors(Type("Ghost")).has_value());
    EXPECT_EQ(Text(schema.EntityTypes()), "Group,NS::Long,NS::User");
}

TEST(SchemaLinker, AllowsEntityTypeCyclesButNotActionCycles)
{
    const cedar::Schema schema =
        Link(R"({"": {"entityTypes": {"A": {"memberOfTypes": ["B"]}, "B": {"memberOfTypes": ["A"]}}, "actions": {}}})");
    EXPECT_EQ(Text(Validator(schema).FindEntityType(Type("A"))->descendants), "A,B");
    EXPECT_EQ(Code(R"({"": {"entityTypes": {}, "actions": {"a": {"memberOf": [{"id": "a"}]}}}})"),
              cedar::DiagnosticCode::CycleInActionHierarchy);
    EXPECT_EQ(Code(R"({"": {"commonTypes": {"A": {"type": "A"}}, "entityTypes": {}, "actions": {}}})"),
              cedar::DiagnosticCode::CycleInCommonTypeReferences);
    EXPECT_EQ(Code(R"({"": {"entityTypes": {"U": {"shape": {"type": "Ghost"}}}, "actions": {}}})"),
              cedar::DiagnosticCode::TypeNotDefined);
    EXPECT_EQ(Code(R"({"": {"entityTypes": {"U": {}}, "actions": {}}, "N": {"entityTypes": {"U": {}}, "actions": {}}})"),
              cedar::DiagnosticCode::TypeShadowing);
}

TEST(SchemaLinker, LinksAcrossFragments)
{
    const auto first = cedar::SchemaFragmentFromJson(R"({"NS": {"entityTypes": {"User": {}}, "actions": {}}})");
    const auto second = cedar::SchemaFragmentFromJson(
        R"({"NS": {"entityTypes": {}, "actions": {"view": {"appliesTo": {"principalTypes": ["User"], "resourceTypes": ["User"]}}}}})");
    const auto duplicate = cedar::SchemaFragmentFromJson(R"({"NS": {"entityTypes": {"User": {}}, "actions": {}}})");
    ASSERT_TRUE(first && second && duplicate);

    const std::array linkable{*first, *second};
    const auto schema = cedar::Schema::FromFragments(linkable);
    ASSERT_TRUE(schema.has_value()) << schema.error().Render();
    EXPECT_EQ(Text(*schema->PrincipalsForAction(Uid("NS::Action", "view"))), "NS::User");
    EXPECT_EQ(Text(schema->ActionsForPrincipalAndResource(Type("NS::User"), Type("NS::User"))), R"(NS::Action::"view")");

    const std::array repeated{*first, *duplicate};
    const auto refused = cedar::Schema::FromFragments(repeated);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().Primary().code, cedar::DiagnosticCode::DuplicateEntityType);

    const std::array alone{*second};
    EXPECT_EQ(cedar::Schema::FromFragments(alone).error().Primary().code, cedar::DiagnosticCode::TypeNotDefined);
}

TEST(SchemaLinker, ReportsExternalDeclarations)
{
    const cedar::Schema schema = Link(R"({"": {"entityTypes": {"User": {"external": true}}, "actions": {
        "view": {"external": true}, "edit": {"appliesTo": {"principalTypes": ["User"], "resourceTypes": ["User"]}}}}})");
    EXPECT_FALSE(schema.IsComplete());
    const auto entities = schema.ActionEntities();
    ASSERT_TRUE(entities.has_value());
    EXPECT_EQ(entities->Size(), 1U) << "an external action has no entity";
    EXPECT_EQ(schema.RequestEnvironments().size(), 1U);
}

} // namespace
