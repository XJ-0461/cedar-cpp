#include "Commands.hpp"
#include "Inputs.hpp"

#include <cedar/Schema.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <string>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification DirectionOption{
    .longName = "direction", .valueName = "DIRECTION",
    .help = "The direction of translation [possible values: json-to-cedar, cedar-to-json, "
            "cedar-to-json-with-resolved-types]"};
constexpr OptionSpecification InputOption{
    .longName = "schema", .shortName = 's', .valueName = "FILE",
    .help = "Filename to read the schema from. If not provided, reads standard input"};

constexpr std::array Options{DirectionOption, InputOption};

enum class Direction
{
    JsonToCedar,
    CedarToJson,
    CedarToJsonWithResolvedTypes,
};

auto WriteError(const std::string_view text) -> void
{
    std::fwrite(text.data(), 1, text.size(), stderr);
}

auto WriteDiagnostics(std::span<const Diagnostic> diagnostics) -> void
{
    if (!diagnostics.empty()) { WriteError(RenderDiagnostics(diagnostics)); }
}

/// The translated schema, or the exit status of a failure already written to standard error.
auto Translate(const Direction direction, const std::string& source) -> std::expected<std::string, ExitCode>
{
    const auto fail = [](const SchemaError& error) {
        WriteDiagnostics(error.Diagnostics());
        return std::unexpected{ExitCode::Failure};
    };
    if (direction == Direction::JsonToCedar)
    {
        const auto fragment = SchemaFragmentFromJson(source);
        if (!fragment) { return fail(fragment.error()); }
        auto text = ToCedar(*fragment);
        if (!text) { return fail(text.error()); }
        return *std::move(text);
    }
    const auto parsed = ParseSchemaFragment(source);
    if (!parsed) { return fail(parsed.error()); }
    // Warnings go to standard error before the output (`translate_schema.rs`).
    WriteDiagnostics(parsed->warnings);
    if (direction == Direction::CedarToJson) { return ToJson(parsed->value); }
    auto text = ToJsonWithResolvedTypes(parsed->value);
    if (!text) { return fail(text.error()); }
    return *std::move(text);
}

/// `translate_schema` (`cedar-policy-cli/src/command/translate_schema.rs`): reads a schema from the
/// file or standard input, prints it in the other format on standard output, and prints failures and
/// warnings on standard error.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    const auto name = arguments.Value(DirectionOption.longName);
    if (!name) { return std::unexpected{UsageError{"the following required arguments were not provided: --direction <DIRECTION>"}}; }
    Direction direction{};
    if (*name == "json-to-cedar") { direction = Direction::JsonToCedar; }
    else if (*name == "cedar-to-json") { direction = Direction::CedarToJson; }
    else if (*name == "cedar-to-json-with-resolved-types") { direction = Direction::CedarToJsonWithResolvedTypes; }
    else { return std::unexpected{UsageError{std::format("invalid value '{}' for '--direction <DIRECTION>'", *name)}}; }

    const auto source = ReadInput(arguments.Value(InputOption.longName), "schema");
    if (!source)
    {
        WriteError(std::format("{}\n", source.error().message));
        return ExitCode::Failure;
    }
    const auto translated = Translate(direction, *source);
    if (!translated) { return translated.error(); }
    std::fwrite(translated->data(), 1, translated->size(), stdout);
    std::fputc('\n', stdout);
    return ExitCode::Success;
}

} // namespace

const Command TranslateSchemaCommand{
    .name = "translate-schema",
    .summary = "Translate a schema between the Cedar and JSON formats",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
