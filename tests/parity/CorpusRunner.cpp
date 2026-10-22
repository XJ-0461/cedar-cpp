// Runs the cedar-integration-tests JsonTest files against cedar-cpp, as Rust's
// `perform_integration_test` (`cedar-testing/src/integration_testing.rs`) runs them against
// cedar-policy with `ErrorComparisonMode::PolicyIds` and `ValidationComparisonMode::AgreeOnAll`:
// load the schema, the policies and the entities by the schema, check the validation verdict, then
// per request parse principal, action, resource and context by the schema, authorize, and compare
// the decision, the reason set and the set of erroring policy ids. Where Rust panics (a schema,
// policy, entity or request that does not load) the test fails.
//
// It reports and does not gate: the exit status is 0 whatever the results, unless
// --require-all-pass is given.

#include <json/JsonReader.hpp>
#include <json/ValueJson.hpp>

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Json.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>
#include <cedar/Syntax.hpp>
#include <cedar/Validator.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using cedar::detail::JsonKind;
using cedar::detail::JsonMember;
using cedar::detail::JsonNode;

constexpr std::string_view Usage =
    "usage: cedar-cpp-corpus-runner --policy-selection every|index\n"
    "           [--integration-tests-directory DIR [--test-file FILE]...]\n"
    "           [--regressions-directory DIR] [--failures-output FILE] [--require-all-pass]\n"
    "\n"
    "Without --test-file, every JsonTest under DIR/tests and every case under DIR/corpus-tests is\n"
    "run. A relative --test-file and every file a JsonTest names are resolved against DIR. Each\n"
    "subdirectory of the regressions directory is one case: its test.json is a JsonTest whose file\n"
    "names are relative to that subdirectory.\n";

// ---- The JsonTest format ----

enum class PolicyFormat
{
    Cedar,
    Json,
};

enum class SchemaFormat
{
    Cedar,
    Json,
};

/// One request with its expected result (Rust's `JsonRequest`). The uids and the context are kept
/// as JSON text, written as the `serde_json::Value` Rust reads them into.
struct ExpectedRequest
{
    std::string description;
    std::string principal;
    std::string action;
    std::string resource;
    std::string context;
    bool validateRequest = true;
    cedar::Decision decision = cedar::Decision::Deny;
    std::set<std::string> reason;
    std::set<std::string> errors;
};

/// Rust's `JsonTest`, file names resolved.
struct TestDescriptor
{
    fs::path policies;
    PolicyFormat policyFormat = PolicyFormat::Cedar;
    fs::path entities;
    fs::path schema;
    SchemaFormat schemaFormat = SchemaFormat::Cedar;
    bool shouldValidate = false;
    std::vector<ExpectedRequest> requests;
};

/// Appends `node` as JSON text, keeping only the last member of each repeated key, as a
/// `serde_json::Value` does.
void AppendValueJson(std::string& output, const JsonNode& node)
{
    switch (node.kind)
    {
    case JsonKind::Null: output += "null"; return;
    case JsonKind::False: output += "false"; return;
    case JsonKind::True: output += "true"; return;
    case JsonKind::Number: output += node.text; return;
    case JsonKind::String: cedar::detail::AppendJsonString(output, node.text); return;
    case JsonKind::Array:
    {
        output += '[';
        bool first = true;
        for (const JsonNode& element : node.Elements())
        {
            if (!first) { output += ','; }
            first = false;
            AppendValueJson(output, element);
        }
        output += ']';
        return;
    }
    case JsonKind::Object:
    {
        std::vector<const JsonMember*> members;
        cedar::detail::EffectiveMembers(node, cedar::detail::DuplicateKeys::LastWins, members);
        output += '{';
        bool first = true;
        for (const JsonMember* member : members)
        {
            if (!first) { output += ','; }
            first = false;
            cedar::detail::AppendJsonString(output, member->key);
            output += ':';
            AppendValueJson(output, member->value);
        }
        output += '}';
        return;
    }
    }
}

