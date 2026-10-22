#pragma once

#include "validation/Types.hpp"

#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/Value.hpp>

#include <expected>
#include <memory>
#include <utility>
#include <vector>

namespace cedar::detail
{

class NodeTypes;

/// The storage of one residual: an expression tree in `arena` rooted at `root`, with `types[i]` the
/// type of node `i`. The arena holds no Slot node.
struct ResidualTree
{
    ExpressionArena arena;
    std::vector<Type> types;
    ExpressionIndex root;
};

/// What a policy's condition reduced to under partial evaluation (Rust's `tpe::residual::Residual`):
/// an expression in an ExpressionArena, with the type the typechecker gave each node in a parallel
/// vector. A concrete value is a Literal, and Rust's `Residual::Error` is the call `error()` with no
/// arguments, which prints as `error()` and evaluates to an extension-function lookup error.
/// Immutable, and copies share it.
class Residual final
{
public:
    /// The condition of `policy`, as the typechecker typed it in `nodeTypes`, with the policy's slot
    /// bindings substituted (Rust's `Residual::try_from_typed_expr`). An `&&` whose left operand is
    /// typed False becomes that operand, an `||` whose left operand is typed True becomes that
    /// operand, and an `if` whose test is typed True or False keeps the test with the taken branch in
    /// both arms. A folded node becomes the subtree it was written as. Fails with
    /// `partial-evaluation.missing-type-annotation` on an untyped node and
    /// `partial-evaluation.residual-unlinked-slot` on an unbound slot.
    [[nodiscard]] static auto FromTypedPolicy(const Policy& policy, const NodeTypes& nodeTypes)
        -> std::expected<Residual, PartialEvaluationError>;

    /// FromTypedPolicy over the condition of `body`, typed in `nodeTypes`, with its slots bound by
    /// `bindings`, which may leave a slot unbound.
    [[nodiscard]] static auto FromTypedTemplate(const Template& body, const SlotBindings& bindings,
                                                const NodeTypes& nodeTypes)
        -> std::expected<Residual, PartialEvaluationError>;

    /// The residual whose tree is `tree`. Precondition: `tree.types` has one entry per node of
    /// `tree.arena`, and `tree.root` is a node of it.
    [[nodiscard]] static auto Create(ResidualTree tree) -> Residual;

    /// The error residual of type `type` (Rust's `Residual::Error`): the call `error()`.
    [[nodiscard]] static auto Error(Type type) -> Residual;

    [[nodiscard]] auto Arena() const noexcept -> const ExpressionArena& { return tree_->arena; }
    [[nodiscard]] auto Root() const noexcept -> ExpressionIndex { return tree_->root; }

    /// The type of the node at `index`.
    [[nodiscard]] auto TypeOf(const ExpressionIndex index) const noexcept -> const Type&
    {
        return tree_->types[std::to_underlying(index)];
    }

    /// The type of the whole residual (Rust's `Residual::ty`).
    [[nodiscard]] auto GetType() const noexcept -> const Type& { return TypeOf(Root()); }

    /// The value of a concrete residual (a Literal root), or null. The value lives as long as any
    /// copy of this residual.
    [[nodiscard]] auto ConcreteValue() const noexcept -> const Value*;

    /// Whether this is the error residual.
    [[nodiscard]] auto IsError() const noexcept -> bool;

    /// Whether evaluating the residual could error, assuming it is well formed (Rust's
    /// `can_error_assuming_well_formed`).
    [[nodiscard]] auto CanErrorAssumingWellFormed() const -> bool;

    /// Every entity literal in the residual, sorted and without repeats (Rust's `all_literal_uids`).
    [[nodiscard]] auto AllLiteralUids() const -> std::vector<EntityUid>;

    /// The residual policy for `source`: its id and annotations, an unconstrained scope, and one
    /// `when` clause holding this residual (Rust's `Policy::from_when_clause_annos`).
    [[nodiscard]] auto ToPolicy(const Policy& source) const -> Policy;

private:
    explicit Residual(std::shared_ptr<const ResidualTree> tree) noexcept : tree_{std::move(tree)} {}

    std::shared_ptr<const ResidualTree> tree_;
};

/// `error`, the name of the call that stands for an error residual. No extension function has it.
[[nodiscard]] auto ResidualErrorName() -> const Name&;

/// Whether evaluating the subtree of `arena` under `root` could error, assuming it is well formed
/// (Rust's `can_error_assuming_well_formed`). Residual::CanErrorAssumingWellFormed is this over the
/// residual's own tree.
[[nodiscard]] auto CanErrorAssumingWellFormed(const ExpressionArena& arena, ExpressionIndex root) -> bool;

} // namespace cedar::detail
