// Request and context validation against a schema (Rust's `RequestSchema` for `ValidatorSchema`,
// `cedar-policy-core/src/validator/coreschema.rs`, behind `Request::new` and `Context::validate`).

#include "schema/RequestValidation.hpp"

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

} // namespace

namespace detail
{

auto CheckRequestAction(const ValidatorSchema& schema, const EntityUid& action)
    -> std::expected<const ValidatorAction*, RequestValidationError>
{
    const ValidatorAction* declaration = schema.FindAction(action);
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
    return declaration;
}

auto CheckPrincipalApplies(const ValidatorAction& declaration, const EntityUid& action, const EntityType& type)
    -> std::expected<void, RequestValidationError>
{
    if (!declaration.AppliesToPrincipal(type))
    {
        return std::unexpected{Invalid(DiagnosticCode::InvalidPrincipalType,
                                       std::format("principal type `{}` is not valid for action `{}`; valid types: {}",
                                                   type.GetName().ToString(), action.ToString(),
                                                   TypeList(declaration.principalTypes)))};
    }
    return {};
}

auto CheckResourceApplies(const ValidatorAction& declaration, const EntityUid& action, const EntityType& type)
    -> std::expected<void, RequestValidationError>
{
    if (!declaration.AppliesToResource(type))
    {
        return std::unexpected{Invalid(DiagnosticCode::InvalidResourceType,
                                       std::format("resource type `{}` is not valid for action `{}`; valid types: {}",
                                                   type.GetName().ToString(), action.ToString(),
                                                   TypeList(declaration.resourceTypes)))};
    }
    return {};
}

auto CheckScopeEntity(const ValidatorSchema& schema, const EntityType& type, const Eid* id,
                      const DiagnosticCode undeclared, const std::string_view role)
    -> std::expected<void, RequestValidationError>
{
    const ValidatorEntityType* declaration = schema.FindEntityType(type);
    if (declaration == nullptr)
    {
        return std::unexpected{Invalid(undeclared, std::format("the {} type `{}` is not declared in the schema", role,
                                                               type.GetName().ToString()))};
    }
    if (id != nullptr && declaration->kind == ValidatorEntityTypeKind::Enumerated && !declaration->IsEnumChoice(*id))
    {
        return std::unexpected{Invalid(DiagnosticCode::RequestInvalidEnumEntity,
                                       std::format("the {} `{}` is not one of the ids the schema declares for its "
                                                   "enumerated entity type",
                                                   role, EntityUid{type, *id}.ToString()))};
    }
    return {};
}

} // namespace detail

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
    if (auto checked = detail::CheckScopeEntity(validator, principal.Type(), &principal.Id(),
                                                DiagnosticCode::UndeclaredPrincipalType, "principal");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked = detail::CheckScopeEntity(validator, resource.Type(), &resource.Id(),
                                                DiagnosticCode::UndeclaredResourceType, "resource");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    auto declaration = detail::CheckRequestAction(validator, action);
    if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
    if (auto checked = detail::CheckPrincipalApplies(**declaration, action, principal.Type()); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked = detail::CheckResourceApplies(**declaration, action, resource.Type()); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked = context.Validate(schema, action); !checked) { return std::unexpected{std::move(checked).error()}; }
    return Request{std::move(principal), std::move(action), std::move(resource), std::move(context)};
}

} // namespace cedar