/// The members of a derived struct with `deny_unknown_fields`: each must be one of `names`, given
/// at most once. Missing fields are null in the result.
template <std::size_t Count>
auto ReadStruct(const JsonNode& node, const std::array<std::string_view, Count>& names, const std::string_view structName)
    -> std::expected<std::array<const JsonNode*, Count>, std::string>
{
    if (!node.IsObject()) { return std::unexpected{std::format("{} is not an object", structName)}; }
    std::array<const JsonNode*, Count> fields{};
    for (const JsonMember& member : node.Members())
    {
        const auto found = std::ranges::find(names, member.key);
        if (found == names.end()) { return std::unexpected{std::format("unknown field `{}` in {}", member.key, structName)}; }
        const auto index = static_cast<std::size_t>(found - names.begin());
        if (fields[index] != nullptr) { return std::unexpected{std::format("duplicate field `{}` in {}", member.key, structName)}; }
        fields[index] = &member.value;
    }
    return fields;
}

auto ReadString(const JsonNode* node, const std::string_view field) -> std::expected<std::string, std::string>
{
    if (node == nullptr) { return std::unexpected{std::format("missing field `{}`", field)}; }
    if (!node->IsString()) { return std::unexpected{std::format("`{}` is not a string", field)}; }
    return std::string{node->text};
}

auto ReadBoolean(const JsonNode* node, const std::string_view field) -> std::expected<bool, std::string>
{
    if (node == nullptr) { return std::unexpected{std::format("missing field `{}`", field)}; }
    if (node->kind == JsonKind::True) { return true; }
    if (node->kind == JsonKind::False) { return false; }
    return std::unexpected{std::format("`{}` is not a boolean", field)};
}

auto ReadIdSet(const JsonNode* node, const std::string_view field) -> std::expected<std::set<std::string>, std::string>
{
    if (node == nullptr) { return std::unexpected{std::format("missing field `{}`", field)}; }
    if (!node->IsArray()) { return std::unexpected{std::format("`{}` is not an array", field)}; }
    std::set<std::string> ids;
    for (const JsonNode& element : node->Elements())
    {
        if (!element.IsString()) { return std::unexpected{std::format("an entry of `{}` is not a string", field)}; }
        ids.emplace(element.text);
    }
    return ids;
}

auto ReadValueText(const JsonNode* node, const std::string_view field) -> std::expected<std::string, std::string>
{
    if (node == nullptr) { return std::unexpected{std::format("missing field `{}`", field)}; }
    std::string text;
    AppendValueJson(text, *node);
    return text;
}

auto ReadRequest(const JsonNode& node) -> std::expected<ExpectedRequest, std::string>
{
    static constexpr std::array<std::string_view, 9> Names{"description", "principal", "action",
                                                           "resource",    "context",   "validateRequest",
                                                           "decision",    "reason",    "errors"};
    auto fields = ReadStruct(node, Names, "a request");
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    ExpectedRequest request;
    auto description = ReadString((*fields)[0], "description");
    if (!description) { return std::unexpected{std::move(description).error()}; }
    request.description = *std::move(description);
    std::array<std::string*, 4> values{&request.principal, &request.action, &request.resource, &request.context};
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        auto text = ReadValueText((*fields)[index + 1], Names[index + 1]);
        if (!text) { return std::unexpected{std::move(text).error()}; }
        *values[index] = *std::move(text);
    }
    if ((*fields)[5] != nullptr)
    {
        auto validate = ReadBoolean((*fields)[5], "validateRequest");
        if (!validate) { return std::unexpected{std::move(validate).error()}; }
        request.validateRequest = *validate;
    }
    auto decision = ReadString((*fields)[6], "decision");
    if (!decision) { return std::unexpected{std::move(decision).error()}; }
    if (*decision == "allow") { request.decision = cedar::Decision::Allow; }
    else if (*decision == "deny") { request.decision = cedar::Decision::Deny; }
    else { return std::unexpected{std::format("unknown decision `{}`", *decision)}; }
    auto reason = ReadIdSet((*fields)[7], "reason");
    if (!reason) { return std::unexpected{std::move(reason).error()}; }
    request.reason = *std::move(reason);
    auto errors = ReadIdSet((*fields)[8], "errors");
    if (!errors) { return std::unexpected{std::move(errors).error()}; }
    request.errors = *std::move(errors);
    return request;
}

