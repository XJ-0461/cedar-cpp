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
struct ValidatorEntityType;

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

/// The diagnostic for `invalid`: `entities.invalid-enum-entity` or `entities.undeclared-action`.
[[nodiscard]] auto DescribeInvalidEntityUid(const InvalidEntityUid& invalid) -> Diagnostic;

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

/// The declaration of the type of `uid`, an entity that is not an action: the type must be
/// declared and not external, and `uid` one of an enumerated type's ids.
[[nodiscard]] auto CheckEntityDeclaration(const ValidatorSchema& schema, const EntityUid& uid)
    -> std::expected<const ValidatorEntityType*, Diagnostic>;

/// The attributes of `uid`, whose type is `declaration` (Rust's `validate_entity_attributes`):
/// every required attribute present, then each attribute in key order declared (unless the type
/// is open) with a value of its declared type and only valid uids inside.
[[nodiscard]] auto CheckEntityAttributes(const ValidatorSchema& schema, const EntityUid& uid,
                                         const ValidatorEntityType& declaration, Record attributes)
    -> std::expected<void, Diagnostic>;

/// The ancestors of `uid`, whose type is `declaration` (Rust's `validate_entity_ancestors`): each
/// a valid uid of a type `declaration` allows as a parent.
[[nodiscard]] auto CheckEntityAncestors(const ValidatorSchema& schema, const EntityUid& uid,
                                        const ValidatorEntityType& declaration, EntityUidSetView ancestors)
    -> std::expected<void, Diagnostic>;

/// The tags of `uid`, whose type is `declaration` (Rust's `validate_tags`): none unless the type
/// declares tags, each of the tag type with only valid uids inside.
[[nodiscard]] auto CheckEntityTags(const ValidatorSchema& schema, const EntityUid& uid,
                                   const ValidatorEntityType& declaration, Record tags)
    -> std::expected<void, Diagnostic>;

} // namespace cedar::detail
