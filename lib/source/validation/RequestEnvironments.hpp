#pragma once

#include "schema/ValidatorSchema.hpp"
#include "validation/Typechecker.hpp"

#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>

#include <optional>
#include <vector>

namespace cedar::detail
{

/// The entity types slot `slot` of `body` may be linked to when the variable it constrains has
/// type `variableType` (Rust's `possible_slot_links`, `validator/typecheck.rs`): `var == ?slot`
/// only `variableType`; `var in ?slot` and `var is T in ?slot` every type `variableType` may
/// descend from, then `variableType` itself. One nothing when `body` has no such slot.
[[nodiscard]] auto PossibleSlotLinks(const ValidatorSchema& schema, const Template& body, SlotId slot,
                                     const EntityType& variableType, const PrincipalOrResourceConstraint& constraint)
    -> std::vector<std::optional<EntityType>>;

/// Every environment `body` is typechecked in (Rust's `link_request_env` over
/// `unlinked_request_envs`): each request environment of the schema, in its order, linked with
/// each possible principal slot type, then each possible resource slot type.
[[nodiscard]] auto LinkedEnvironments(const ValidatorSchema& schema, const Template& body)
    -> std::vector<TypecheckEnvironment>;

} // namespace cedar::detail