auto ReadFile(const fs::path& path) -> std::optional<std::string>
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) { return std::nullopt; }
    return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

/// A file a JsonTest names: relative names are relative to `root` (Rust's
/// `resolve_integration_test_path`), absolute ones are used as they are.
auto Resolve(const fs::path& root, const std::string& name) -> fs::path
{
    const fs::path path{name};
    return path.is_relative() ? root / path : path;
}

/// Reads a JsonTest file as serde reads it into Rust's `JsonTest` (`deny_unknown_fields`).
auto ReadDescriptor(const fs::path& path, const fs::path& root) -> std::expected<TestDescriptor, std::string>
{
    const auto text = ReadFile(path);
    if (!text) { return std::unexpected{std::string{"cannot read the file"}}; }
    std::pmr::monotonic_buffer_resource resource;
    auto document = cedar::detail::ReadJson(*text, resource);
    if (!document) { return std::unexpected{std::format("JSON syntax: {}", document.error().message)}; }
    const JsonNode& rootNode = document->Root();
    if (auto strict = cedar::detail::CheckStrictJson(rootNode, cedar::detail::DuplicateKeys::LastWins); !strict)
    {
        return std::unexpected{std::format("JSON: {}", strict.error().message)};
    }
    static constexpr std::array<std::string_view, 7> Names{"policies",       "policyFormat",   "entities", "schema",
                                                           "schemaFormat",   "shouldValidate", "requests"};
    auto fields = ReadStruct(rootNode, Names, "the test");
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    TestDescriptor test;
    auto policies = ReadString((*fields)[0], "policies");
    if (!policies) { return std::unexpected{std::move(policies).error()}; }
    test.policies = Resolve(root, *policies);
    if ((*fields)[1] != nullptr)
    {
        auto format = ReadString((*fields)[1], "policyFormat");
        if (!format) { return std::unexpected{std::move(format).error()}; }
        if (*format == "json") { test.policyFormat = PolicyFormat::Json; }
        else if (*format != "cedar") { return std::unexpected{std::format("unknown policyFormat `{}`", *format)}; }
    }
    auto entities = ReadString((*fields)[2], "entities");
    if (!entities) { return std::unexpected{std::move(entities).error()}; }
    test.entities = Resolve(root, *entities);
    auto schema = ReadString((*fields)[3], "schema");
    if (!schema) { return std::unexpected{std::move(schema).error()}; }
    test.schema = Resolve(root, *schema);
    if ((*fields)[4] != nullptr)
    {
        auto format = ReadString((*fields)[4], "schemaFormat");
        if (!format) { return std::unexpected{std::move(format).error()}; }
        if (*format == "json") { test.schemaFormat = SchemaFormat::Json; }
        else if (*format != "cedar") { return std::unexpected{std::format("unknown schemaFormat `{}`", *format)}; }
    }
    auto shouldValidate = ReadBoolean((*fields)[5], "shouldValidate");
    if (!shouldValidate) { return std::unexpected{std::move(shouldValidate).error()}; }
    test.shouldValidate = *shouldValidate;
    const JsonNode* requests = (*fields)[6];
    if (requests == nullptr) { return std::unexpected{std::string{"missing field `requests`"}}; }
    if (!requests->IsArray()) { return std::unexpected{std::string{"`requests` is not an array"}}; }
    for (const JsonNode& element : requests->Elements())
    {
        auto request = ReadRequest(element);
        if (!request) { return std::unexpected{std::move(request).error()}; }
        test.requests.push_back(*std::move(request));
    }
    return test;
}

