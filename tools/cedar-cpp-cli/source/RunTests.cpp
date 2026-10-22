#include <cedar-cpp-cli/Commands.hpp>
#include <cedar-cpp-cli/Inputs.hpp>
#include <cedar-cpp-cli/JsonInputs.hpp>
#include <cedar-cpp-cli/SchemaInputs.hpp>
#include <cedar-cpp-cli/ValidationReport.hpp>

#include <json/JsonReader.hpp>
#include <json/ValueJson.hpp>

#include <cedar/Authorizer.hpp>
#include <cedar/Json.hpp>
#include <cedar/Validator.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory_resource>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification TestsOption{.longName = "tests", .valueName = "FILE", .help = "Tests in JSON format (required)"};

constexpr std::array Options{PoliciesOption, PolicyFormatOption, TemplateLinkedOption,
                             TestsOption,    SchemaOption,       SchemaFormatOption};

/// One test after reading (`command/run_test.rs` `TestCase`).
struct TestCase
{
    Request request;
    Entities entities;
    Decision decision;
    std::vector<std::string> reason;
    std::uint64_t errorCount;
};

auto Print(const std::string_view text) -> void { std::fwrite(text.data(), 1, text.size(), stdout); }

/// Whether `path` lies inside `directory`, comparing whole components as Rust's `Path::starts_with` does.
auto IsWithin(const std::filesystem::path& path, const std::filesystem::path& directory) -> bool
{
    auto pathPosition = path.begin();
    for (const std::filesystem::path& component : directory)
    {
        if (pathPosition == path.end() || *pathPosition != component) { return false; }
        ++pathPosition;
    }
    return true;
}

/// The entities of a test: a string names a file relative to the tests file's directory, which
/// must stay inside that directory; anything else is the entities themselves. Read by `schema`
/// when there is one.
auto ReadTestEntities(const detail::JsonNode& node, const std::filesystem::path& testDirectory,
                      const std::optional<Schema>& schema) -> std::expected<Entities, Failure>
{
    if (!node.IsString())
    {
        std::string text;
        AppendJson(text, node, JsonOrigin::Value);
        auto parsed = schema ? EntitiesFromJson(text, *schema, "<test entities>") : EntitiesFromJson(text, "<test entities>");
        if (!parsed) { return std::unexpected{Failure{"failed to parse entities", std::move(parsed).error().TakeDiagnostics()}}; }
        return *std::move(parsed);
    }
    std::error_code error;
    const std::filesystem::path canonicalPath = std::filesystem::canonical(testDirectory / node.text, error);
    if (error)
    {
        return std::unexpected{Failure{std::format("failed to canonicalize path `{}`: {}",
                                                   (testDirectory / node.text).string(), error.message())}};
    }
    const std::filesystem::path canonicalDirectory = std::filesystem::canonical(testDirectory, error);
    if (error)
    {
        return std::unexpected{Failure{std::format("failed to canonicalize test file directory `{}`: {}",
                                                   testDirectory.string(), error.message())}};
    }
    if (!IsWithin(canonicalPath, canonicalDirectory))
    {
        return std::unexpected{Failure{std::format("entities file `{}` is outside of the test file directory `{}`",
                                                   canonicalPath.string(), canonicalDirectory.string())}};
    }
    return LoadEntities(canonicalPath.string(), schema);
}

