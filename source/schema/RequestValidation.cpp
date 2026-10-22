// Request and context validation against a schema (Rust's `RequestSchema` for `ValidatorSchema`,
// `cedar-policy-core/src/validator/coreschema.rs`, behind `Request::new` and `Context::validate`).

#include "entities/Conformance.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>

#include <format>
#include <string>
#include <utility>

namespace cedar
{

namespace
{

[[nodiscard]] auto Invalid(const DiagnosticCode code, std::string message) -> RequestValidationError
{
    return RequestValidationError{Diagnostic{.code = code, .message = std::move(message)}};
}

[[nodiscard]] auto TypeList(const std::span<const EntityType> types) -> std::string
{
    std::string text;
    for (const EntityType& type : types)
    {
        text += std::format("{}`{}`", text.empty() ? "" : ", ", type.GetName().ToString());
    }
    return text.empty() ? "none" : text;
}

/// A principal or resource: its type must be declared, and an enumerated type's id one of its ids.
[[nodiscard]] auto CheckScopeEntity(const detail::ValidatorSchema& schema, const EntityUid& uid,
                                    const DiagnosticCode undeclared, const std::string_view role)
    -> std::expected<void, RequestValidationError>
{
    const detail::ValidatorEntityType* declaration = schema.FindEntityType(uid.Type());
    if (declaration == nullptr)
    {
        return std::unexpected{Invalid(undeclared, std::format("the {} type `{}` is not declared in the schema", role,
                                                               uid.Type().GetName().ToString()))};
    }
    if (declaration->kind == detail::ValidatorEntityTypeKind::Enumerated && !declaration->IsEnumChoice(uid.Id()))
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestInvalidEnumEntity,
                                       std::format("the {} `{}` is not one of the ids the schema declares for its "
                                                   "enumerated entity type",
                                                   role, uid.ToString()))};
    }
    return {};
}

} // namespace

auto Context::Validate(const Schema& schema, const EntityUid& action) const -> std::expected<void, RequestValidationError>
{
    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    const detail::ValidatorAction* declaration = validator.FindAction(action);
    if (declaration == nullptr)
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestUndeclaredAction,
                                       std::format("action `{}` is not declared in the schema", action.ToString()))};
    }
    if (!declaration->context)
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestExternalAction,
                                       std::format("action `{}` is external, so no request names it",
                                                   action.ToString()))};
    }
    if (auto checked = detail::CheckEntityUidsIn(validator, AsValue()); !checked)
    {
        const detail::InvalidEntityUid& invalid = checked.error();
        if (invalid.problem == detail::EntityUidProblem::UndeclaredAction)
        {
            return std::unexpected{Invalid(DiagnosticCode::RequestUndeclaredAction,
                                           std::format("the context names action `{}`, which is not declared in the "
                                                       "schema",
                                                       invalid.uid.ToString()))};
        }
        return std::unexpected{Invalid(DiagnosticCode::RequestInvalidEnumEntity,
                                       std::format("the context names `{}`, which is not one of the ids the schema "
                                                   "declares for its enumerated entity type",
                                                   invalid.uid.ToString()))};
    }
    if (!detail::HasType(AsValue(), *declaration->context))
    {
        return std::unexpected{Invalid(DiagnosticCode::InvalidContext,
                                       std::format("the context `{}` is not valid for action `{}`: it must have the "
                                                   "type `{}`",
                                                   ToCedar(AsValue()), action.ToString(),
                                                   detail::ToString(*declaration->context)))};
    }
    return {};
}

auto Request::Create(EntityUid principal, EntityUid action, EntityUid resource, Context context, const Schema& schema)
    -> std::expected<Request, RequestValidationError>
{
    // `validate_scope_variables`, then `validate_context`.
    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    if (auto checked = CheckScopeEntity(validator, principal, DiagnosticCode::UndeclaredPrincipalType, "principal");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked = CheckScopeEntity(validator, resource, DiagnosticCode::UndeclaredResourceType, "resource");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    const detail::ValidatorAction* declaration = validator.FindAction(action);
    if (declaration == nullptr)
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestUndeclaredAction,
                                       std::format("action `{}` is not declared in the schema", action.ToString()))};
    }
    if (declaration->external)
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestExternalAction,
                                       std::format("action `{}` is external, so no request names it",
                                                   action.ToString()))};
    }
    if (!declaration->AppliesToPrincipal(principal.Type()))
    {
        return std::unexpected{Invalid(DiagnosticCode::InvalidPrincipalType,
                                       std::format("principal type `{}` is not valid for action `{}`; valid types: {}",
                                                   principal.Type().GetName().ToString(), action.ToString(),
                                                   TypeList(declaration->principalTypes)))};
    }
    if (!declaration->AppliesToResource(resource.Type()))
    {
        return std::unexpected{Invalid(DiagnosticCode::InvalidResourceType,
                                       std::format("resource type `{}` is not valid for action `{}`; valid types: {}",
                                                   resource.Type().GetName().ToString(), action.ToString(),
                                                   TypeList(declaration->resourceTypes)))};
    }
    if (auto checked = context.Validate(schema, action); !checked) { return std::unexpected{std::move(checked).error()}; }
    return Request{std::move(principal), std::move(action), std::move(resource), std::move(context)};
}

} // namespace cedar
