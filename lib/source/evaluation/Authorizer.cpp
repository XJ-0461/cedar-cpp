#include "evaluation/Evaluator.hpp"
#include "evaluation/ScopeIndex.hpp"
#include "model/PolicySetAccess.hpp"

#include <cedar/Authorizer.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

// Gathers the response one evaluated policy at a time. `reason` collects the satisfied permits
// until the first satisfied forbid, then only forbids.
class ResponseBuilder final
{
public:
    ResponseBuilder(const Request& request, const EntityStoreView entities) noexcept
        : request_{request}, entities_{entities}
    {
    }

    auto Evaluate(const Policy& policy) -> void
    {
        std::expected<bool, EvaluationError> satisfied = detail::EvaluateCondition(policy, request_, entities_);
        if (!satisfied)
        {
            response_.errors.push_back(PolicyEvaluationError{policy.Id(), std::move(satisfied).error()});
            return;
        }
        if (!*satisfied) { return; }
        if (policy.GetEffect() == Effect::Forbid)
        {
            if (!anyForbidSatisfied_)
            {
                anyForbidSatisfied_ = true;
                response_.reason.clear();
            }
            response_.reason.push_back(policy.Id());
        }
        else if (!anyForbidSatisfied_) { response_.reason.push_back(policy.Id()); }
    }

    auto Finish() && -> Response
    {
        response_.decision = !anyForbidSatisfied_ && !response_.reason.empty() ? Decision::Allow : Decision::Deny;
        std::ranges::sort(response_.reason);
        std::ranges::sort(response_.errors, {}, &PolicyEvaluationError::policyId);
        return std::move(response_);
    }

private:
    const Request& request_;
    EntityStoreView entities_;
    Response response_;
    bool anyForbidSatisfied_ = false;
};

} // namespace

// Rust `Authorizer::is_authorized_core_internal` (`cedar-policy-core/src/authorizer.rs:95`) with
// the decision of `PartialResponse` (`authorizer/partial_response.rs`). An evaluation error is
// recorded and the policy counts as not satisfied (Rust's only `ErrorHandling`, Skip). The
// decision is Allow when some permit holds and no forbid does.
//
// With ScopeIndex only the policies whose scope matches are evaluated. The response is the same
// as evaluating every policy: a condition is `principal && (action && (resource && nonScope))`,
// the scope parts only compare entity uids and types and look up ancestors, which cannot fail, and
// `&&` stops at the first false part, so a policy whose scope does not match evaluates to false
// without ever reaching the part that could fail.
auto IsAuthorized(const Request& request, const PolicySet& policies, const EntityStoreView entities,
                  const AuthorizerOptions options) -> Response
{
    ResponseBuilder builder{request, entities};
    const std::span<const std::shared_ptr<const Policy>> all = policies.Policies();
    const detail::ScopeIndex* const index = options.policySelection == PolicySelection::ScopeIndex
                                                ? detail::PolicySetAccess::GetScopeIndex(policies)
                                                : nullptr;
    if (index == nullptr)
    {
        for (const std::shared_ptr<const Policy>& policy : all) { builder.Evaluate(*policy); }
        return std::move(builder).Finish();
    }

    detail::PolicyBits candidates;
    detail::PolicyBits scratch;
    index->Candidates(request, entities, candidates, scratch);
    for (std::size_t word = 0; word < candidates.size(); ++word)
    {
        for (std::uint64_t bits = candidates[word]; bits != 0; bits &= bits - 1)
        {
            builder.Evaluate(*all[(word * 64) + static_cast<std::size_t>(std::countr_zero(bits))]);
        }
    }
    return std::move(builder).Finish();
}

} // namespace cedar
