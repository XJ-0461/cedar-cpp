// A request context from JSON, with or without a schema (Rust's `ContextJsonParser`,
// `cedar-policy-core/src/entities/json/context.rs`, behind `Context::from_json_str`).

#include "json/JsonReader.hpp"
#include "json/ValueJson.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Json.hpp>

#include <format>
#include <memory>
#include <memory_resource>
#include <string>
#include <utility>

namespace cedar
{

auto ContextFromJson(const std::string_view json, const std::string_view sourceName)
    -> std::expected<Context, ContextError>
{
    const auto fail = [&](detail::JsonReadError error) {
        return std::unexpected{ContextError{detail::ToDiagnostic(
            std::move(error), MakeSourceText(std::string{sourceName}, std::string{json}), "in the context")}};
    };

    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(json, scratch);
    if (!document) { return fail(detail::FromSyntaxError(std::move(document).error())); }
    const detail::JsonNode& root = document->Root();

    // Rust reads the text into a `serde_json::Value`, which keeps the last value of a repeated key
    // at every level, and then reads that value as a Cedar value.
    if (auto checked = detail::CheckStrictJson(root, detail::DuplicateKeys::LastWins); !checked)
    {
        return fail(std::move(checked).error());
    }
    auto storage = std::make_shared<ValueArena>();
    auto value = detail::ReadCedarValue(root, detail::DuplicateKeys::LastWins, *storage);
    if (!value) { return fail(std::move(value).error()); }
    // `Context::from_expr` takes only a record expression; an escape at the top is not one, even
    // when it evaluates to a record.
    if (!detail::IsCedarRecord(root, detail::DuplicateKeys::LastWins))
    {
        return std::unexpected{ContextError{Diagnostic{
            .code = DiagnosticCode::ContextNotARecord,
            .message = std::format("the context must be a record, not `{}`", ToCedar(*value)),
            .primary = SourceSpan{root.offset, root.offset + 1},
            .source = MakeSourceText(std::string{sourceName}, std::string{json}),
        }}};
    }
    return Context{value->AsRecord(), std::move(storage)};
}

auto ContextFromJson(const std::string_view json, const Schema& schema, const EntityUid& action,
                     const std::string_view sourceName) -> std::expected<Context, ContextError>
{
    // `context_schema_for_action`: an undeclared action, or an external one, has no context type.
    const detail::ValidatorAction* declaration = detail::SchemaAccess::Validator(schema).FindAction(action);
    if (declaration == nullptr || !declaration->context)
    {
        return std::unexpected{ContextError{Diagnostic{
            .code = DiagnosticCode::ContextMissingAction,
            .message = std::format("action `{}` is not declared in the schema, so its context type is unknown",
                                   action.ToString()),
        }}};
    }
    const auto fail = [&](detail::JsonReadError error) {
        return std::unexpected{ContextError{detail::ToDiagnostic(
            std::move(error), MakeSourceText(std::string{sourceName}, std::string{json}), "in the context")}};
    };

    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(json, scratch);
    if (!document) { return fail(detail::FromSyntaxError(std::move(document).error())); }
    const detail::JsonNode& root = document->Root();
    if (auto checked = detail::CheckStrictJson(root, detail::DuplicateKeys::LastWins); !checked)
    {
        return fail(std::move(checked).error());
    }
    // The context type is a record, so a value read by it is one. Its values are not checked
    // against their types here: Context::Validate and Request::Create do that.
    auto storage = std::make_shared<ValueArena>();
    auto value = detail::ReadTypedCedarValue(root, *declaration->context, detail::DuplicateKeys::LastWins, *storage,
                                             detail::EntityTypeNames::Intern);
    if (!value) { return fail(std::move(value).error()); }
    return Context{value->AsRecord(), std::move(storage)};
}

} // namespace cedar
