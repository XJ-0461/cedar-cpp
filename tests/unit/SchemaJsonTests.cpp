#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{

namespace schema_type = cedar::detail::schema_type;

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

/// A table from scripts/generate-schema-json-parity-table.sh: `accept|reject <TAB> path`.
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
        const auto fragment = cedar::SchemaFragmentFromJson(text, parityCase.path);
        EXPECT_EQ(fragment.has_value(), parityCase.accepted)
            << parityCase.path << (fragment ? std::string{} : "\n" + fragment.error().Render());
    }
}

auto Read(const std::string_view json) -> cedar::SchemaFragment
{
    auto fragment = cedar::SchemaFragmentFromJson(json);
    EXPECT_TRUE(fragment.has_value()) << (fragment ? std::string{} : fragment.error().Render());
    if (!fragment) { return *cedar::SchemaFragmentFromJson("{}"); }
    return *std::move(fragment);
}

auto Model(const cedar::SchemaFragment& fragment) -> const cedar::detail::SchemaFragmentModel&
{
    return cedar::detail::SchemaAccess::Model(fragment);
}

auto Code(const std::string_view json) -> cedar::DiagnosticCode
{
    const auto fragment = cedar::SchemaFragmentFromJson(json);
    return fragment ? cedar::DiagnosticCode{} : fragment.error().Primary().code;
}

auto Names(const std::vector<cedar::detail::TypeName>& names) -> std::string
{
    std::string text;
    for (const auto& name : names) { text += (text.empty() ? "" : ",") + name.name.ToString(); }
    return text;
}

// Every case under tests/parity/schemas/json, accepted or rejected as the reference CLI's
// `translate-schema --direction json-to-cedar` reads it.
TEST(SchemaJson, MatchesTheOracleOnEveryParityCase)
{
    const std::string directory = CEDAR_SCHEMA_JSON_PARITY_DIRECTORY;
    const auto cases = ReadParityTable(directory + "/Expected.tsv");
    ASSERT_GE(cases.size(), 100U);
    ExpectOracleAgreement(cases, directory + "/");
}

// A table over schema files outside the repository (the reference checkout, the integration
// corpus's schemas converted to JSON), written with absolute paths by the same script. Run with
// CEDAR_SCHEMA_JSON_SWEEP_TABLE set to the table.
TEST(SchemaJson, MatchesTheOracleOnTheSweepTable)
{
    const char* table = std::getenv("CEDAR_SCHEMA_JSON_SWEEP_TABLE");
    if (table == nullptr) { GTEST_SKIP() << "CEDAR_SCHEMA_JSON_SWEEP_TABLE is not set"; }
    const auto cases = ReadParityTable(table);
    ASSERT_FALSE(cases.empty());
    ExpectOracleAgreement(cases, "");
}

