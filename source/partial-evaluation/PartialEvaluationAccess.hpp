#pragma once

#include "partial-evaluation/Residual.hpp"

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/PartialEvaluation.hpp>
#include <cedar/Policy.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cedar::detail
{

/// One policy after partial evaluation (Rust's `ResidualPolicy`): the source policy and what its
/// condition reduced to.
struct ResidualPolicy
{
    Policy policy;
    Residual residual;
};

/// What a partial authorization produced, shared by every copy of its response. The eight lists and
/// `policies` are sorted by policy id.
struct PartialAuthorizationResult
{
    PartialRequest request;
    PartialEntities entities;
    Schema schema;
    std::optional<Decision> decision{};
    std::vector<Policy> policies{};
    std::vector<PolicyId> truePermits{};
    std::vector<PolicyId> falsePermits{};
    std::vector<PolicyId> errorPermits{};
    std::vector<PolicyId> residualPermits{};
    std::vector<PolicyId> trueForbids{};
    std::vector<PolicyId> falseForbids{};
    std::vector<PolicyId> errorForbids{};
    std::vector<PolicyId> residualForbids{};
};

/// Makes and opens the partial evaluation handles, whose constructors are private.
struct PartialEvaluationAccess
{
    [[nodiscard]] static auto MakeRequest(PartialEntityUid principal, EntityUid action, PartialEntityUid resource,
                                          std::optional<Context> context) noexcept -> PartialRequest
    {
        return PartialRequest{std::move(principal), std::move(action), std::move(resource), std::move(context)};
    }

    [[nodiscard]] static auto MakeEntity(EntityUid uid, std::optional<Record> attributes,
                                         std::optional<std::vector<EntityUid>> ancestors, std::optional<Record> tags,
                                         std::shared_ptr<const ValueArena> storage) noexcept -> PartialEntity
    {
        return PartialEntity{std::move(uid), attributes, std::move(ancestors), tags, std::move(storage)};
    }

    /// The entity's ancestor set, writable, for closing it.
    [[nodiscard]] static auto Ancestors(PartialEntity& entity) noexcept -> std::optional<std::vector<EntityUid>>&
    {
        return entity.ancestors_;
    }

    [[nodiscard]] static auto MakeEntities(std::shared_ptr<const std::vector<PartialEntity>> entities) noexcept
        -> PartialEntities
    {
        return PartialEntities{std::move(entities)};
    }

    [[nodiscard]] static auto Table(const PartialEntities& entities) noexcept
        -> const std::shared_ptr<const std::vector<PartialEntity>>&
    {
        return entities.entities_;
    }

    [[nodiscard]] static auto MakeResponse(std::shared_ptr<const PartialAuthorizationResult> result) noexcept
        -> PartialAuthorizationResponse
    {
        return PartialAuthorizationResponse{std::move(result)};
    }

    [[nodiscard]] static auto Result(const PartialAuthorizationResponse& response) noexcept
        -> const PartialAuthorizationResult&
    {
        return *response.result_;
    }
};

// ---- Defined in PartialRequest.cpp ----

/// The one request environment of `schema` that `request` names (Rust's `find_request_env`), or
/// `partial-evaluation.no-matching-request-environment`.
[[nodiscard]] auto FindRequestEnvironment(const PartialRequest& request, const Schema& schema)
    -> std::expected<RequestEnvironment, PartialEvaluationError>;

/// Whether `concrete` agrees with every known part of `partial` (Rust's
/// `PartialRequest::check_consistency`), or the first `partial-evaluation.inconsistent-*` error.
[[nodiscard]] auto CheckRequestConsistency(const PartialRequest& partial, const Request& concrete)
    -> std::expected<void, ReauthorizationError>;

// ---- Defined in PartialEntities.cpp ----

/// Whether `concrete` agrees with every known part of `partial` (Rust's
/// `PartialEntities::check_consistency`), or the first `partial-evaluation.*` consistency error.
[[nodiscard]] auto CheckEntitiesConsistency(const PartialEntities& partial, const Entities& concrete)
    -> std::expected<void, ReauthorizationError>;

/// `entities` with the loaded entities added, their ancestor sets taken as already closed; a null
/// entity is added as one with no attributes, ancestors or tags (Rust's `add_entities` and
/// `add_entity_trusted` in `batched_evaluator.rs`).
[[nodiscard]] auto AddLoadedEntities(const PartialEntities& entities, std::span<const LoadedEntity> loaded,
                                     const Schema& schema) -> std::expected<PartialEntities, PartialEntitiesError>;

// ---- Defined in PartialAuthorizer.cpp ----

/// Typechecks and partially evaluates every policy of `policies` (Rust's `residual_policies`), in
/// policy set order.
[[nodiscard]] auto ResidualPolicies(const PartialRequest& request, const PolicySet& policies,
                                    const PartialEntities& entities, const Schema& schema)
    -> std::expected<std::vector<ResidualPolicy>, PartialEvaluationError>;

/// The decision `residuals` settle, if any (Rust's `decision_from_residuals`).
[[nodiscard]] auto DecisionFromResiduals(std::span<const ResidualPolicy> residuals) -> std::optional<Decision>;

} // namespace cedar::detail