// ---- Running a test ----

enum class Suite
{
    HandWritten,
    Corpus,
    Regressions,
};

constexpr std::array<std::string_view, 3> SuiteNames{"hand-written", "corpus", "regressions"};

/// Where a test failed before its requests ran.
enum class SetupStage
{
    Descriptor,
    Schema,
    Policies,
    Entities,
};

constexpr std::array<std::string_view, 4> SetupStageNames{"descriptor", "schema", "policies", "entities"};

enum class ValidationOutcome
{
    Pass,
    Fail,
    Skipped,
};

struct TestResult
{
    std::string name;
    Suite suite = Suite::HandWritten;
    std::optional<SetupStage> setupFailure;
    ValidationOutcome validation = ValidationOutcome::Skipped;
    std::size_t requestsParsed = 0;
    std::size_t requestParseFailures = 0;
    std::size_t authorizationPasses = 0;
    std::size_t authorizationFailures = 0;
    /// The first failure, on one line, with the stage that failed.
    std::string failureStage;
    std::string failure;

    [[nodiscard]] auto Failed() const noexcept -> bool { return !failure.empty(); }
};

/// The first diagnostic of an error on one line.
auto OneLine(const std::span<const cedar::Diagnostic> diagnostics) -> std::string
{
    if (diagnostics.empty()) { return "(no diagnostic)"; }
    std::string line = std::format("{}: {}", cedar::CodeName(diagnostics.front().code), diagnostics.front().message);
    std::ranges::replace(line, '\n', ' ');
    return line;
}

auto SetText(const std::set<std::string>& ids) -> std::string
{
    std::string text = "{";
    for (const std::string& id : ids)
    {
        if (text.size() > 1) { text += ", "; }
        text += id;
    }
    return text + "}";
}

auto DecisionText(const cedar::Decision decision) -> std::string_view
{
    return decision == cedar::Decision::Allow ? "allow" : "deny";
}

/// Whether `policies` pass strict validation against `schema` (Rust's `Validator::validate` with
/// `ValidationMode::Strict`), or nothing when validation is not run.
auto ValidatePolicies(const cedar::Schema& schema, const cedar::PolicySet& policies) -> std::optional<bool>
{
    return cedar::Validator{schema}.Validate(policies, cedar::ValidationMode::Strict).Passed();
}

/// Parses one request as Rust's `parse_request_from_test` does, or says why it does not parse.
auto ParseRequest(const ExpectedRequest& expected, const cedar::Schema& schema) -> std::expected<cedar::Request, std::string>
{
    auto principal = cedar::EntityUidFromJson(expected.principal, "<principal>");
    if (!principal) { return std::unexpected{"principal: " + OneLine(principal.error().Diagnostics())}; }
    auto action = cedar::EntityUidFromJson(expected.action, "<action>");
    if (!action) { return std::unexpected{"action: " + OneLine(action.error().Diagnostics())}; }
    auto resource = cedar::EntityUidFromJson(expected.resource, "<resource>");
    if (!resource) { return std::unexpected{"resource: " + OneLine(resource.error().Diagnostics())}; }
    auto context = cedar::ContextFromJson(expected.context, schema, *action, "<context>");
    if (!context) { return std::unexpected{"context: " + OneLine(context.error().Diagnostics())}; }
    if (!expected.validateRequest)
    {
        return cedar::Request{*std::move(principal), *std::move(action), *std::move(resource), *std::move(context)};
    }
    auto request = cedar::Request::Create(*std::move(principal), *std::move(action), *std::move(resource),
                                          *std::move(context), schema);
    if (!request) { return std::unexpected{"request validation: " + OneLine(request.error().Diagnostics())}; }
    return *std::move(request);
}

