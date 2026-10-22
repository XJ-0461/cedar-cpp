#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Capabilities.hpp"
#include "validation/Typechecker.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using cedar::detail::PolicyCheckOutcome;
using cedar::detail::TypecheckEnvironment;
using cedar::detail::Typechecker;

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

auto SplitTabs(const std::string& line) -> std::vector<std::string>
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) { return fields; }
        start = tab + 1;
    }
}

auto ReadTable(const std::string& path) -> std::vector<std::vector<std::string>>
{
    std::istringstream table{ReadFile(path)};
    std::vector<std::vector<std::string>> rows;
    std::string line;
    std::getline(table, line);
    while (std::getline(table, line))
    {
        if (!line.empty()) { rows.push_back(SplitTabs(line)); }
    }
    return rows;
}

// The validation kinds the validator driver reports from the scope and entity checks rather than
// the typechecker (Rust's `rbac.rs`): the oracle's expectations include them, the typechecker's
// output does not.
const std::set<std::string> DriverKinds{
    "validation.unrecognized-entity-type",
    "validation.unrecognized-action-id",
    "validation.invalid-enum-entity",
    "validation.invalid-action-application",
};

auto ParseKinds(const std::string& text) -> std::set<std::string>
{
    std::set<std::string> kinds;
    if (text == "-") { return kinds; }
    std::size_t start = 0;
    while (true)
    {
        const std::size_t comma = text.find(',', start);
        kinds.insert(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) { return kinds; }
        start = comma + 1;
    }
}

auto JoinKinds(const std::set<std::string>& kinds) -> std::string
{
    std::string text;
    for (const std::string& kind : kinds) { text += (text.empty() ? "" : ",") + kind; }
    return text.empty() ? "-" : text;
}

auto SlotType(const std::string& text) -> std::optional<cedar::EntityType>
{
    if (text == "-") { return std::nullopt; }
    return cedar::EntityType::Parse(text).value();
}

// Each case's schema has one request environment, so the typechecker's result in it is the
// policy's validation result, minus the driver's kinds.
TEST(Typechecker, ParityTable)
{
    const std::string directory = CEDAR_VALIDATION_PARITY_DIRECTORY;
    const auto cases = ReadTable(directory + "/Cases.tsv");
    const auto expectations = ReadTable(directory + "/Expected.tsv");
    ASSERT_EQ(cases.size(), expectations.size());
    std::map<std::string, cedar::Schema> schemas;
    std::size_t checked = 0;
    for (std::size_t row = 0; row < cases.size(); ++row)
    {
        const auto& input = cases[row];
        const auto& expected = expectations[row];
        ASSERT_EQ(input.size(), 6U);
        ASSERT_EQ(expected.size(), 3U);
        const std::string& name = input[0];
        ASSERT_EQ(name, expected[0]);
        if (!schemas.contains(input[1]))
        {
            auto parsed = cedar::ParseSchema(ReadFile(directory + "/" + input[1]), input[1]);
            ASSERT_TRUE(parsed.has_value()) << input[1] << "\n" << parsed.error().Render();
            schemas.emplace(input[1], parsed->value);
        }
        const auto& schema = cedar::detail::SchemaAccess::Validator(schemas.at(input[1]));
        ASSERT_EQ(schema.RequestEnvironments().size(), 1U) << input[1];
        const auto mode = input[2] == "strict" ? cedar::ValidationMode::Strict : cedar::ValidationMode::Permissive;
        TypecheckEnvironment environment = TypecheckEnvironment::FromUnlinked(schema.RequestEnvironments().front());
        environment.principalSlot = SlotType(input[3]);
        environment.resourceSlot = SlotType(input[4]);

        const auto policies = cedar::ParsePolicySet(input[5]);
        ASSERT_TRUE(policies.has_value()) << name << "\n" << policies.error().Render();
        const cedar::Template* body = nullptr;
        if (!policies->Templates().empty()) { body = policies->Templates().front().get(); }
        else if (!policies->Policies().empty()) { body = &policies->Policies().front()->GetTemplate(); }
        ASSERT_NE(body, nullptr) << name;

        const Typechecker typechecker{schema, mode, *body};
        const auto result = typechecker.Check(body->Condition(), environment);
        std::set<std::string> kinds;
        for (const cedar::Diagnostic& error : result.errors)
        {
            kinds.insert(std::string{cedar::CodeName(error.code)});
            EXPECT_EQ(error.severity, cedar::Severity::Error) << name;
        }
        if (result.outcome == PolicyCheckOutcome::Irrelevant) { kinds.insert("validation.impossible-policy"); }

        std::set<std::string> expectedKinds = ParseKinds(expected[2]);
        bool driverError = false;
        for (const std::string& kind : DriverKinds)
        {
            if (expectedKinds.erase(kind) > 0 && kind != "validation.invalid-action-application") { driverError = true; }
        }
        EXPECT_EQ(JoinKinds(kinds), JoinKinds(expectedKinds)) << name << ": " << input[5];
        const bool passes = result.errors.empty() && !driverError;
        EXPECT_EQ(passes ? "pass" : "fail", expected[1]) << name << ": " << input[5];
        ++checked;
    }
    EXPECT_GE(checked, 150U);
}

