#include "Commands.hpp"
#include "Inputs.hpp"
#include "Requests.hpp"
#include "SchemaInputs.hpp"

#include <cedar/Authorizer.hpp>
#include <cedar/Syntax.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <optional>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification EntitiesOption{
    .longName = "entities", .valueName = "FILE",
    .help = "File containing JSON representation of the Cedar entity hierarchy; without it the hierarchy is empty"};

constexpr std::array Options{PrincipalOption,   ActionOption,            ResourceOption, ContextOption,
                             RequestJsonOption, RequestValidationOption, SchemaOption,   SchemaFormatOption,
                             EntitiesOption};

/// `command/evaluate.rs`: reads the request, the expression and the entities, stopping at the
/// first that fails, then evaluates the expression and prints its value in Cedar syntax.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    if (auto checked = CheckRequestArguments(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }

    std::fputs("\n", stdout);
    const auto fail = [&](const Failure& failure) {
        Report(invocation.errorFormat, failure);
        return ExitCode::Failure;
    };
    auto schema = LoadSchema(arguments);
    if (!schema) { return fail(schema.error()); }
    auto request = LoadRequest(arguments, *schema);
    if (!request) { return fail(request.error()); }
    auto expression = ParseExpression(*invocation.positional);
    if (!expression)
    {
        return fail(Failure{"failed to parse the expression", std::move(expression).error().TakeDiagnostics()});
    }
    const auto entitiesFile = arguments.Value(EntitiesOption.longName);
    auto entities = entitiesFile ? LoadEntities(*entitiesFile, *schema)
                                 : decltype(LoadEntities(*entitiesFile, *schema)){std::in_place};
    if (!entities) { return fail(entities.error()); }

    auto value = EvaluateExpression(*request, *entities, *expression);
    if (!value)
    {
        return fail(Failure{"failed to evaluate the expression", {value.error().ToDiagnostic(expression->Source())}});
    }
    const std::string text = ToCedar(value->Get()) + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    return ExitCode::Success;
}

} // namespace

const Command EvaluateCommand{
    .name = "evaluate",
    .summary = "Evaluate a Cedar expression",
    .options = Options,
    .run = Run,
    .positionalName = "EXPRESSION",
};

} // namespace cedar::cli
