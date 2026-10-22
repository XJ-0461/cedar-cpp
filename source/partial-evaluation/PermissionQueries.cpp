#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Rbac.hpp"

#include <cedar/Authorizer.hpp>
#include <cedar/PartialEvaluation.hpp>

#include <algorithm>
#include <expected>
#include <optional>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

// The incomplete-schema error, if `schema` leaves a name undefined. Every query checks this before
// it looks at the entities.
[[nodiscard]] auto CheckComplete(const Schema& schema) -> std::expected<void, PermissionQueryError>
{
    if (auto incomplete = detail::IncompleteSchemaError(detail::SchemaAccess::Validator(schema)))
    {
        return std::unexpected{PermissionQueryError{std::move(*incomplete)}};
    }
    return {};
}

// Rust's `query_principal` and `query_resource`: partially authorize the query request, then keep
// the entities of `candidateType`; all of them on Allow, none on Deny, and otherwise each one whose
// concrete request IsAuthorized allows against the residual policies and the concrete entities.
template <typename QueryRequest>
[[nodiscard]] auto QueryEntities(const QueryRequest& request, const EntityType& candidateType,
                                 const PolicySet& policies, const Entities& entities, const Schema& schema)
    -> std::expected<std::vector<EntityUid>, PermissionQueryError>
{
    if (auto complete = CheckComplete(schema); !complete) { return std::unexpected{std::move(complete).error()}; }
    auto partialEntities = PartialEntities::FromConcrete(entities, schema);
    if (!partialEntities) { return std::unexpected{PermissionQueryError{std::move(partialEntities).error()}}; }
    auto response = IsAuthorizedPartial(request.GetPartialRequest(), policies, *partialEntities, schema);
    if (!response) { return std::unexpected{PermissionQueryError{std::move(response).error()}}; }

    std::vector<EntityUid> found;
    const std::optional<Decision> decision = response->GetDecision();
    if (decision == Decision::Deny) { return found; }
    const PolicySet residualPolicies = decision ? PolicySet{} : response->ToPolicySet();
    for (const Entity& entity : entities)
    {
        if (entity.Uid().Type() != candidateType) { continue; }
        if (!decision)
        {
            // A candidate drawn from the entities is valid, so the unchecked request is enough.
            const Request concrete = request.ToRequest(entity.Uid().Id());
            if (IsAuthorized(concrete, residualPolicies, entities).decision != Decision::Allow) { continue; }
        }
        found.push_back(entity.Uid());
    }
    std::ranges::sort(found);
    return found;
}

} // namespace

auto QueryPrincipal(const PrincipalQueryRequest& request, const PolicySet& policies, const Entities& entities,
                    const Schema& schema) -> std::expected<std::vector<EntityUid>, PermissionQueryError>
{
    return QueryEntities(request, request.GetPartialRequest().Principal().type, policies, entities, schema);
}

auto QueryResource(const ResourceQueryRequest& request, const PolicySet& policies, const Entities& entities,
                   const Schema& schema) -> std::expected<std::vector<EntityUid>, PermissionQueryError>
{
    return QueryEntities(request, request.GetPartialRequest().Resource().type, policies, entities, schema);
}

auto QueryAction(const ActionQueryRequest& request, const PolicySet& policies, const PartialEntities& entities)
    -> std::expected<std::vector<ActionQueryResult>, PermissionQueryError>
{
    const Schema& schema = request.GetSchema();
    if (auto complete = CheckComplete(schema); !complete) { return std::unexpected{std::move(complete).error()}; }

    std::vector<ActionQueryResult> found;
    // An external action applies to nothing, so it is never tried.
    for (const detail::ValidatorAction& action : detail::SchemaAccess::Validator(schema).Actions())
    {
        if (!action.AppliesToPrincipal(request.Principal().type) || !action.AppliesToResource(request.Resource().type))
        {
            continue;
        }
        // A context that does not conform to this action's context type means the action is never
        // authorized for the request.
        auto partialRequest =
            PartialRequest::Create(request.Principal(), action.name, request.Resource(), request.GetContext(), schema);
        if (!partialRequest) { continue; }
        auto response = IsAuthorizedPartial(*partialRequest, policies, entities, schema);
        if (!response) { return std::unexpected{PermissionQueryError{std::move(response).error()}}; }
        const std::optional<Decision> decision = response->GetDecision();
        if (decision != Decision::Deny) { found.push_back(ActionQueryResult{.action = action.name, .decision = decision}); }
    }
    std::ranges::sort(found, std::ranges::less{}, &ActionQueryResult::action);
    return found;
}

} // namespace cedar
