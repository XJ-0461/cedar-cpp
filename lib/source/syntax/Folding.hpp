#pragma once

#include "extensions/ExtensionCalls.hpp"

#include <cedar/Expression.hpp>

#include <optional>
#include <span>

namespace cedar::detail
{

// Read-time folding, shared by the Cedar text parser and the JSON policy reader so that a policy
// evaluates identically whichever form it was read from. Rust keeps every call and set literal (it
// folds only `&&`/`||` of two bool literals), so a folded node keeps the subtree as written: the
// printers and the JSON writer read `written`, and the evaluator reads the value it would have
// computed.

/// The value of a node that is already a constant: a literal or a folded node. Null otherwise.
[[nodiscard]] auto ConstantValue(const ExpressionArena& arena, ExpressionIndex index) -> const Value*;

/// `call`, the call of `function` on `arguments` already built in `arena`, folded when `function`
/// is a constructor (`ip("10.0.0.1")`) called on literals of its argument kinds and its body
/// succeeds. A call that would fail (wrong argument count or kind, text that does not parse) stays
/// a call, so its error surfaces at evaluation, where Rust raises it.
[[nodiscard]] auto FoldConstructorCall(const ExpressionArena& arena, ExpressionBuilder builder,
                                       const ExtensionCall& function, ExpressionIndex call,
                                       std::span<const ExpressionIndex> arguments, std::optional<SourceSpan> span)
    -> ExpressionIndex;

/// `set`, the set literal of `elements` already built in `arena`, folded when every element is a
/// constant: the folded value is sorted with duplicates removed, and `written` keeps the elements
/// as written.
[[nodiscard]] auto FoldConstantSet(const ExpressionArena& arena, ExpressionBuilder builder, ExpressionIndex set,
                                   std::span<const ExpressionIndex> elements, std::optional<SourceSpan> span)
    -> ExpressionIndex;

} // namespace cedar::detail