TEST(SchemaJson, ReadsTheModel)
{
    const auto fragment = Read(ReadFile(std::string{CEDAR_SCHEMA_JSON_PARITY_DIRECTORY} + "/accept-full-featured.json"));
    const auto& model = Model(fragment);
    ASSERT_EQ(model.namespaces.size(), 3U);
    EXPECT_FALSE(model.namespaces[0].name.has_value());
    EXPECT_EQ(model.namespaces[1].name->ToString(), "NS::Inner");
    EXPECT_EQ(model.namespaces[2].name->ToString(), "NS");
    EXPECT_EQ(model.namespaces[2].annotations.Find("doc"), "namespace");

    const auto& empty = model.namespaces[0];
    ASSERT_EQ(empty.commonTypes.size(), 1U);
    EXPECT_EQ(empty.commonTypes[0].name.Text(), "Address");
    EXPECT_EQ(empty.commonTypes[0].annotations.Find("doc"), "x");
    ASSERT_EQ(empty.entityTypes.size(), 3U);
    EXPECT_EQ(empty.entityTypes[0].name.Text(), "Color");
    EXPECT_EQ(std::get<cedar::detail::EnumEntityType>(empty.entityTypes[0].kind).choices.size(), 2U);

    const auto& user = empty.entityTypes[2];
    ASSERT_EQ(user.name.Text(), "User");
    const auto& standard = std::get<cedar::detail::StandardEntityType>(user.kind);
    EXPECT_EQ(Names(standard.memberOfTypes), "Group");
    ASSERT_TRUE(standard.tags.has_value());
    EXPECT_TRUE(std::holds_alternative<schema_type::String>(standard.tags->kind));
    const auto& shape = std::get<schema_type::Record>(standard.shape.kind);
    EXPECT_TRUE(shape.additionalAttributes);
    std::string attributes;
    for (const auto& attribute : shape.attributes) { attributes += attribute.name + (attribute.required ? "" : "?") + " "; }
    EXPECT_EQ(attributes, "address admin age? groups home name other ");
    EXPECT_TRUE(std::holds_alternative<schema_type::CommonTypeReference>(shape.attributes[0].type->kind));
    EXPECT_EQ(shape.attributes[1].annotations.Find("doc"), "is admin");
    const auto& groups = std::get<schema_type::Set>(shape.attributes[3].type->kind);
    EXPECT_EQ(std::get<schema_type::Entity>(groups.element->kind).name.name.ToString(), "Group");
    EXPECT_EQ(std::get<schema_type::Extension>(shape.attributes[4].type->kind).name.Text(), "ipaddr");
    EXPECT_EQ(std::get<schema_type::EntityOrCommon>(shape.attributes[6].type->kind).name.name.ToString(), "NS::Thing");

    ASSERT_EQ(empty.actions.size(), 2U);
    EXPECT_EQ(empty.actions[0].name, "read");
    EXPECT_FALSE(empty.actions[0].appliesTo.has_value());
    EXPECT_FALSE(empty.actions[0].memberOf.has_value());
    const auto& view = empty.actions[1];
    ASSERT_TRUE(view.appliesTo.has_value());
    EXPECT_EQ(Names(view.appliesTo->principalTypes), "User");
    EXPECT_EQ(Names(view.appliesTo->resourceTypes), "Group");
    EXPECT_FALSE(cedar::detail::IsEmptyRecord(view.appliesTo->context));
    ASSERT_TRUE(view.memberOf.has_value());
    ASSERT_EQ(view.memberOf->size(), 2U);
    EXPECT_EQ((*view.memberOf)[0].id, "read");
    EXPECT_FALSE((*view.memberOf)[0].type.has_value());
    EXPECT_EQ((*view.memberOf)[1].type->name.ToString(), "NS::Action");
    ASSERT_EQ(view.annotations.Size(), 1U);
    EXPECT_FALSE(view.annotations.Entries()[0].value.has_value());

    const auto& ns = model.namespaces[2];
    EXPECT_TRUE(ns.entityTypes[0].external);
    EXPECT_TRUE(ns.actions[1].external);
}

// Rust's `BTreeMap<Option<Name>, _>`: the empty namespace first, then Name order (basename, then
// the namespace path).
TEST(SchemaJson, OrdersNamespacesAsRust)
{
    const auto fragment = Read(R"({"B": {"entityTypes": {}, "actions": {}}, "Z::A": {"entityTypes": {}, "actions": {}},
                                  "": {"entityTypes": {}, "actions": {}}, "A::B": {"entityTypes": {}, "actions": {}}})");
    std::string order;
    for (const auto& definition : Model(fragment).namespaces)
    {
        order += (definition.name ? definition.name->ToString() : std::string{"<empty>"}) + " ";
    }
    EXPECT_EQ(order, "<empty> Z::A B A::B ");
}

TEST(SchemaJson, ComparesStructurally)
{
    const auto base = Read(R"({"": {"entityTypes": {"A": {"memberOfTypes": ["B"]}, "B": {}},
        "actions": {"a": {"appliesTo": {"principalTypes": ["A"], "resourceTypes": ["B"]}}}}})");
    const auto copy = base;
    EXPECT_TRUE(base == copy);

    // Key order, defaults written out, and spacing make no difference.
    EXPECT_TRUE(base == Read(R"({"": {"actions": {"a": {"appliesTo": {"resourceTypes": ["B"], "principalTypes": ["A"],
        "context": {"type": "Record", "attributes": {}, "additionalAttributes": false}}, "external": false}},
        "commonTypes": {}, "annotations": {}, "entityTypes": {"B": {"shape": {"type": "Record", "attributes": {}}},
        "A": {"memberOfTypes": ["B"], "external": false, "annotations": {}}}}})"));

    // Rust keeps `memberOf` and `appliesTo` as options and the type lists in order.
    EXPECT_FALSE(base == Read(R"({"": {"entityTypes": {"A": {"memberOfTypes": ["B"]}, "B": {}},
        "actions": {"a": {"appliesTo": {"principalTypes": ["A"], "resourceTypes": ["B"]}, "memberOf": []}}}})"));
    EXPECT_FALSE(base == Read(R"({"": {"entityTypes": {"A": {"memberOfTypes": ["B"]}, "B": {}},
        "actions": {"a": {"appliesTo": {"principalTypes": ["A", "B"], "resourceTypes": ["B"]}}}}})"));
    EXPECT_FALSE(Read(R"({"": {"entityTypes": {}, "actions": {"a": {}}}})") ==
                 Read(R"({"": {"entityTypes": {}, "actions": {"a": {"appliesTo": {"principalTypes": [], "resourceTypes": []}}}}})"));
    EXPECT_FALSE(Read(R"({"": {"entityTypes": {"A": {"enum": ["a", "b"]}}, "actions": {}}})") ==
                 Read(R"({"": {"entityTypes": {"A": {"enum": ["b", "a"]}}, "actions": {}}})"));
    EXPECT_FALSE(Read(R"({"": {"entityTypes": {"A": {"annotations": {"x": null}}}, "actions": {}}})") ==
                 Read(R"({"": {"entityTypes": {"A": {"annotations": {"x": ""}}}, "actions": {}}})"));
    EXPECT_FALSE(Read(R"({"": {"entityTypes": {"A": {"shape": {"type": "Record", "attributes": {"x": {"type": "Long"}}}}}, "actions": {}}})") ==
                 Read(R"({"": {"entityTypes": {"A": {"shape": {"type": "Record", "attributes": {"x": {"type": "Long", "required": false}}}}}, "actions": {}}})"));
    // An entity reference, an either reference and a common reference are different types.
    EXPECT_FALSE(Read(R"({"": {"commonTypes": {"T": {"type": "Entity", "name": "A"}}, "entityTypes": {}, "actions": {}}})") ==
                 Read(R"({"": {"commonTypes": {"T": {"type": "EntityOrCommon", "name": "A"}}, "entityTypes": {}, "actions": {}}})"));
    EXPECT_FALSE(Read(R"({"": {"commonTypes": {"T": {"type": "A"}}, "entityTypes": {}, "actions": {}}})") ==
                 Read(R"({"": {"commonTypes": {"T": {"type": "EntityOrCommon", "name": "A"}}, "entityTypes": {}, "actions": {}}})"));
    EXPECT_FALSE(Read(R"({"": {"entityTypes": {}, "actions": {}}})") == Read("{}"));
}