/// What differs between the response and the expected result, or nothing when they agree.
auto CompareResponse(const cedar::Response& response, const ExpectedRequest& expected) -> std::optional<std::string>
{
    if (response.decision != expected.decision)
    {
        return std::format("decision: expected {}, got {}", DecisionText(expected.decision),
                           DecisionText(response.decision));
    }
    std::set<std::string> reason;
    for (const cedar::PolicyId& id : response.reason) { reason.emplace(id.Text()); }
    if (reason != expected.reason)
    {
        return std::format("reason: expected {}, got {}", SetText(expected.reason), SetText(reason));
    }
    std::set<std::string> errors;
    for (const cedar::PolicyEvaluationError& error : response.errors) { errors.emplace(error.policyId.Text()); }
    if (errors != expected.errors)
    {
        return std::format("errors: expected {}, got {}", SetText(expected.errors), SetText(errors));
    }
    return std::nullopt;
}

void RecordFailure(TestResult& result, const std::string_view stage, std::string message)
{
    if (result.Failed()) { return; }
    std::ranges::replace(message, '\n', ' ');
    result.failureStage = stage;
    result.failure = std::move(message);
}

auto LoadSchema(const TestDescriptor& test) -> std::expected<cedar::Schema, std::string>
{
    const auto text = ReadFile(test.schema);
    if (!text) { return std::unexpected{std::format("cannot read {}", test.schema.string())}; }
    const std::string sourceName = test.schema.filename().string();
    if (test.schemaFormat == SchemaFormat::Json)
    {
        auto schema = cedar::SchemaFromJson(*text, sourceName);
        if (!schema) { return std::unexpected{OneLine(schema.error().Diagnostics())}; }
        return *std::move(schema);
    }
    auto schema = cedar::ParseSchema(*text, sourceName);
    if (!schema) { return std::unexpected{OneLine(schema.error().Diagnostics())}; }
    return std::move(schema->value);
}

auto LoadPolicies(const TestDescriptor& test) -> std::expected<cedar::PolicySet, std::string>
{
    const auto text = ReadFile(test.policies);
    if (!text) { return std::unexpected{std::format("cannot read {}", test.policies.string())}; }
    const std::string sourceName = test.policies.filename().string();
    if (test.policyFormat == PolicyFormat::Json)
    {
        auto policies = cedar::PolicySetFromJson(*text, sourceName);
        if (!policies) { return std::unexpected{OneLine(policies.error().Diagnostics())}; }
        return *std::move(policies);
    }
    auto policies = cedar::ParsePolicySet(*text, sourceName);
    if (!policies) { return std::unexpected{OneLine(policies.error().Diagnostics())}; }
    return *std::move(policies);
}

auto LoadEntities(const TestDescriptor& test, const cedar::Schema& schema) -> std::expected<cedar::Entities, std::string>
{
    const auto text = ReadFile(test.entities);
    if (!text) { return std::unexpected{std::format("cannot read {}", test.entities.string())}; }
    auto entities = cedar::EntitiesFromJson(*text, schema, test.entities.filename().string());
    if (!entities) { return std::unexpected{OneLine(entities.error().Diagnostics())}; }
    return *std::move(entities);
}

