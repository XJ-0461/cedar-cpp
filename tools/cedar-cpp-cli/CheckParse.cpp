#include "Commands.hpp"
#include "Inputs.hpp"
#include "SchemaInputs.hpp"

#include <cedar/Json.hpp>
#include <cedar/Syntax.hpp>

#include <array>
#include <format>
#include <optional>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification ExpressionOption{
    .longName = "expression", .valueName = "EXPRESSION", .help = "Expression to parse"};
constexpr OptionSpecification EntitiesOption{
    .longName = "entities", .valueName = "FILE",
    .help = "File containing JSON representation of a Cedar entity hierarchy"};

constexpr std::array Options{PoliciesOption, PolicyFormatOption,  TemplateLinkedOption, ExpressionOption,
                             SchemaOption,   SchemaFormatOption, EntitiesOption};

/// `check_parse` (`cedar-policy-cli/src/command/check_parse.rs`): with no policies, expression,
/// schema or entities given, reads policies from standard input; otherwise checks each input
/// given, reports every one that fails, and fails if any did.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    auto policyInput = ReadPolicyInput(arguments);
    if (!policyInput) { return std::unexpected{std::move(policyInput).error()}; }
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }

    const auto expression = arguments.Value(ExpressionOption.longName);
    const auto schema = arguments.Value(SchemaOption.longName);
    const auto entities = arguments.Value(EntitiesOption.longName);

    ExitCode exitCode = ExitCode::Success;
    const auto fail = [&](const Failure& failure) {
        Report(invocation.errorFormat, failure);
        exitCode = ExitCode::Failure;
    };

    if (!policyInput->policiesFile && !expression && !schema && !entities)
    {
        if (auto policies = LoadPolicySet(*policyInput); !policies) { fail(policies.error()); }
        return exitCode;
    }

    if (policyInput->policiesFile)
    {
        if (auto policies = LoadPolicySet(*policyInput); !policies) { fail(policies.error()); }
    }
    if (expression)
    {
        if (auto parsed = ParseExpression(*expression); !parsed)
        {
            fail(Failure{"failed to parse expression", std::move(parsed).error().TakeDiagnostics()});
        }
    }
    // A schema that fails to load counts as none for the entities.
    std::optional<Schema> loadedSchema;
    if (schema)
    {
        if (auto loaded = LoadSchema(arguments)) { loadedSchema = *std::move(loaded); }
        else { fail(loaded.error()); }
    }
    if (entities)
    {
        if (auto loaded = LoadEntities(*entities, loadedSchema); !loaded) { fail(loaded.error()); }
    }
    return exitCode;
}

} // namespace

const Command CheckParseCommand{
    .name = "check-parse",
    .summary = "Check that policies, an expression and/or entities parse. With none given, reads policies "
               "from standard input",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
