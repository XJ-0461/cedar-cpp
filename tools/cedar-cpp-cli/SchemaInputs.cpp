#include "SchemaInputs.hpp"

#include "Inputs.hpp"

#include <cedar/Json.hpp>

#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace cedar::cli
{

auto LoadSchema(const ParsedArguments& arguments) -> std::expected<std::optional<Schema>, Failure>
{
    const auto path = arguments.Value(SchemaOption.longName);
    if (!path) { return std::optional<Schema>{}; }
    auto text = ReadInput(*path, "schema");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    const auto fail = [&](SchemaError error) {
        return std::unexpected{
            Failure{std::format("failed to parse schema from file {}", *path), std::move(error).TakeDiagnostics()}};
    };
    if (arguments.Value(SchemaFormatOption.longName) == "json")
    {
        auto schema = SchemaFromJson(*text, *path);
        if (!schema) { return fail(std::move(schema).error()); }
        return std::optional<Schema>{*std::move(schema)};
    }
    auto schema = ParseSchema(*text, *path);
    if (!schema) { return fail(std::move(schema).error()); }
    if (!schema->warnings.empty())
    {
        const std::string rendered = RenderDiagnostics(schema->warnings);
        std::fwrite(rendered.data(), 1, rendered.size(), stderr);
    }
    return std::optional<Schema>{std::move(schema->value)};
}

auto LoadEntities(const std::string_view path, const std::optional<Schema>& schema) -> std::expected<Entities, Failure>
{
    if (!schema) { return LoadEntities(path); }
    auto text = ReadInput(path, "entities");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto parsed = EntitiesFromJson(*text, *schema, path);
    if (!parsed)
    {
        return std::unexpected{Failure{std::format("failed to parse entities from file {}", path),
                                       std::move(parsed).error().TakeDiagnostics()}};
    }
    return *std::move(parsed);
}

} // namespace cedar::cli
