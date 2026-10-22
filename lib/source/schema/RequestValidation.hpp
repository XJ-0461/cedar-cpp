#pragma once

// The request checks Request::Create makes, which PartialRequest::Create makes in another order
// (Rust's `get_defined_action_id`, `check_principal_type`, `check_resource_type` and the scope
// entity checks of `cedar-policy-core/src/validator/coreschema.rs`).

#include "schema/ValidatorSchema.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>

#include <expected>
#include <string_view>

namespace cedar::detail
{

/// The declaration of `action`, which must be declared (`request.undeclared-action`) and not
/// external (`request.external-action`).
[[nodiscard]] auto CheckRequestAction(const ValidatorSchema& schema, const EntityUid& action)
    -> std::expected<const ValidatorAction*, RequestValidationError>;

/// Whether `declaration`, the declaration of `action`, applies to principal type `type`
/// (`request.invalid-principal-type`).
[[nodiscard]] auto CheckPrincipalApplies(const ValidatorAction& declaration, const EntityUid& action,
                                         const EntityType& type) -> std::expected<void, RequestValidationError>;

/// Whether `declaration`, the declaration of `action`, applies to resource type `type`
/// (`request.invalid-resource-type`).
[[nodiscard]] auto CheckResourceApplies(const ValidatorAction& declaration, const EntityUid& action,
                                        const EntityType& type) -> std::expected<void, RequestValidationError>;

/// A principal or resource of type `type`: the type must be declared (`undeclared`), and when `id`
/// is not null and the type is enumerated, `id` must be one of its ids. `role` names it in the
/// message.
[[nodiscard]] auto CheckScopeEntity(const ValidatorSchema& schema, const EntityType& type, const Eid* id,
                                    DiagnosticCode undeclared, std::string_view role)
    -> std::expected<void, RequestValidationError>;

} // namespace cedar::detail
