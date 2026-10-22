#include "Commands.hpp"
#include "Inputs.hpp"
#include "SchemaInputs.hpp"
#include "ValidationReport.hpp"

#include <json/JsonReader.hpp>
#include <model/EscapeDebug.hpp>

#include <cedar/Validator.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace cedar::cli
{

auto ValidationDiagnostics(const ValidationResult& result) -> std::vector<Diagnostic>
{
    std::vector<Diagnostic> diagnostics;
    diagnostics.reserve(result.errors.size() + result.warnings.size());
    for (const auto* group : {&result.errors, &result.warnings})
    {
        for (const ValidationDiagnostic& found : *group)
        {
            Diagnostic diagnostic = found.diagnostic;
            std::string message = "for policy `";
            detail::AppendEscapeDebug(message, found.policyId.Text());
            diagnostic.message = std::format("{}`, {}", message, diagnostic.message);
            diagnostics.push_back(std::move(diagnostic));
        }
    }
    return diagnostics;
}

namespace
{

constexpr OptionSpecification RequiredSchemaOption{
    .longName = "schema", .shortName = 's', .valueName = "FILE", .help = "File containing the schema (required)"};
constexpr OptionSpecification DenyWarningsOption{
    .longName = "deny-warnings", .takesValue = false, .help = "Report a validation failure for non-fatal warnings"};
constexpr OptionSpecification ValidationModeOption{
    .longName = "validation-mode", .valueName = "VALIDATION_MODE",
    .help = "Validate the policy using this mode [default: strict] [possible values: strict, permissive, partial]"};
constexpr OptionSpecification LevelOption{
    .longName = "level", .valueName = "LEVEL", .help = "Validate the policy at this level"};

constexpr std::array Options{RequiredSchemaOption, SchemaFormatOption,   PoliciesOption,       PolicyFormatOption,
                             TemplateLinkedOption, DenyWarningsOption,   ValidationModeOption, LevelOption};

auto Print(const std::string_view text) -> void { std::fwrite(text.data(), 1, text.size(), stdout); }

/// One line: `{"message", "severity", "diagnostics": [{"policy", "severity", "code", "message",
/// "source", "offset", "length"}]}`, the source fields only when the diagnostic has them. The
/// top-level severity is that of the first error, else of the first warning, else `advice`, as
/// the Rust CLI reports it.
auto ToJsonLine(const std::string_view headline, const ValidationResult& result) -> std::string
{
    std::string out = "{\"message\":";
    detail::AppendJsonString(out, headline);
    out += ",\"severity\":";
    out += !result.errors.empty() ? "\"error\"" : !result.warnings.empty() ? "\"warning\"" : "\"advice\"";
    out += ",\"diagnostics\":[";
    bool first = true;
    for (const auto* group : {&result.errors, &result.warnings})
    {
        for (const ValidationDiagnostic& found : *group)
        {
            const Diagnostic& diagnostic = found.diagnostic;
            if (!first) { out += ','; }
            first = false;
            // The id as the Rust CLI displays it, escaped (`PolicyID`'s Display).
            std::string policy;
            detail::AppendEscapeDebug(policy, found.policyId.Text());
            out += "{\"policy\":";
            detail::AppendJsonString(out, policy);
            out += ",\"severity\":";
            detail::AppendJsonString(out, SeverityName(diagnostic.severity));
            out += ",\"code\":";
            detail::AppendJsonString(out, CodeName(diagnostic.code));
            out += ",\"message\":";
            detail::AppendJsonString(out, diagnostic.message);
            if (diagnostic.source)
            {
                out += ",\"source\":";
                detail::AppendJsonString(out, diagnostic.source->name);
            }
            if (diagnostic.primary)
            {
                out += std::format(",\"offset\":{},\"length\":{}", diagnostic.primary->begin, diagnostic.primary->Length());
            }
            out += '}';
        }
    }
    out += "]}\n";
    return out;
}

/// `command/validate.rs`: reads the policies, then the schema, validates (with `--level`, level
/// validation too) and prints whether the set passed with every error and warning. Exit 3 when there is an error, or with
/// `--deny-warnings` a warning; 1 when an input cannot be read. Both strict and permissive modes
/// are supported (the Rust CLI needs its `permissive-validate` feature for the latter); partial
/// validation is not built.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    auto policyInput = ReadPolicyInput(arguments);
    if (!policyInput) { return std::unexpected{std::move(policyInput).error()}; }
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    if (!arguments.Has(RequiredSchemaOption.longName))
    {
        return std::unexpected{UsageError{"the following required arguments were not provided:\n  --schema <FILE>"}};
    }
    ValidationMode mode = ValidationMode::Strict;
    bool partial = false;
    if (const auto text = arguments.Value(ValidationModeOption.longName))
    {
        if (*text == "permissive") { mode = ValidationMode::Permissive; }
        else if (*text == "partial") { partial = true; }
        else if (*text != "strict")
        {
            return std::unexpected{UsageError{std::format(
                "invalid value '{}' for '--validation-mode <VALIDATION_MODE>' [possible values: strict, permissive, "
                "partial]",
                *text)}};
        }
    }
    std::optional<std::uint32_t> level;
    if (const auto text = arguments.Value(LevelOption.longName))
    {
        std::uint32_t parsed = 0;
        const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), parsed);
        if (text->empty() || error != std::errc{} || end != text->data() + text->size())
        {
            return std::unexpected{UsageError{std::format("invalid value '{}' for '--level <LEVEL>'", *text)}};
        }
        level = parsed;
    }

    if (partial)
    {
        std::fputs("Error: `--validation-mode partial` is not supported\n", stderr);
        return ExitCode::Failure;
    }

    const auto fail = [&](const Failure& failure) {
        Report(invocation.errorFormat, failure);
        return ExitCode::Failure;
    };
    auto policies = LoadPolicySet(*policyInput);
    if (!policies) { return fail(policies.error()); }
    auto schema = LoadSchema(arguments);
    if (!schema) { return fail(schema.error()); }

    const Validator validator{**std::move(schema)};
    const ValidationResult result =
        level ? validator.ValidateWithLevel(*policies, mode, *level) : validator.Validate(*policies, mode);
    const bool failed = !result.Passed() || (arguments.Has(DenyWarningsOption.longName) && !result.PassedWithoutWarnings());
    const std::string_view headline = failed ? "policy set validation failed" : "policy set validation passed";
    if (invocation.errorFormat == ErrorFormat::Json) { Print(ToJsonLine(headline, result)); }
    else
    {
        std::string text = std::format("{}\n", headline);
        const std::vector<Diagnostic> diagnostics = ValidationDiagnostics(result);
        text += diagnostics.empty() ? std::string{"no errors or warnings\n"} : RenderDiagnostics(diagnostics);
        Print(text);
    }
    return failed ? ExitCode::ValidationFailure : ExitCode::Success;
}

} // namespace

const Command ValidateCommand{
    .name = "validate",
    .summary = "Validate a policy set against a schema",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