// A shape names an attribute chain the same way whether it is written out or reached through a
// `has` path.
TEST(Typechecker, CapabilityShapes)
{
    const auto expression = cedar::ParseExpression("[principal.manager.age, principal.manager, principal]").value();
    const auto& arena = expression.Arena();
    const cedar::detail::ExpressionShapes shapes{arena};
    const auto* set = std::get_if<cedar::node::SetLiteral>(&arena.Node(expression.Root()));
    ASSERT_NE(set, nullptr);
    const auto elements = arena.Elements(set->elements);
    const std::vector<std::string_view> managerAge{"manager", "age"};
    const std::vector<std::string_view> manager{"manager"};
    EXPECT_TRUE(shapes.Equal(elements[0], {}, elements[2], managerAge));
    EXPECT_TRUE(shapes.Equal(elements[1], std::span{managerAge}.first(0), elements[2], manager));
    EXPECT_FALSE(shapes.Equal(elements[0], {}, elements[2], manager));
    EXPECT_EQ(shapes.Hash(elements[0]), shapes.Hash(elements[2], managerAge));
    const auto written = cedar::detail::Capability::ForAttribute(shapes, elements[1], {}, "age");
    const auto reached = cedar::detail::Capability::ForAttribute(shapes, elements[2], manager, "age");
    EXPECT_TRUE(cedar::detail::CapabilitySet::Singleton(written).Contains(reached, shapes));
    const auto other = cedar::detail::Capability::ForAttribute(shapes, elements[2], {}, "age");
    EXPECT_FALSE(cedar::detail::CapabilitySet::Singleton(written).Contains(other, shapes));
}

// A condition nested past the depth limit ends in the recursion-limit answer instead of
// exhausting the stack, and reports nothing, as Rust's typechecker does when its stack runs low.
TEST(Typechecker, DeepConditionStopsAtTheLimit)
{
    const std::string directory = CEDAR_VALIDATION_PARITY_DIRECTORY;
    const auto parsed = cedar::ParseSchema(ReadFile(directory + "/Main.cedarschema")).value();
    const auto& schema = cedar::detail::SchemaAccess::Validator(parsed.value);
    const auto environment = TypecheckEnvironment::FromUnlinked(schema.RequestEnvironments().front());
    std::string condition = "1";
    for (std::uint32_t term = 0; term < cedar::detail::MaximumTypecheckDepth + 100; ++term)
    {
        condition += " && principal.active";
    }
    const auto policies = cedar::ParsePolicySet("permit(principal, action, resource) when { " + condition + " };");
    ASSERT_TRUE(policies.has_value());
    const cedar::Template& body = policies->Policies().front()->GetTemplate();
    const Typechecker typechecker{schema, cedar::ValidationMode::Strict, body};
    const auto result = typechecker.Check(body.Condition(), environment);
    EXPECT_EQ(result.outcome, PolicyCheckOutcome::Fail);
    EXPECT_TRUE(result.errors.empty());
}

} // namespace
