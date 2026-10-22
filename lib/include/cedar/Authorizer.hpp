#pragma once

#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Value.hpp>

#include <cstdint>
#include <expected>
#include <vector>

namespace cedar
{

/// The answer to an authorization request.
enum class Decision : std::uint8_t
{
    Allow,
    Deny,
};

/// A policy whose condition failed to evaluate (Rust's `AuthorizationError::PolicyEvaluationError`).
/// An erroring policy is treated as not satisfied.
struct PolicyEvaluationError
{
    PolicyId policyId;
    EvaluationError error;
};

/// The result of an authorization request (Rust's `Response` with its `Diagnostics`).
///
/// `reason` holds the determining policies: the satisfied forbid policies when any forbid policy is
/// satisfied (the decision is then Deny), otherwise the satisfied permit policies (Allow when there
/// is at least one, Deny with an empty reason when there is none). `errors` holds every policy that
/// failed to evaluate, whatever the decision. Both are sorted by policy id, so a response does not
/// depend on the order policies were evaluated in.
struct Response
{
    Decision decision{Decision::Deny};
    std::vector<PolicyId> reason{};
    std::vector<PolicyEvaluationError> errors{};
};

/// How the authorizer chooses which policies to evaluate.
enum class PolicySelection : std::uint8_t
{
    /// Evaluate every policy's full condition. The reference behavior.
    EvaluateEveryPolicy,
    /// Evaluate only the policies whose scope can match the request, found through an index of the
    /// policy set's scope constraints. A policy whose scope cannot match is not satisfied and
    /// cannot error, so the response is the same as EvaluateEveryPolicy's. The index is built on
    /// the first such call for a set and kept until the set changes; calls from any number of
    /// threads on an unchanging set share it.
    ScopeIndex,
};

/// Options for IsAuthorized. Policies are selected through the scope index unless the caller asks
/// for EvaluateEveryPolicy.
struct AuthorizerOptions
{
    PolicySelection policySelection{PolicySelection::ScopeIndex};
};

/// Answers `request` against `policies`, looking entities up in `entities` (Rust's
/// `Authorizer::is_authorized`). Any EntityStore (an `Entities`, or a caller's own store) converts
/// to the view, so one compiled authorizer serves every store type. The store and the request must
/// outlive the call.
[[nodiscard]] auto IsAuthorized(const Request& request, const PolicySet& policies, EntityStoreView entities,
                                AuthorizerOptions options = {}) -> Response;

/// Evaluates `expression` in the environment of `request`, looking entities up in `entities`
/// (Rust's `eval_expression`). The expression must not contain slots; a slot evaluates to an
/// `evaluation.unlinked-slot` error. The result owns its storage.
[[nodiscard]] auto EvaluateExpression(const Request& request, EntityStoreView entities, const Expression& expression)
    -> std::expected<OwnedValue, EvaluationError>;

} // namespace cedar
