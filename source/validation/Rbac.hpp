#pragma once

#include "schema/ValidatorSchema.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Policy.hpp>

#include <optional>
#include <vector>

namespace cedar::detail
{

// The checks the validator runs beside the typechecker, once per policy rather than once per
// request environment (Rust's `validator/rbac.rs`). Diagnostics point into the template's source;
// an entity uid in the scope has no span of its own, so its diagnostic points at the policy.

/// Every entity type in `body` (scope and condition: entity literals and `is` types) that the
/// schema does not declare (`validation.unrecognized-entity-type`), then every entity literal of
/// an enumerated type whose id is not one of the type's ids (`validation.invalid-enum-entity`),
/// then every action literal the schema does not declare (`validation.unrecognized-action-id`)
/// (Rust's `validate_entity_types`, `validate_enum_entity`, `validate_action_ids`).
[[nodiscard]] auto ScopeAndLiteralErrors(const ValidatorSchema& schema, const Template& body)
    -> std::vector<Diagnostic>;

/// The `validation.invalid-action-application` warning when no action the action constraint
/// admits applies to both a principal type and a resource type that satisfy the principal and
/// resource constraints (Rust's `validate_action_application`). A constraint on a slot admits
/// every entity type.
[[nodiscard]] auto ActionApplicationWarning(const ValidatorSchema& schema, const PrincipalConstraint& principal,
                                            const ActionConstraint& action, const ResourceConstraint& resource,
                                            const Template& body) -> std::optional<Diagnostic>;

/// A `validation.unrecognized-entity-type` error for each entity bound to a slot of `policy` whose
/// type the schema does not declare (Rust's `validate_entity_types_in_slots`), principal first.
[[nodiscard]] auto SlotEntityTypeErrors(const ValidatorSchema& schema, const Policy& policy)
    -> std::vector<Diagnostic>;

/// The `validation.incomplete-schema` error, with no source, when `schema` declares something
/// `external` (Rust's `check_complete`), naming every such declaration.
[[nodiscard]] auto IncompleteSchemaError(const ValidatorSchema& schema) -> std::optional<Diagnostic>;

/// The `validation.incomplete-schema` error for `body` when `schema` declares something
/// `external` (Rust's `check_complete`), naming every such declaration.
[[nodiscard]] auto IncompleteSchemaError(const ValidatorSchema& schema, const Template& body)
    -> std::optional<Diagnostic>;

} // namespace cedar::detail
