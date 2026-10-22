#include <cedar-cpp-cli/Commands.hpp>
#include <cedar-cpp-cli/Inputs.hpp>
#include <cedar-cpp-cli/Requests.hpp>
#include <cedar-cpp-cli/SchemaInputs.hpp>

#include <model/EscapeDebug.hpp>

#include <cedar/Authorizer.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <optional>
#include <vector>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification EntitiesOption{
    .longName = "entities", .valueName = "FILE",
    .help = "File containing JSON representation of the Cedar entity hierarchy (required)"};
constexpr OptionSpecification VerboseOption{
    .longName = "verbose", .shortName = 'v', .takesValue = false,
    .help = "More verbose output: which policies determined the decision"};
constexpr OptionSpecification TimingOption{
    .longName = "timing", .shortName = 't', .takesValue = false,
    .help = "Time authorization and report timing information"};

constexpr std::array Options{
    PrincipalOption, ActionOption, ResourceOption, ContextOption, RequestJsonOption, RequestValidationOption,
    PoliciesOption, PolicyFormatOption, TemplateLinkedOption, SchemaOption, SchemaFormatOption, EntitiesOption,
    VerboseOption, TimingOption};

auto AppendPolicyId(std::string& output, const PolicyId& id) -> void { detail::AppendEscapeDebug(output, id.Text()); }

/// The answer as `command/authorize.rs` prints it: the decision, then each policy that failed to
/// evaluate (`error while evaluating policy `id`: message`), then with `verbose` the determining
/// policies, one per line indented by two spaces.
auto Describe(const Response& response, const bool verbose) -> std::string
{
    std::string text = response.decision == Decision::Allow ? "ALLOW\n" : "DENY\n";
    if (!response.errors.empty())
    {
        text += '\n';
        for (const PolicyEvaluationError& error : response.errors)
        {
            text += "error while evaluating policy `";
            AppendPolicyId(text, error.policyId);
            text += std::format("`: {}\n", error.error.Message());
        }
    }
    if (verbose)
    {
        text += '\n';
        if (response.reason.empty()) { text += "note: no policies applied to this request\n"; }
        else
        {
            text += "note: this decision was due to the following policies:\n";
            for (const PolicyId& id : response.reason)
            {
                text += "  ";
                AppendPolicyId(text, id);
                text += '\n';
            }
            text += '\n';
        }
    }
    return text;
}

/// `command/authorize.rs`: reads the policies, entities and request, reporting every input that
/// fails; then answers the request. Exits 0 on Allow and 2 on Deny.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    auto policyInput = ReadPolicyInput(arguments);
    if (!policyInput) { return std::unexpected{std::move(policyInput).error()}; }
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    if (auto checked = CheckRequestArguments(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    const auto options = AuthorizerOptionsFromEnvironment();
    if (!options) { return std::unexpected{options.error()}; }
    const auto entitiesFile = arguments.Value(EntitiesOption.longName);
    if (!entitiesFile)
    {
        return std::unexpected{
            UsageError{"the following required arguments were not provided:\n  --entities <FILE>"}};
    }

    std::fputs("\n", stdout);
    std::vector<Failure> failures;
    auto policies = LoadPolicySet(*policyInput);
    if (!policies) { failures.push_back(std::move(policies).error()); }
    // A schema that fails to load counts as none for the inputs read after it.
    auto loadedSchema = LoadSchema(arguments);
    std::optional<Schema> schema;
    if (loadedSchema) { schema = *std::move(loadedSchema); }
    else { failures.push_back(std::move(loadedSchema).error()); }
    auto entities = LoadEntities(*entitiesFile, schema);
    if (!entities) { failures.push_back(std::move(entities).error()); }
    auto request = LoadRequest(arguments, schema);
    if (!request)
    {
        Failure failure = std::move(request).error();
        failure.message = "failed to parse request: " + failure.message;
        failures.push_back(std::move(failure));
    }
    if (!failures.empty())
    {
        for (const Failure& failure : failures) { Report(invocation.errorFormat, failure); }
        return ExitCode::Failure;
    }

    const auto start = std::chrono::steady_clock::now();
    const Response response = IsAuthorized(*request, *policies, *entities, *options);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::string text;
    if (arguments.Has(TimingOption.longName))
    {
        text += std::format("Authorization Time (micro seconds) : {}\n",
                            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
    text += Describe(response, arguments.Has(VerboseOption.longName));
    std::fwrite(text.data(), 1, text.size(), stdout);
    return response.decision == Decision::Allow ? ExitCode::Success : ExitCode::AuthorizeDeny;
}

} // namespace

const Command AuthorizeCommand{
    .name = "authorize",
    .summary = "Evaluate an authorization request",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