/// Runs one JsonTest. Unlike Rust, which stops at the first failure, every request is run, so
/// the counts cover each stage separately.
auto RunTest(const TestDescriptor& test, TestResult& result, const cedar::AuthorizerOptions options) -> void
{
    const auto setupFailed = [&](const SetupStage stage, std::string message) {
        result.setupFailure = stage;
        RecordFailure(result, SetupStageNames[static_cast<std::size_t>(stage)], std::move(message));
    };
    auto schema = LoadSchema(test);
    if (!schema) { return setupFailed(SetupStage::Schema, std::move(schema).error()); }
    auto policies = LoadPolicies(test);
    if (!policies) { return setupFailed(SetupStage::Policies, std::move(policies).error()); }
    auto entities = LoadEntities(test, *schema);
    if (!entities) { return setupFailed(SetupStage::Entities, std::move(entities).error()); }

    const std::optional<bool> validated = ValidatePolicies(*schema, *policies);
    if (!validated) { result.validation = ValidationOutcome::Skipped; }
    else if (*validated == test.shouldValidate) { result.validation = ValidationOutcome::Pass; }
    else
    {
        result.validation = ValidationOutcome::Fail;
        RecordFailure(result, "validation",
                      test.shouldValidate ? "expected the policies to validate, they did not"
                                          : "expected validation to fail, it passed");
    }

    for (const ExpectedRequest& expected : test.requests)
    {
        auto request = ParseRequest(expected, *schema);
        if (!request)
        {
            ++result.requestParseFailures;
            RecordFailure(result, "request-parsing", std::format("request \"{}\": {}", expected.description, request.error()));
            continue;
        }
        ++result.requestsParsed;
        const cedar::Response response = cedar::IsAuthorized(*request, *policies, *entities, options);
        if (auto difference = CompareResponse(response, expected))
        {
            ++result.authorizationFailures;
            RecordFailure(result, "authorization", std::format("request \"{}\": {}", expected.description, *difference));
            continue;
        }
        ++result.authorizationPasses;
    }
}

// ---- Finding tests ----

struct TestFile
{
    fs::path path;
    /// The directory the JsonTest's file names are relative to.
    fs::path root;
    std::string name;
    Suite suite;
};

auto HasJsonExtension(const fs::path& path) -> bool
{
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const char character) {
        return static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    });
    return extension == ".json";
}

auto SortedFiles(const fs::path& directory) -> std::vector<fs::path>
{
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator{directory})
    {
        if (entry.is_regular_file()) { files.push_back(entry.path()); }
    }
    std::ranges::sort(files);
    return files;
}

/// Rust's `get_integration_tests` (every `*.json` under `tests/` that reads as a JsonTest) and
/// `get_corpus_tests("")` (every `*.json` under `corpus-tests/` but the `*.entities.json` files).
auto ScanIntegrationTests(const fs::path& root) -> std::vector<TestFile>
{
    std::vector<TestFile> tests;
    for (const fs::path& path : SortedFiles(root / "tests"))
    {
        if (!HasJsonExtension(path) || !ReadDescriptor(path, root)) { continue; }
        tests.push_back({path, root, fs::relative(path, root).string(), Suite::HandWritten});
    }
    for (const fs::path& path : SortedFiles(root / "corpus-tests"))
    {
        const std::string filename = path.filename().string();
        if (!filename.ends_with(".json") || filename.ends_with(".entities.json")) { continue; }
        tests.push_back({path, root, fs::relative(path, root).string(), Suite::Corpus});
    }
    return tests;
}

auto SuiteOf(const fs::path& relative) -> Suite
{
    return !relative.empty() && *relative.begin() == "corpus-tests" ? Suite::Corpus : Suite::HandWritten;
}

// ---- Arguments and the report ----

struct Arguments
{
    std::optional<fs::path> integrationTestsDirectory;
    std::vector<std::string> testFiles;
    std::optional<fs::path> regressionsDirectory;
    std::optional<fs::path> failuresOutput;
    std::optional<cedar::PolicySelection> policySelection;
    bool requireAllPass = false;
};

