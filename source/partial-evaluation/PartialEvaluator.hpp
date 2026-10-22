#pragma once

#include "partial-evaluation/Residual.hpp"

#include <cedar/PartialEvaluation.hpp>
#include <cedar/Schema.hpp>

namespace cedar::detail
{

/// Evaluates `residual` as far as `request` and `entities` allow (Rust's `tpe::evaluator::Evaluator`
/// and its `interpret`), applying operators to concrete operands through the concrete evaluator's
/// operator functions and reducing `&&`, `||` and `if` by the node types (the three `try_decide_*`
/// reductions). A chain deeper than MaximumEvaluationDepth becomes an error residual.
[[nodiscard]] auto Interpret(const Residual& residual, const PartialRequest& request,
                             const PartialEntities& entities, const Schema& schema) -> Residual;

} // namespace cedar::detail
