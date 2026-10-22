#include "validation/UnicodeSecurityTables.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
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

auto IsStringCheck(const cedar::DiagnosticCode code) -> bool
{
    using enum cedar::DiagnosticCode;
    return code == MixedScriptString || code == BidirectionalCharactersInString ||
           code == BidirectionalCharactersInIdentifier || code == MixedScriptIdentifier ||
           code == ConfusableIdentifier;
}

// Each policy's string-check warning kinds in order. Rust orders the policies by its own policy
// set iteration, so only the order within one policy is compared.
using WarningsByPolicy = std::map<std::string, std::vector<std::string>>;

auto ParseExpectedWarnings(const std::string& text) -> WarningsByPolicy
{
    WarningsByPolicy warnings;
    if (text == "-") { return warnings; }
    for (const std::string& entry : Split(text, ';'))
    {
        const std::size_t colon = entry.find(':');
        warnings[entry.substr(0, colon)].push_back(entry.substr(colon + 1));
    }
    return warnings;
}

// `-`, or `template:link:slot=uid+slot=uid` entries separated by `;`.
auto AddLinks(cedar::PolicySet& policies, const std::string& text) -> testing::AssertionResult
{
    if (text == "-") { return testing::AssertionSuccess(); }
    for (const std::string& entry : Split(text, ';'))
    {
        const std::size_t first = entry.find(':');
        const std::size_t second = entry.find(':', first + 1);
        cedar::SlotBindings bindings;
        for (const std::string& slot : Split(entry.substr(second + 1), '+'))
        {
            const std::size_t equals = slot.find('=');
            auto uid = cedar::ParseEntityUid(slot.substr(equals + 1));
            if (!uid) { return testing::AssertionFailure() << "bad uid in " << entry; }
            bindings.Bind(slot.substr(0, equals) == "?principal" ? cedar::SlotId::Principal : cedar::SlotId::Resource,
                          *uid);
        }
        const std::string linkId = entry.substr(first + 1, second - first - 1);
        if (auto linked = policies.Link(entry.substr(0, first), cedar::PolicyId{linkId}, bindings); !linked)
        {
            return testing::AssertionFailure() << "cannot link " << entry << ": " << linked.error().Render();
        }
    }
    return testing::AssertionSuccess();
}

// Validates every case with Validator::Validate and compares the verdict, the set of all kinds
// and each policy's string-check warnings in order with the oracle's.
TEST(StringChecks, OracleTable)
{
    const std::string directory = CEDAR_STRING_CHECKS_PARITY_DIRECTORY;
    const auto inputs = ReadTable(directory + "/Cases.tsv");
    const auto expected = ReadTable(directory + "/Expected.tsv");
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
        ASSERT_TRUE(AddLinks(*policies, input.at("links"))) << name;

        const auto mode =
            input.at("mode") == "strict" ? cedar::ValidationMode::Strict : cedar::ValidationMode::Permissive;
        const cedar::ValidationResult result = cedar::Validator{schemas.at(schemaFile)}.Validate(*policies, mode);
        std::set<std::string> kinds;
        WarningsByPolicy warnings;
        for (const cedar::ValidationDiagnostic& error : result.errors)
        {
            kinds.insert(std::string{cedar::CodeName(error.diagnostic.code)});
        }
        for (const cedar::ValidationDiagnostic& warning : result.warnings)
        {
            EXPECT_EQ(warning.diagnostic.severity, cedar::Severity::Warning) << name;
            const std::string kind{cedar::CodeName(warning.diagnostic.code)};
            kinds.insert(kind);
            if (IsStringCheck(warning.diagnostic.code)) { warnings[std::string{warning.policyId.Text()}].push_back(kind); }
        }
        std::string joined;
        for (const std::string& kind : kinds) { joined += (joined.empty() ? "" : ",") + kind; }
        EXPECT_EQ(joined.empty() ? "-" : joined, expected[row].at("kinds")) << name << ": " << policyText;
        EXPECT_EQ(result.Passed() ? "pass" : "fail", expected[row].at("verdict")) << name << ": " << policyText;
        EXPECT_EQ(warnings, ParseExpectedWarnings(expected[row].at("strings"))) << name << ": " << policyText;
    }
    EXPECT_GE(inputs.size(), 60U);
}

TEST(StringChecks, GeneratedTablesAreSortedAndDisjoint)
{
    const auto allowed = cedar::detail::IdentifierAllowedRanges();
    ASSERT_FALSE(allowed.empty());
    for (std::size_t index = 0; index < allowed.size(); ++index)
    {
        EXPECT_LE(allowed[index].first, allowed[index].last);
        if (index > 0) { EXPECT_LT(allowed[index - 1].last, allowed[index].first); }
    }
    const auto ranges = cedar::detail::ScriptSetRanges();
    const auto sets = cedar::detail::ScriptSets();
    ASSERT_FALSE(ranges.empty());
    for (std::size_t index = 0; index < ranges.size(); ++index)
    {
        EXPECT_LE(ranges[index].first, ranges[index].last);
        EXPECT_LT(ranges[index].set, sets.size());
        if (index > 0) { EXPECT_LT(ranges[index - 1].last, ranges[index].first); }
    }
}

} // namespace
