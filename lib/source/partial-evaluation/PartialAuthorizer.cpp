#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "partial-evaluation/PartialEvaluator.hpp"
#include "partial-evaluation/Residual.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Rbac.hpp"
#include "validation/Typechecker.hpp"

#include <cedar/PartialEvaluation.hpp>

#include <algorithm>
#include <cassert>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

[[nodiscard]] auto ResultOf(const PartialAuthorizationResponse& response) noexcept
    -> const detail::PartialAuthorizationResult&
{
    return detail::PartialEvaluationAccess::Result(response);
}

[[nodiscard]] auto IsTrue(const detail::Residual& residual) noexcept -> bool
{
    const Value* value = residual.ConcreteValue();
    return value != nullptr && value->Kind() == ValueKind::Bool && value->AsBool();
}

[[nodiscard]] auto IsFalse(const detail::Residual& residual) noexcept -> bool
{
    const Value* value = residual.ConcreteValue();
    return value != nullptr && value->Kind() == ValueKind::Bool && !value->AsBool();
}

// Rust's `Residual::is_partial`: neither a concrete value nor the error residual.
[[nodiscard]] auto IsPartial(const detail::Residual& residual) noexcept -> bool
{
    return residual.ConcreteValue() == nullptr && !residual.IsError();
}

// Rust's `decide`.
[[nodiscard]] auto Decide(const bool trueForbids, const bool truePermits, const bool residualPermits,
                          const bool residualForbids) noexcept -> std::optional<Decision>
{
    if (trueForbids) { return Decision::Deny; }
    if (!truePermits && !residualPermits) { return Decision::Deny; }
    if (residualForbids) { return std::nullopt; }
    if (!truePermits) { return std::nullopt; }
    return Decision::Allow;
}

[[nodiscard]] auto PolicyValidationError(const Template& body, std::vector<Diagnostic> diagnostics)
    -> PartialEvaluationError
{
    return PartialEvaluationError{Diagnostic{.code = DiagnosticCode::PolicyValidation,
                                             .severity = Severity::Error,
                                             .message = "policy failed to validate against the schema",
                                             .help = std::nullopt,
                                             .primary = body.SourceExtent(),
                                             .labels = {},
                                             .source = body.Source()},
                                  std::move(diagnostics)};
}

} // namespace

auto PartialAuthorizationResponse::GetDecision() const noexcept -> std::optional<cedar::Decision>
{
    return ResultOf(*this).decision;
}

auto PartialAuthorizationResponse::Reason() const& noexcept -> std::optional<std::span<const PolicyId>>
{
    const detail::PartialAuthorizationResult& result = ResultOf(*this);
    if (!result.decision) { return std::nullopt; }
    if (*result.decision == cedar::Decision::Allow) { return std::span<const PolicyId>{result.truePermits}; }
    return std::span<const PolicyId>{result.trueForbids};
}

auto PartialAuthorizationResponse::TruePermits() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).truePermits;
}

auto PartialAuthorizationResponse::FalsePermits() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).falsePermits;
}

auto PartialAuthorizationResponse::ErrorPermits() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).errorPermits;
}

auto PartialAuthorizationResponse::ResidualPermits() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).residualPermits;
}

auto PartialAuthorizationResponse::TrueForbids() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).trueForbids;
}

auto PartialAuthorizationResponse::FalseForbids() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).falseForbids;
}

auto PartialAuthorizationResponse::ErrorForbids() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).errorForbids;
}

auto PartialAuthorizationResponse::ResidualForbids() const& noexcept -> std::span<const PolicyId>
{
    return ResultOf(*this).residualForbids;
}

auto PartialAuthorizationResponse::ResidualPolicies() const -> std::vector<Policy>
{
    const detail::PartialAuthorizationResult& result = ResultOf(*this);
    std::vector<Policy> residuals;
    residuals.reserve(result.residualPermits.size() + result.residualForbids.size());
    for (const std::vector<PolicyId>* ids : {&result.residualPermits, &result.residualForbids})
    {
        for (const PolicyId& id : *ids)
        {
            const Policy* policy = GetPolicy(id.Text());
            assert(policy != nullptr);
            residuals.push_back(*policy);
        }
    }
    return residuals;
}

auto PartialAuthorizationResponse::Policies() const -> std::vector<Policy>
{
    return ResultOf(*this).policies;
}

auto PartialAuthorizationResponse::ToPolicySet() const -> PolicySet
{
    PolicySet set;
    for (const Policy& policy : ResultOf(*this).policies)
    {
        // The residual policies have the distinct ids of the source set, so no Add fails.
        [[maybe_unused]] const auto added = set.Add(policy);
        assert(added.has_value());
    }
    return set;
}

auto PartialAuthorizationResponse::GetPolicy(const std::string_view id) const& noexcept -> const Policy*
{
    const std::vector<Policy>& policies = ResultOf(*this).policies;
    const auto found = std::ranges::lower_bound(policies, id, std::ranges::less{},
                                                [](const Policy& policy) { return policy.Id().Text(); });
    return found != policies.end() && found->Id() == id ? &*found : nullptr;
}

