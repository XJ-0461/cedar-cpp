#include "entities/Conformance.hpp"
#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "schema/SchemaAccess.hpp"

#include <cedar/PartialEvaluation.hpp>

#include <expected>
#include <utility>

namespace cedar
{

auto PartialAuthorizationResponse::Reauthorize(const Request& request, const Entities& entities) const
    -> std::expected<Response, ReauthorizationError>
{
    const detail::PartialAuthorizationResult& result = *result_;

    // Rust's `validate_request`, the checks `Request::Create` makes.
    if (auto validated = Request::Create(request.Principal(), request.Action(), request.Resource(),
                                         request.GetContext(), result.schema);
        !validated)
    {
        return std::unexpected{ReauthorizationError{std::move(validated).error()}};
    }

    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(result.schema);
    for (const Entity& entity : entities)
    {
        if (auto checked = detail::CheckEntityConformance(validator, entity); !checked)
        {
            return std::unexpected{ReauthorizationError{std::move(checked).error()}};
        }
    }

    if (auto consistent = detail::CheckEntitiesConsistency(result.entities, entities); !consistent)
    {
        return std::unexpected{std::move(consistent).error()};
    }
    if (auto consistent = detail::CheckRequestConsistency(result.request, request); !consistent)
    {
        return std::unexpected{std::move(consistent).error()};
    }

    const PolicySet policies = ToPolicySet();
    return IsAuthorized(request, policies, entities, AuthorizerOptions{PolicySelection::EvaluateEveryPolicy});
}

} // namespace cedar
