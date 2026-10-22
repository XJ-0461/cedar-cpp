#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Names.hpp>
#include <cedar/Value.hpp>

#include <expected>

namespace cedar::detail
{

// Checks of entity and request data against a linked schema (Rust's
// `cedar-policy-core/src/entities/conformance.rs` over the `CoreSchema` adapter of
// `validator/coreschema.rs`, and the validator's `Type::typecheck_value`).

class ValidatorSchema;
struct Type;

/// Whether `value` has the schema type `type`: the same kind, a set whose elements all have the
/// element type, a record with every required attribute and only declared ones (unless the record
/// type is open) each of its type, an entity of one of the entity types, an extension value of
/// the extension type (Rust's `typecheck_restricted_expr_against_schematype`, which for the types
/// a schema states agrees with `Type::typecheck_restricted_expr`).
[[nodiscard]] auto HasType(const Value& value, const Type& type) noexcept -> bool;

/// What is wrong with an entity uid that appears in entity or request data (Rust's
/// `ValidateEuidError`).
enum class EntityUidProblem : std::uint8_t
{
    /// An entity of an enumerated type whose id is not one of the type's ids.
    NotAnEnumChoice,
    /// An action the schema does not declare.
    UndeclaredAction,
};

struct InvalidEntityUid
{
    EntityUid uid;
    EntityUidProblem problem;
};

/// Checks one uid (Rust's `validate_euid`).
[[nodiscard]] auto CheckEntityUid(const ValidatorSchema& schema, const EntityUid& uid)
    -> std::expected<void, InvalidEntityUid>;

/// Checks every uid inside `value`, at any depth (Rust's `validate_euids_in_partial_value`).
[[nodiscard]] auto CheckEntityUidsIn(const ValidatorSchema& schema, const Value& value)
    -> std::expected<void, InvalidEntityUid>;

/// Checks `entity` against its declaration (Rust's `EntitySchemaConformanceChecker::validate_entity`):
/// an action must be declared and equal the schema's action entity (attributes, tags and ancestor
/// set); any other entity's type must be declared and not external, its id one of an enumerated
/// type's ids, its attributes the declared ones with the declared types, its ancestors of the
/// allowed parent types and its tags of the tag type. The diagnostic has an `entities.*`
/// conformance code.
[[nodiscard]] auto CheckEntityConformance(const ValidatorSchema& schema, const Entity& entity)
    -> std::expected<void, Diagnostic>;

} // namespace cedar::detail