/// Reads one test from its `serde_json::Value` (`CheckedTestCaseSeed`): the fields `request`,
/// `entities`, `decision` (`"allow"` or `"deny"`), `reason` (policy ids) and `num_errors` (a
/// non-negative integer); unknown fields such as `name` are ignored. With a schema the context and
/// entities are read by it and the request is checked against it.
auto ReadTest(const detail::JsonNode& node, const std::filesystem::path& testDirectory,
              const std::optional<Schema>& schema) -> std::expected<TestCase, Failure>
{
    static constexpr std::array<std::string_view, 5> Names{"request", "entities", "decision", "reason", "num_errors"};
    auto fields = ReadStructFields(node, Names, JsonOrigin::Value, "UncheckedTestCase");
    if (!fields) { return std::unexpected{Failure{std::move(fields).error()}}; }
    for (std::size_t index = 0; index < Names.size(); ++index)
    {
        if ((*fields)[index] == nullptr) { return std::unexpected{Failure{std::format("missing field `{}`", Names[index])}}; }
    }

    // A unit variant of an enum read from a Value is its name, or a one-entry map from the name to null.
    const detail::JsonNode* decisionNode = (*fields)[2];
    std::string_view decisionName;
    if (decisionNode->IsString()) { decisionName = decisionNode->text; }
    else if (decisionNode->IsObject() && decisionNode->count == 1 &&
             decisionNode->Members()[0].value.kind == detail::JsonKind::Null)
    {
        decisionName = decisionNode->Members()[0].key;
    }
    else { return std::unexpected{Failure{"invalid type for `decision`, expected `allow` or `deny`"}}; }
    if (decisionName != "allow" && decisionName != "deny")
    {
        return std::unexpected{Failure{std::format("unknown variant `{}`, expected `allow` or `deny`", decisionName)}};
    }

    const detail::JsonNode* reasonNode = (*fields)[3];
    if (!reasonNode->IsArray()) { return std::unexpected{Failure{"invalid type for `reason`, expected a sequence"}}; }
    std::vector<std::string> reason;
    for (const detail::JsonNode& element : reasonNode->Elements())
    {
        if (!element.IsString()) { return std::unexpected{Failure{"invalid type for a `reason` entry, expected a string"}}; }
        reason.emplace_back(element.text);
    }

    const detail::JsonNode* countNode = (*fields)[4];
    std::uint64_t errorCount = 0;
    const auto number = countNode->kind == detail::JsonKind::Number ? std::optional{detail::ClassifyJsonNumber(countNode->text)}
                                                                     : std::nullopt;
    if (number && number->kind == detail::JsonNumberKind::Long && number->value >= 0)
    {
        errorCount = static_cast<std::uint64_t>(number->value);
    }
    else if (number && number->kind == detail::JsonNumberKind::UnsignedLong)
    {
        errorCount = std::stoull(std::string{countNode->text});
    }
    else { return std::unexpected{Failure{"invalid value for `num_errors`, expected a non-negative integer"}}; }

    auto request = ReadRequestJson(*(*fields)[0], JsonOrigin::Value, "the test request",
                                   RequestSchema{.schema = schema ? &*schema : nullptr, .validate = true});
    if (!request) { return std::unexpected{std::move(request).error()}; }
    auto entities = ReadTestEntities(*(*fields)[1], testDirectory, schema);
    if (!entities) { return std::unexpected{std::move(entities).error()}; }
    return TestCase{.request = *std::move(request),
                    .entities = *std::move(entities),
                    .decision = decisionName == "allow" ? Decision::Allow : Decision::Deny,
                    .reason = std::move(reason),
                    .errorCount = errorCount};
}

auto DecisionName(const Decision decision) -> std::string_view { return decision == Decision::Allow ? "Allow" : "Deny"; }

/// `compare_test_decisions`: the decision must match, every expected reason must be among the
/// determining policies, and the number of erroring policies must match. Nothing means a pass.
auto Compare(const TestCase& test, const Response& response) -> std::optional<std::string>
{
    if (response.decision != test.decision)
    {
        return std::format("expected {}, got {}", DecisionName(test.decision), DecisionName(response.decision));
    }
    std::vector<std::string> problems;
    std::string missing;
    for (const std::string& expected : test.reason)
    {
        if (std::ranges::find(response.reason, std::string_view{expected}, &PolicyId::Text) != response.reason.end())
        {
            continue;
        }
        missing += std::format("{}`{}`", missing.empty() ? "" : ", ", expected);
    }
    if (!missing.empty()) { problems.push_back("missing reason(s): " + missing); }
    if (response.errors.size() != test.errorCount)
    {
        std::string problem = std::format("expected {} error(s), but got {} runtime error(s)", test.errorCount,
                                          response.errors.size());
        for (std::size_t index = 0; index < response.errors.size(); ++index)
        {
            const PolicyEvaluationError& error = response.errors[index];
            problem += std::format("{}error while evaluating policy `{}`: {}", index == 0 ? ": " : ", ",
                                   error.policyId.Text(), error.error.Message());
        }
        problems.push_back(std::move(problem));
    }
    if (problems.empty()) { return std::nullopt; }
    std::string joined;
    for (const std::string& problem : problems) { joined += (joined.empty() ? "" : "; ") + problem; }
    return joined;
}