auto IsAuthorizedPartial(const PartialRequest& request, const PolicySet& policies, const PartialEntities& entities,
                         const Schema& schema) -> std::expected<PartialAuthorizationResponse, PartialEvaluationError>
{
    // Typechecking a policy needs every name defined.
    if (auto incomplete = detail::IncompleteSchemaError(detail::SchemaAccess::Validator(schema)))
    {
        return std::unexpected{PartialEvaluationError{std::move(*incomplete)}};
    }
    auto residuals = detail::ResidualPolicies(request, policies, entities, schema);
    if (!residuals) { return std::unexpected{std::move(residuals).error()}; }

    // Rust's `Response::new`.
    auto result = std::make_shared<detail::PartialAuthorizationResult>(
        detail::PartialAuthorizationResult{.request = request, .entities = entities, .schema = schema});
    result->policies.reserve(residuals->size());
    for (const detail::ResidualPolicy& residual : *residuals)
    {
        const PolicyId& id = residual.policy.Id();
        const bool permit = residual.policy.GetEffect() == Effect::Permit;
        if (IsTrue(residual.residual)) { (permit ? result->truePermits : result->trueForbids).push_back(id); }
        else if (IsFalse(residual.residual)) { (permit ? result->falsePermits : result->falseForbids).push_back(id); }
        else if (residual.residual.IsError()) { (permit ? result->errorPermits : result->errorForbids).push_back(id); }
        else { (permit ? result->residualPermits : result->residualForbids).push_back(id); }
        result->policies.push_back(residual.residual.ToPolicy(residual.policy));
    }
    std::ranges::sort(result->policies, std::ranges::less{}, [](const Policy& policy) { return policy.Id(); });
    for (std::vector<PolicyId>* ids :
         {&result->truePermits, &result->falsePermits, &result->errorPermits, &result->residualPermits,
          &result->trueForbids, &result->falseForbids, &result->errorForbids, &result->residualForbids})
    {
        std::ranges::sort(*ids);
    }
    result->decision = Decide(!result->trueForbids.empty(), !result->truePermits.empty(),
                              !result->residualPermits.empty(), !result->residualForbids.empty());
    return detail::PartialEvaluationAccess::MakeResponse(std::move(result));
}

namespace detail
{

auto ResidualPolicies(const PartialRequest& request, const PolicySet& policies, const PartialEntities& entities,
                      const Schema& schema) -> std::expected<std::vector<ResidualPolicy>, PartialEvaluationError>
{
    const ValidatorSchema& validatorSchema = SchemaAccess::Validator(schema);
    auto environment = FindRequestEnvironment(request, schema);
    if (!environment) { return std::unexpected{std::move(environment).error()}; }
    const ValidatorAction* action = validatorSchema.FindAction(environment->action);
    assert(action != nullptr);

    std::vector<ResidualPolicy> residuals;
    residuals.reserve(policies.Policies().size());
    for (const std::shared_ptr<const Policy>& policy : policies.Policies())
    {
        const Template& body = policy->GetTemplate();

        // Rust's `validate_entity_types_and_literals`.
        if (std::vector<Diagnostic> errors = ScopeAndLiteralErrors(validatorSchema, body); !errors.empty())
        {
            return std::unexpected{PolicyValidationError(body, std::move(errors))};
        }

        // The environment linked with the types of the entities bound to the policy's slots.
        TypecheckEnvironment linked{environment->principal, action, environment->resource};
        if (const EntityUid* bound = policy->Binding(SlotId::Principal)) { linked.principalSlot = bound->Type(); }
        if (const EntityUid* bound = policy->Binding(SlotId::Resource)) { linked.resourceSlot = bound->Type(); }

        NodeTypes nodeTypes;
        const Typechecker typechecker{validatorSchema, ValidationMode::Strict, body};
        TypecheckResult checked = typechecker.Check(body.Condition(), linked, &nodeTypes);
        if (checked.outcome == PolicyCheckOutcome::Fail || !checked.errors.empty())
        {
            return std::unexpected{PolicyValidationError(body, std::move(checked.errors))};
        }

        auto residual = Residual::FromTypedPolicy(*policy, nodeTypes);
        if (!residual) { return std::unexpected{std::move(residual).error()}; }
        residuals.push_back(ResidualPolicy{*policy, Interpret(*residual, request, entities, schema)});
    }
    return residuals;
}

auto DecisionFromResiduals(const std::span<const ResidualPolicy> residuals) -> std::optional<Decision>
{
    bool trueForbid = false;
    bool truePermit = false;
    bool residualForbid = false;
    bool residualPermit = false;
    for (const ResidualPolicy& residual : residuals)
    {
        if (residual.policy.GetEffect() == Effect::Forbid)
        {
            trueForbid = trueForbid || IsTrue(residual.residual);
            residualForbid = residualForbid || IsPartial(residual.residual);
        }
        else
        {
            truePermit = truePermit || IsTrue(residual.residual);
            residualPermit = residualPermit || IsPartial(residual.residual);
        }
    }
    return Decide(trueForbid, truePermit, residualPermit, residualForbid);
}

} // namespace detail

} // namespace cedar
