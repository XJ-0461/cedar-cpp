// Batched authorization (Rust's `cedar-policy-core/src/batched_evaluator.rs`): partial
// authorization repeated over entities loaded on demand, and the loader over an entity store
// (Rust's `TestEntityLoader`).

#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "partial-evaluation/PartialEvaluator.hpp"

#include "entities/TransitiveClosure.hpp"
#include "schema/SchemaAccess.hpp"
#include "validation/Rbac.hpp"

#include <cedar/PartialEvaluation.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cedar
{

auto EntityStoreLoader::Load(const std::span<const EntityUid> uids) const -> std::vector<LoadedEntity>
{
    std::vector<LoadedEntity> loaded;
    loaded.reserve(uids.size());
    for (const EntityUid& uid : uids)
    {
        const Entity* const found = store_.Find(uid);
        loaded.push_back(LoadedEntity{
            .uid = uid,
            .entity = found == nullptr ? nullptr : std::make_shared<const Entity>(*found),
        });
    }
    return loaded;
}

auto IsAuthorizedBatched(const Request& request, const PolicySet& policies, const Schema& schema,
                         const EntityLoaderReference loader, const std::uint32_t maximumIterations)
    -> std::expected<Decision, BatchedAuthorizationError>
{
    // The schema is checked before the request.
    if (auto incomplete = detail::IncompleteSchemaError(detail::SchemaAccess::Validator(schema)))
    {
        return std::unexpected{BatchedAuthorizationError{std::move(*incomplete)}};
    }
    auto partialRequest = PartialRequest::Create(PartialEntityUid::FromConcrete(request.Principal()), request.Action(),
                                                 PartialEntityUid::FromConcrete(request.Resource()),
                                                 request.GetContext(), schema);
    if (!partialRequest) { return std::unexpected{BatchedAuthorizationError{std::move(partialRequest).error()}}; }

    // The schema's action entities, which every round can use without loading them.
    auto entities = PartialEntities::Create({}, schema);
    if (!entities) { return std::unexpected{BatchedAuthorizationError{std::move(entities).error()}}; }

    auto residuals = detail::ResidualPolicies(*partialRequest, policies, *entities, schema);
    if (!residuals) { return std::unexpected{BatchedAuthorizationError{std::move(residuals).error()}}; }

    for (std::uint32_t iteration = 0; iteration < maximumIterations; ++iteration)
    {
        if (detail::DecisionFromResiduals(*residuals)) { break; }

        std::vector<EntityUid> toLoad;
        for (const detail::ResidualPolicy& residual : *residuals)
        {
            for (EntityUid& uid : residual.residual.AllLiteralUids())
            {
                if (!entities->Contains(uid)) { toLoad.push_back(std::move(uid)); }
            }
        }
        detail::SortUidSet(toLoad);

        const std::vector<LoadedEntity> loaded = loader.Load(toLoad);
        auto added = detail::AddLoadedEntities(*entities, loaded, schema);
        if (!added) { return std::unexpected{BatchedAuthorizationError{std::move(added).error()}}; }
        *entities = std::move(*added);

        for (detail::ResidualPolicy& residual : *residuals)
        {
            residual.residual = detail::Interpret(residual.residual, *partialRequest, *entities, schema);
        }
    }

    if (auto decision = detail::DecisionFromResiduals(*residuals)) { return *decision; }
    return std::unexpected{BatchedAuthorizationError{
        Diagnostic{.code = DiagnosticCode::InsufficientIterations,
                   .message = "batched authorization reached its iteration limit without a decision"}}};
}

} // namespace cedar
