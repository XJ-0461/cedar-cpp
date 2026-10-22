#pragma once

#include "schema/ValidatorSchema.hpp"
#include "validation/Typechecker.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Policy.hpp>

#include <cstdint>
#include <vector>

namespace cedar::detail
{

/// Level validation of `body` in one environment (RFC 76, Rust's `LevelChecker` in
/// `cedar-policy-core/src/validator/level_validate.rs`), over the node types the typechecker
/// recorded there (`nodeTypes`; the environment's check did not fail). Every attribute access,
/// `has`, `in`, `hasTag` and `getTag` on an entity is a dereference; one reached from a request
/// variable through more than `maximumLevel` dereferences is a
/// `validation.entity-dereference-level-violation` error, and so is dereferencing an entity
/// literal (other than the environment's action) at any level. Appends to `errors` each error not
/// already there.
auto CheckDereferenceLevels(const ValidatorSchema& schema, const Template& body,
                            const TypecheckEnvironment& environment, const NodeTypes& nodeTypes,
                            std::uint32_t maximumLevel, std::vector<Diagnostic>& errors) -> void;

} // namespace cedar::detail