auto ParseArguments(const std::span<char* const> arguments) -> std::expected<Arguments, std::string>
{
    Arguments parsed;
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view flag = arguments[index];
        if (flag == "--require-all-pass")
        {
            parsed.requireAllPass = true;
            continue;
        }
        if (index + 1 >= arguments.size()) { return std::unexpected{std::format("{} needs a value", flag)}; }
        const std::string_view value = arguments[++index];
        if (flag == "--integration-tests-directory") { parsed.integrationTestsDirectory = fs::path{value}; }
        else if (flag == "--test-file") { parsed.testFiles.emplace_back(value); }
        else if (flag == "--regressions-directory") { parsed.regressionsDirectory = fs::path{value}; }
        else if (flag == "--failures-output") { parsed.failuresOutput = fs::path{value}; }
        else if (flag == "--policy-selection")
        {
            if (value == "every") { parsed.policySelection = cedar::PolicySelection::EvaluateEveryPolicy; }
            else if (value == "index") { parsed.policySelection = cedar::PolicySelection::ScopeIndex; }
            else { return std::unexpected{std::format("--policy-selection takes every or index, not `{}`", value)}; }
        }
        else { return std::unexpected{std::format("unknown argument `{}`", flag)}; }
    }
    if (!parsed.policySelection) { return std::unexpected{std::string{"--policy-selection is required"}}; }
    if (!parsed.integrationTestsDirectory && !parsed.regressionsDirectory)
    {
        return std::unexpected{std::string{"give --integration-tests-directory, --regressions-directory or both"}};
    }
    if (!parsed.testFiles.empty() && !parsed.integrationTestsDirectory)
    {
        return std::unexpected{std::string{"--test-file needs --integration-tests-directory"}};
    }
    return parsed;
}

struct SuiteCounts
{
    std::size_t files = 0;
    std::size_t filesPassed = 0;
    std::size_t filesPassedValidationSkipped = 0;
    std::size_t filesFailed = 0;
    std::array<std::size_t, 4> setupFailures{};
    std::size_t validationPass = 0;
    std::size_t validationFail = 0;
    std::size_t validationSkipped = 0;
    std::size_t requestsParsed = 0;
    std::size_t requestParseFailures = 0;
    std::size_t authorizationPasses = 0;
    std::size_t authorizationFailures = 0;
};

auto Report(const std::span<const TestResult> results) -> std::string
{
    std::array<SuiteCounts, 3> counts{};
    for (const TestResult& result : results)
    {
        SuiteCounts& suite = counts[static_cast<std::size_t>(result.suite)];
        ++suite.files;
        if (result.Failed()) { ++suite.filesFailed; }
        else if (result.validation == ValidationOutcome::Skipped) { ++suite.filesPassedValidationSkipped; }
        else { ++suite.filesPassed; }
        if (result.setupFailure)
        {
            ++suite.setupFailures[static_cast<std::size_t>(*result.setupFailure)];
            continue;
        }
        switch (result.validation)
        {
        case ValidationOutcome::Pass: ++suite.validationPass; break;
        case ValidationOutcome::Fail: ++suite.validationFail; break;
        case ValidationOutcome::Skipped: ++suite.validationSkipped; break;
        }
        suite.requestsParsed += result.requestsParsed;
        suite.requestParseFailures += result.requestParseFailures;
        suite.authorizationPasses += result.authorizationPasses;
        suite.authorizationFailures += result.authorizationFailures;
    }
    std::string text;
    for (std::size_t index = 0; index < counts.size(); ++index)
    {
        const SuiteCounts& suite = counts[index];
        if (suite.files == 0) { continue; }
        text += std::format("{}: {} files: {} pass, {} pass with validation skipped, {} fail\n", SuiteNames[index],
                            suite.files, suite.filesPassed, suite.filesPassedValidationSkipped, suite.filesFailed);
        text += "  setup failures:";
        for (std::size_t stage = 0; stage < SetupStageNames.size(); ++stage)
        {
            text += std::format(" {} {}", SetupStageNames[stage], suite.setupFailures[stage]);
        }
        text += '\n';
        text += std::format("  validation (files): {} pass, {} fail, {} skipped\n", suite.validationPass,
                            suite.validationFail, suite.validationSkipped);
        text += std::format("  request parsing (requests): {} pass, {} fail\n", suite.requestsParsed,
                            suite.requestParseFailures);
        text += std::format("  authorization (parsed requests): {} pass, {} fail\n", suite.authorizationPasses,
                            suite.authorizationFailures);
    }
    return text;
}