TEST(SchemaJson, ReportsTheCondition)
{
    using cedar::DiagnosticCode;
    EXPECT_EQ(Code(R"({"": {"entityTypes": {}, "actions": {},}})"), DiagnosticCode::JsonSyntax);
    EXPECT_EQ(Code(R"({"": {"entityTypes": {}}})"), DiagnosticCode::JsonUnexpectedShape);
    EXPECT_EQ(Code(R"({"": {"entityTypes": {"A": {}, "A": {}}, "actions": {}}})"), DiagnosticCode::JsonUnexpectedShape);
    EXPECT_EQ(Code(R"({"N": {"entityTypes": {"Action": {}}, "actions": {}}})"), DiagnosticCode::ActionEntityTypeDeclared);
    EXPECT_EQ(Code(R"({"": {"entityTypes": {}, "actions": {"a": {"attributes": {}}}}})"),
              DiagnosticCode::UnsupportedSchemaFeature);

    // Both restrictions are reported together, as Rust collects them across namespaces.
    const auto both = cedar::SchemaFragmentFromJson(
        R"({"": {"entityTypes": {"Action": {}}, "actions": {}}, "N": {"entityTypes": {}, "actions": {"a": {"attributes": {}}}}})");
    ASSERT_FALSE(both.has_value());
    EXPECT_EQ(both.error().Diagnostics().size(), 2U);
    EXPECT_TRUE(both.error().HasCode(DiagnosticCode::ActionEntityTypeDeclared));
    EXPECT_TRUE(both.error().HasCode(DiagnosticCode::UnsupportedSchemaFeature));

    // The diagnostic points into the input.
    const auto shape = cedar::SchemaFragmentFromJson(R"({"": {"entityTypes": {"A": {"tags": null}}, "actions": {}}})", "s.json");
    ASSERT_FALSE(shape.has_value());
    ASSERT_TRUE(shape.error().Primary().primary.has_value());
    EXPECT_EQ(shape.error().Primary().primary->begin, 36U);
    EXPECT_NE(shape.error().Render().find("s.json:1:37"), std::string::npos) << shape.error().Render();
}

TEST(SchemaJson, InsertsDeclarationsInOrderOnce)
{
    std::vector<cedar::detail::ActionDeclaration> actions;
    EXPECT_TRUE(cedar::detail::InsertSorted(actions, cedar::detail::ActionDeclaration{.name = "b"}));
    EXPECT_TRUE(cedar::detail::InsertSorted(actions, cedar::detail::ActionDeclaration{.name = "a"}));
    EXPECT_FALSE(cedar::detail::InsertSorted(actions, cedar::detail::ActionDeclaration{.name = "b"}));
    ASSERT_EQ(actions.size(), 2U);
    EXPECT_EQ(actions[0].name, "a");
    const auto* found = cedar::detail::FindSorted(std::span<const cedar::detail::ActionDeclaration>{actions}, std::string{"b"});
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->name, "b");
    EXPECT_EQ(cedar::detail::FindSorted(std::span<const cedar::detail::ActionDeclaration>{actions}, std::string{"c"}), nullptr);
}

} // namespace
