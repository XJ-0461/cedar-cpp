#pragma once

#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Policy.hpp>
#include <cedar/Request.hpp>
#include <cedar/Value.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>

namespace cedar::detail
{

/// The deepest chain of nested expression nodes the evaluator follows before it reports
/// RecursionLimit. Rust stops when less than 100 KB of stack remains, which on the CLI's main
/// thread is between 2,000 and 2,500 nested additions; the limit here is above that.
inline constexpr std::uint32_t MaximumEvaluationDepth = 3000;

/// The size of the stack buffer each evaluation gives its ValueArena. Set and record literals
/// that are not folded at parse time are built in it; a larger one spills to the heap.
inline constexpr std::size_t EvaluationArenaBufferBytes = 1024;

/// What the two slots evaluate to: the policy's bindings, or null for an unlinked slot.
struct SlotValues
{
    const EntityUid* principal{nullptr};
    const EntityUid* resource{nullptr};
};

/// Evaluates expressions of one arena against one request and entity store, as Rust's
/// `Evaluator::interpret` (`cedar-policy-core/src/evaluator.rs`). Values it returns are views
/// into the request, the entity store, the expression arena, or `arena` (set and record literals),
/// so they are valid while all four live.
class Evaluator final
{
public:
    using Result = std::expected<Value, EvaluationError>;

    Evaluator(const Request& request, EntityStoreView entities, const ExpressionArena& expressions, SlotValues slots,
              ValueArena& arena) noexcept
        : request_{request}, entities_{entities}, expressions_{expressions}, slots_{slots}, arena_{arena}
    {
    }

    Evaluator(const Evaluator&) = delete;
    auto operator=(const Evaluator&) -> Evaluator& = delete;

    /// The value of the expression rooted at `index`. An error carries the span of the innermost
    /// node that has one (Rust `with_maybe_source_loc`); a type error points at the operand whose
    /// value had the wrong type.
    [[nodiscard]] auto Evaluate(ExpressionIndex index) -> Result;

private:
    // One per node type; Evaluate dispatches to them.
    [[nodiscard]] auto EvaluateNode(const node::Literal& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Folded& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Variable& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Slot& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::If& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::And& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Or& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Unary& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Binary& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::ExtensionCall& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::GetAttribute& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::HasAttribute& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Like& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::Is& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::SetLiteral& node) -> Result;
    [[nodiscard]] auto EvaluateNode(const node::RecordLiteral& node) -> Result;

    const Request& request_;
    EntityStoreView entities_;
    const ExpressionArena& expressions_;
    SlotValues slots_;
    ValueArena& arena_;
    std::uint32_t depth_{0};
};

/// Whether `policy`'s condition holds for `request` (Rust `Evaluator::evaluate`): the condition
/// evaluated with the policy's slot bindings, which must be a bool. The authorizer calls this once
/// per policy; nothing is converted to an OwnedValue and literal composites live on the stack.
[[nodiscard]] auto EvaluateCondition(const Policy& policy, const Request& request, EntityStoreView entities)
    -> std::expected<bool, EvaluationError>;

} // namespace cedar::detail