auto Write(std::FILE* file, const std::string_view text) -> void { std::fwrite(text.data(), 1, text.size(), file); }

auto Run(const Arguments& arguments) -> int
{
    std::vector<TestFile> files;
    if (arguments.integrationTestsDirectory)
    {
        const fs::path& root = *arguments.integrationTestsDirectory;
        if (arguments.testFiles.empty())
        {
            for (const char* directory : {"tests", "corpus-tests"})
            {
                if (!fs::is_directory(root / directory))
                {
                    Write(stderr, std::format("cedar-cpp-corpus-runner: {} is not a directory\n", (root / directory).string()));
                    return 2;
                }
            }
            files = ScanIntegrationTests(root);
        }
        for (const std::string& name : arguments.testFiles)
        {
            const fs::path path = Resolve(root, name);
            const fs::path relative = fs::path{name}.is_relative() ? fs::path{name} : fs::relative(path, root);
            files.push_back({path, root, relative.string(), SuiteOf(relative)});
        }
    }
    if (arguments.regressionsDirectory)
    {
        const fs::path& directory = *arguments.regressionsDirectory;
        if (!fs::is_directory(directory))
        {
            Write(stderr, std::format("cedar-cpp-corpus-runner: {} is not a directory\n", directory.string()));
            return 2;
        }
        std::vector<fs::path> cases;
        for (const fs::directory_entry& entry : fs::directory_iterator{directory})
        {
            if (entry.is_directory()) { cases.push_back(entry.path()); }
        }
        std::ranges::sort(cases);
        for (const fs::path& caseDirectory : cases)
        {
            files.push_back({caseDirectory / "test.json", caseDirectory,
                             "Regressions/" + caseDirectory.filename().string(), Suite::Regressions});
        }
    }

    const cedar::AuthorizerOptions options{.policySelection = *arguments.policySelection};
    std::vector<TestResult> results;
    results.reserve(files.size());
    for (const TestFile& file : files)
    {
        TestResult& result = results.emplace_back();
        result.name = file.name;
        result.suite = file.suite;
        auto test = ReadDescriptor(file.path, file.root);
        if (!test)
        {
            result.setupFailure = SetupStage::Descriptor;
            RecordFailure(result, "descriptor", std::move(test).error());
            continue;
        }
        try
        {
            RunTest(*test, result, options);
        }
        catch (const std::exception& exception)
        {
            RecordFailure(result, "exception", exception.what());
        }
    }

    std::string failures;
    std::size_t failureCount = 0;
    bool allPassed = true;
    for (const TestResult& result : results)
    {
        if (result.validation == ValidationOutcome::Skipped) { allPassed = false; }
        if (!result.Failed()) { continue; }
        allPassed = false;
        ++failureCount;
        failures += std::format("{}\t{}\t{}\n", result.name, result.failureStage, result.failure);
    }

    std::string summary = Report(results);
    summary += std::format("failing files: {}\n", failureCount);
    Write(stdout, summary);
    if (arguments.failuresOutput)
    {
        std::ofstream output{*arguments.failuresOutput, std::ios::binary | std::ios::trunc};
        output << failures;
        if (!output)
        {
            Write(stderr, std::format("cedar-cpp-corpus-runner: cannot write {}\n", arguments.failuresOutput->string()));
            return 2;
        }
    }
    else { Write(stdout, failures); }
    return arguments.requireAllPass && !allPassed ? 1 : 0;
}

} // namespace

auto main(const int argumentCount, char** argumentValues) -> int
{
    auto arguments = ParseArguments(std::span<char* const>{argumentValues, static_cast<std::size_t>(argumentCount)});
    if (!arguments)
    {
        Write(stderr, std::format("cedar-cpp-corpus-runner: {}\n\n{}", arguments.error(), Usage));
        return 2;
    }
    return Run(*arguments);
}
