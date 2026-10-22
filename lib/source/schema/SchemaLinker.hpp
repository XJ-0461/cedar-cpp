#pragma once

#include "schema/SchemaFragmentModel.hpp"
#include "schema/ValidatorSchema.hpp"

#include <cedar/Errors.hpp>

#include <expected>
#include <memory>
#include <span>

namespace cedar::detail
{

/// Links fragments into one schema (Rust's `ValidatorSchema::from_schema_fragments`,
/// `cedar-policy-core/src/validator/schema.rs`). The `__cedar` built-ins (`Bool`, `Long`, `String`
/// and the extension types) are added, and aliased in the empty namespace unless a declaration
/// there takes the name. Then, in Rust's order, each step failing with every error it finds:
///   1. no unqualified entity type, common type or action shares its basename (or action id) with
///      a namespaced one (RFC 70: `schema.type-shadowing`, `schema.action-shadowing`);
///   2. every type and action reference resolves: an unqualified name means the enclosing
///      namespace's declaration if there is one, else the empty namespace's; a common type wins
///      over an entity type of the same name (RFC 24); `schema.type-not-defined`,
///      `schema.action-not-defined`;
///   3. no name is declared twice across fragments (`schema.duplicate-*`);
///   4. common types are inlined; a cycle among them is `schema.cycle-in-common-type-references`;
///      an unknown extension type and a record with `additionalAttributes` are refused;
///   5. entity shapes and action contexts are records (`schema.context-or-shape-not-record`);
///   6. the hierarchies are closed: a cycle among entity types is allowed, one among actions is
///      `schema.cycle-in-action-hierarchy`; an external entity type may not descend from an
///      enumerated one; a parent type or `appliesTo` type naming an action type is
///      `schema.undeclared-entity-types`.
[[nodiscard]] auto LinkSchema(std::span<const SchemaFragmentModel* const> fragments)
    -> std::expected<std::shared_ptr<const ValidatorSchema>, SchemaError>;

} // namespace cedar::detail