/// `command/run_test.rs`: reads the policies, the tests file (a JSON array of tests) and the
/// optional schema, runs each test and prints `ok`, `fail: <why>` or `error:` with the reason it
/// could not be read; fails when any test did not pass. With a schema, a test that reads is an
/// error too when the policies do not validate against the schema in strict mode without warnings
/// (Rust validates before each test; the answer is the same for every test).
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    auto policyInput = ReadPolicyInput(arguments);
    if (!policyInput) { return std::unexpected{std::move(policyInput).error()}; }
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    const auto options = AuthorizerOptionsFromEnvironment();
    if (!options) { return std::unexpected{options.error()}; }
    const auto testsFile = arguments.Value(TestsOption.longName);
    if (!testsFile)
    {
        return std::unexpected{UsageError{"the following required arguments were not provided:\n  --tests <FILE>"}};
    }
    const auto fail = [&](const Failure& failure) {
        Report(invocation.errorFormat, failure);
        return ExitCode::Failure;
    };

    auto policies = LoadPolicySet(*policyInput);
    if (!policies) { return fail(policies.error()); }

    // `load_partial_tests` reads the file into a `Vec<serde_json::Value>`.
    auto text = ReadInput(*testsFile, "test");
    if (!text) { return fail(text.error()); }
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(*text, scratch);
    if (!document)
    {
        return fail(Failure{std::format("failed to parse tests from file {}: {}", *testsFile, document.error().message)});
    }
    const detail::JsonNode& root = document->Root();
    if (auto checked = detail::CheckStrictJson(root, detail::DuplicateKeys::LastWins); !checked)
    {
        return fail(Failure{std::format("failed to parse tests from file {}: {}", *testsFile, checked.error().message)});
    }
    if (!root.IsArray())
    {
        return fail(Failure{std::format("failed to parse tests from file {}: invalid type: {}, expected a sequence",
                                        *testsFile, detail::DescribeJson(root))});
    }
    auto schema = LoadSchema(arguments);
    if (!schema) { return fail(schema.error()); }
    std::optional<ValidationResult> validation;
    if (*schema) { validation = Validator{**schema}.Validate(*policies, ValidationMode::Strict); }

    const std::filesystem::path testDirectory = std::filesystem::path{std::string{*testsFile}}.parent_path();
    std::size_t failures = 0;
    Print(std::format("running {} test(s)\n", root.count));
    for (const detail::JsonNode& test : root.Elements())
    {
        // `test["name"].as_str()`: the last `name` of an object, when it is a string.
        std::string_view name = "(unnamed)";
        if (test.IsObject())
        {
            for (const detail::JsonMember& member : test.Members())
            {
                if (member.key == "name") { name = member.value.IsString() ? member.value.text : "(unnamed)"; }
            }
        }
        Print(std::format("  test {} ... ", name));
        std::fflush(stdout);

        auto read = ReadTest(test, testDirectory, *schema);
        if (!read)
        {
            ++failures;
            Print("error:\n  ");
            Report(invocation.errorFormat, read.error());
            continue;
        }
        if (validation && !validation->PassedWithoutWarnings())
        {
            ++failures;
            Print("error:\n  ");
            Report(invocation.errorFormat, Failure{"policy set validation failed", ValidationDiagnostics(*validation)});
            continue;
        }
        const Response response = IsAuthorized(read->request, *policies, read->entities, *options);
        if (const auto problem = Compare(*read, response))
        {
            ++failures;
            Print(std::format("fail: {}\n", *problem));
        }
        else { Print("ok\n"); }
    }
    Print(std::format("results: {} passed, {} failed\n", root.count - failures, failures));
    return failures == 0 ? ExitCode::Success : ExitCode::Failure;
}

} // namespace

const Command RunTestsCommand{
    .name = "run-tests",
    .summary = "Run test cases (requests with their expected decision, reasons and error count) against policies",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
