#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

auto Split(const std::string& text, const char separator) -> std::vector<std::string>
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t found = text.find(separator, start);
        fields.push_back(text.substr(start, found == std::string::npos ? std::string::npos : found - start));
        if (found == std::string::npos) { return fields; }
        start = found + 1;
    }
}

// The rows of a table, each keyed by its header's column names.
auto ReadTable(const std::string& path) -> std::vector<std::map<std::string, std::string>>
{
    std::istringstream table{ReadFile(path)};
    std::string line;
    std::getline(table, line);
    const std::vector<std::string> columns = Split(line, '\t');
    std::vector<std::map<std::string, std::string>> rows;
    while (std::getline(table, line))
    {
        if (line.empty()) { continue; }
        const std::vector<std::string> fields = Split(line, '\t');
        EXPECT_EQ(fields.size(), columns.size()) << line;
        std::map<std::string, std::string> row;
        for (std::size_t column = 0; column < columns.size() && column < fields.size(); ++column)
        {
            row[columns[column]] = fields[column];
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

auto JoinKinds(const std::set<std::string>& kinds) -> std::string
{
    std::string text;
    for (const std::string& kind : kinds) { text += (text.empty() ? "" : ",") + kind; }
    return text.empty() ? "-" : text;
}

// `-`, or `template:link:slot=uid+slot=uid` entries separated by `;` (the generator script's form).
auto AddLinks(cedar::PolicySet& policies, const std::string& text) -> testing::AssertionResult
{
    if (text == "-") { return testing::AssertionSuccess(); }
    for (const std::string& entry : Split(text, ';'))
    {
        const std::size_t first = entry.find(':');
        const std::size_t second = entry.find(':', first + 1);
        const std::string templateId = entry.substr(0, first);
        const std::string linkId = entry.substr(first + 1, second - first - 1);
        cedar::SlotBindings bindings;
        for (const std::string& slot : Split(entry.substr(second + 1), '+'))
        {
            const std::size_t equals = slot.find('=');
            const std::string name = slot.substr(0, equals);
            auto uid = cedar::ParseEntityUid(slot.substr(equals + 1));
            if (!uid) { return testing::AssertionFailure() << "bad uid in " << entry; }
            bindings.Bind(name == "?principal" ? cedar::SlotId::Principal : cedar::SlotId::Resource, *uid);
        }
        if (auto linked = policies.Link(templateId, cedar::PolicyId{linkId}, bindings); !linked)
        {
            return testing::AssertionFailure() << "cannot link " << entry << ": " << linked.error().Render();
        }
    }
    return testing::AssertionSuccess();
}

// Validates every case of `cases` in `directory` with Validator::Validate (with
// Validator::ValidateWithLevel when its `level` column is not `-`) and compares the verdict and
// the set of error and warning kinds with the oracle's in `expectations`.
void CheckTable(const std::string& directory, const std::string& cases, const std::string& expectations,
                const std::size_t minimum)
{
    const auto inputs = ReadTable(directory + "/" + cases);
    const auto expected = ReadTable(directory + "/" + expectations);
    ASSERT_EQ(inputs.size(), expected.size());
    std::map<std::string, cedar::Schema> schemas;
    for (std::size_t row = 0; row < inputs.size(); ++row)
    {
        const auto& input = inputs[row];
        const std::string& name = input.at("case");
        ASSERT_EQ(name, expected[row].at("case"));
        const std::string& schemaFile = input.at("schema");
        if (!schemas.contains(schemaFile))
        {
            auto parsed = cedar::ParseSchema(ReadFile(directory + "/" + schemaFile), schemaFile);
            ASSERT_TRUE(parsed.has_value()) << schemaFile << "\n" << parsed.error().Render();
            schemas.emplace(schemaFile, parsed->value);
        }
        const std::string& policyText = input.at("policy");
        auto policies = cedar::ParsePolicySet(policyText);
        ASSERT_TRUE(policies.has_value()) << name << "\n" << policies.error().Render();
        const auto links = input.find("links");
        ASSERT_TRUE(AddLinks(*policies, links == input.end() ? "-" : links->second)) << name;

        const auto mode =
            input.at("mode") == "strict" ? cedar::ValidationMode::Strict : cedar::ValidationMode::Permissive;
        const cedar::Validator validator{schemas.at(schemaFile)};
        const auto level = input.find("level");
        const cedar::ValidationResult result =
            level == input.end() || level->second == "-"
                ? validator.Validate(*policies, mode)
                : validator.ValidateWithLevel(*policies, mode, static_cast<std::uint32_t>(std::stoul(level->second)));
        std::set<std::string> kinds;
        for (const cedar::ValidationDiagnostic& error : result.errors)
        {
            EXPECT_EQ(error.diagnostic.severity, cedar::Severity::Error) << name;
            kinds.insert(std::string{cedar::CodeName(error.diagnostic.code)});
        }
        for (const cedar::ValidationDiagnostic& warning : result.warnings)
        {
            EXPECT_EQ(warning.diagnostic.severity, cedar::Severity::Warning) << name;
            kinds.insert(std::string{cedar::CodeName(warning.diagnostic.code)});
        }
        EXPECT_EQ(JoinKinds(kinds), expected[row].at("kinds")) << name << ": " << policyText;
        EXPECT_EQ(result.Passed() ? "pass" : "fail", expected[row].at("verdict")) << name << ": " << policyText;
    }
    EXPECT_GE(inputs.size(), minimum);
}

// The typechecker's table, now through the whole validator: the scope and literal checks and the
// action application warning are compared too.
TEST(Validator, TypecheckerTableEndToEnd)
{
    CheckTable(CEDAR_VALIDATION_PARITY_DIRECTORY, "Cases.tsv", "Expected.tsv", 200);
}

// Multi-action schemas, templates and links, enumerated entities, unknown types and actions,
// schemas with no request environment, incomplete schemas.
TEST(Validator, DriverTable)
{
    CheckTable(CEDAR_VALIDATION_PARITY_DIRECTORY, "DriverCases.tsv", "DriverExpected.tsv", 80);
}

// Level validation (RFC 76): dereferences through attributes, `has` paths, tags and `in` from
// variables, records and conditionals, entity literals, short-circuited operands, type errors.
TEST(Validator, LevelTable)
{
    CheckTable(CEDAR_VALIDATION_LEVEL_PARITY_DIRECTORY, "LevelCases.tsv", "LevelExpected.tsv", 60);
}

TEST(Validator, ValidRequestEnvironmentsOfTemplate)
{
    auto schema = cedar::ParseSchema(R"(
        entity Group;
        entity User in [Group] { level: Long };
        entity Admin;
        entity Doc;
        action view appliesTo { principal: [User, Admin], resource: Doc };
    )");
    ASSERT_TRUE(schema.has_value());
    auto body = cedar::ParseTemplate("permit(principal in ?principal, action, resource) when { principal.level > 1 };");
    ASSERT_TRUE(body.has_value());
    const auto environments = cedar::ValidRequestEnvironments(**body, schema->value);
    // Only User has `level`; a User principal may be linked to a Group or a User.
    ASSERT_EQ(environments.size(), 2U);
    for (const cedar::RequestEnvironment& environment : environments)
    {
        EXPECT_EQ(environment.principal.ToString(), "User");
        EXPECT_EQ(environment.resource.ToString(), "Doc");
        EXPECT_FALSE(environment.resourceSlot.has_value());
    }
    EXPECT_EQ(environments[0].principalSlot->ToString(), "Group");
    EXPECT_EQ(environments[1].principalSlot->ToString(), "User");
}

TEST(Validator, IncompleteSchemaFailsEveryBody)
{
    auto schema = cedar::ParseSchema("entity User; external entity Doc; action view appliesTo { principal: User, "
                                     "resource: User };");
    ASSERT_TRUE(schema.has_value());
    auto policies = cedar::ParsePolicySet("permit(principal, action, resource); permit(principal == ?principal, "
                                          "action, resource);");
    ASSERT_TRUE(policies.has_value());
    const cedar::ValidationResult result = cedar::Validator{schema->value}.Validate(*policies);
    ASSERT_EQ(result.errors.size(), 2U);
    for (const cedar::ValidationDiagnostic& error : result.errors)
    {
        EXPECT_EQ(error.diagnostic.code, cedar::DiagnosticCode::IncompleteSchema);
    }
    EXPECT_TRUE(result.warnings.empty());
}

} // namespace
