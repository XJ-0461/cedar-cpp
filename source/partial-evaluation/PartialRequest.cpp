// Partial requests (Rust's `cedar-policy-core/src/tpe/request.rs`) and the query requests built on
// them (`cedar-policy/src/api/tpe.rs`).

#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "schema/RequestValidation.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"

#include <cedar/PartialEvaluation.hpp>

#include <expected>
#include <format>
#include <optional>
#include <string>
#include <utility>

namespace cedar
{

namespace
{

/// `uid`'s type, then its id when known, as the scope entity check takes them.
[[nodiscard]] auto CheckPartialScopeEntity(const detail::ValidatorSchema& schema, const PartialEntityUid& uid,
                                           const DiagnosticCode undeclared, const std::string_view role)
    -> std::expected<void, RequestValidationError>
{
    return detail::CheckScopeEntity(schema, uid.type, uid.id ? &*uid.id : nullptr, undeclared, role);
}

[[nodiscard]] auto Inconsistent(const DiagnosticCode code, std::string message) -> ReauthorizationError
{
    return ReauthorizationError{Diagnostic{.code = code, .message = std::move(message)}};
}

/// Whether the concrete `concrete` agrees with the partial `partial` (Rust's
/// `PartialEntityUID::check_consistency`). `role` is `principal` or `resource`.
[[nodiscard]] auto CheckUidConsistency(const PartialEntityUid& partial, const EntityUid& concrete,
                                       const DiagnosticCode typeCode, const DiagnosticCode eidCode,
                                       const std::string_view role) -> std::expected<void, ReauthorizationError>
{
    if (concrete.Type() != partial.type)
    {
        return std::unexpected{Inconsistent(typeCode, std::format("partial request {} type `{}` does not match "
                                                                  "concrete request {} type `{}`",
                                                                  role, partial.type.ToString(), role,
                                                                  concrete.Type().ToString()))};
    }
    if (partial.id && *partial.id != concrete.Id())
    {
        return std::unexpected{Inconsistent(eidCode, std::format("partial request {} id `{}` does not match "
                                                                 "concrete request {} id `{}`",
                                                                 role, partial.id->Escaped(), role,
                                                                 concrete.Id().Escaped()))};
    }
    return {};
}

} // namespace

auto PartialRequest::Create(PartialEntityUid principal, EntityUid action, PartialEntityUid resource,
                            std::optional<Context> context, const Schema& schema)
    -> std::expected<PartialRequest, RequestValidationError>
{
    // Rust's `PartialRequest::validate`: the action, the scope types it applies to, the scope
    // entities, then a known context.
    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    auto declaration = detail::CheckRequestAction(validator, action);
    if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
    if (auto checked = detail::CheckPrincipalApplies(**declaration, action, principal.type); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked = detail::CheckResourceApplies(**declaration, action, resource.type); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked =
            CheckPartialScopeEntity(validator, principal, DiagnosticCode::UndeclaredPrincipalType, "principal");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (auto checked =
            CheckPartialScopeEntity(validator, resource, DiagnosticCode::UndeclaredResourceType, "resource");
        !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    if (context)
    {
        if (auto checked = context->Validate(schema, action); !checked)
        {
            return std::unexpected{std::move(checked).error()};
        }
    }
    return PartialRequest{std::move(principal), std::move(action), std::move(resource), std::move(context)};
}

auto PrincipalQueryRequest::Create(EntityType principalType, EntityUid action, EntityUid resource, Context context,
                                   const Schema& schema) -> std::expected<PrincipalQueryRequest, RequestValidationError>
{
    auto request = PartialRequest::Create(PartialEntityUid{.type = std::move(principalType)}, std::move(action),
                                          PartialEntityUid::FromConcrete(std::move(resource)), std::move(context),
                                          schema);
    if (!request) { return std::unexpected{std::move(request).error()}; }
    return PrincipalQueryRequest{*std::move(request)};
}

auto PrincipalQueryRequest::ToRequest(Eid principalId) const -> Request
{
    // Create made the resource and the context known.
    return Request{EntityUid{request_.Principal().type, std::move(principalId)}, request_.Action(),
                   EntityUid{request_.Resource().type, *request_.Resource().id}, *request_.GetContext()};
}

auto PrincipalQueryRequest::ToRequest(Eid principalId, const Schema& schema) const
    -> std::expected<Request, RequestValidationError>
{
    return Request::Create(EntityUid{request_.Principal().type, std::move(principalId)}, request_.Action(),
                           EntityUid{request_.Resource().type, *request_.Resource().id}, *request_.GetContext(),
                           schema);
}

auto ResourceQueryRequest::Create(EntityUid principal, EntityUid action, EntityType resourceType, Context context,
                                  const Schema& schema) -> std::expected<ResourceQueryRequest, RequestValidationError>
{
    auto request = PartialRequest::Create(PartialEntityUid::FromConcrete(std::move(principal)), std::move(action),
                                          PartialEntityUid{.type = std::move(resourceType)}, std::move(context),
                                          schema);
    if (!request) { return std::unexpected{std::move(request).error()}; }
    return ResourceQueryRequest{*std::move(request)};
}

auto ResourceQueryRequest::ToRequest(Eid resourceId) const -> Request
{
    // Create made the principal and the context known.
    return Request{EntityUid{request_.Principal().type, *request_.Principal().id}, request_.Action(),
                   EntityUid{request_.Resource().type, std::move(resourceId)}, *request_.GetContext()};
}

auto ResourceQueryRequest::ToRequest(Eid resourceId, const Schema& schema) const
    -> std::expected<Request, RequestValidationError>
{
    return Request::Create(EntityUid{request_.Principal().type, *request_.Principal().id}, request_.Action(),
                           EntityUid{request_.Resource().type, std::move(resourceId)}, *request_.GetContext(),
                           schema);
}

namespace detail
{

auto FindRequestEnvironment(const PartialRequest& request, const Schema& schema)
    -> std::expected<RequestEnvironment, PartialEvaluationError>
{
    for (const UnlinkedRequestEnvironment& environment : SchemaAccess::Validator(schema).RequestEnvironments())
    {
        if (environment.action->name == request.Action() && environment.principal == request.Principal().type &&
            environment.resource == request.Resource().type)
        {
            return RequestEnvironment{environment.principal, environment.action->name, environment.resource};
        }
    }
    return std::unexpected{PartialEvaluationError{
        Diagnostic{.code = DiagnosticCode::NoMatchingRequestEnvironment,
                   .message = "no request environment in the schema matches the given request"}}};
}

auto CheckRequestConsistency(const PartialRequest& partial, const Request& concrete)
    -> std::expected<void, ReauthorizationError>
{
    if (auto checked = CheckUidConsistency(partial.Principal(), concrete.Principal(),
                                           DiagnosticCode::InconsistentPrincipalType,
                                           DiagnosticCode::InconsistentPrincipalEid, "principal");
        !checked)
    {
        return checked;
    }
    if (auto checked =
            CheckUidConsistency(partial.Resource(), concrete.Resource(), DiagnosticCode::InconsistentResourceType,
                                DiagnosticCode::InconsistentResourceEid, "resource");
        !checked)
    {
        return checked;
    }
    if (concrete.Action() != partial.Action())
    {
        return std::unexpected{Inconsistent(DiagnosticCode::InconsistentAction,
                                            std::format("partial request action `{}` does not match concrete "
                                                        "request action `{}`",
                                                        partial.Action().ToString(), concrete.Action().ToString()))};
    }
    if (partial.GetContext() && partial.GetContext()->AsValue() != concrete.GetContext().AsValue())
    {
        return std::unexpected{Inconsistent(DiagnosticCode::InconsistentContext,
                                            "the partial and concrete request contexts do not match")};
    }
    return {};
}

} // namespace detail

} // namespace cedar
