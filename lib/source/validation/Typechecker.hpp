#pragma once

#include "schema/ValidatorSchema.hpp"
#include "validation/Capabilities.hpp"
#include "validation/Types.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/Validator.hpp>

#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace cedar::detail
{

/// The deepest chain of nested expression nodes the typechecker follows; deeper policies end in
/// the recursion-limit answer, as Rust's typechecker does when its stack runs low.
inline constexpr std::uint32_t MaximumTypecheckDepth = 3000;

/// One request environment a policy is typechecked in (Rust's `RequestEnv::DeclaredAction`): the
/// principal and resource entity types, the action (whose context type is the context's), and,
/// for a template, the entity types its slots are linked to (nothing: an unlinked slot, any
/// entity).
struct TypecheckEnvironment
{
    EntityType principal;
    const ValidatorAction* action;
    EntityType resource;
    std::optional<EntityType> principalSlot{};
    std::optional<EntityType> resourceSlot{};

    /// The environment of `environment` with no slot types.
    [[nodiscard]] static auto FromUnlinked(const UnlinkedRequestEnvironment& environment) -> TypecheckEnvironment
    {
        return TypecheckEnvironment{environment.principal, environment.action, environment.resource};
    }
};

/// What typechecking a policy in one environment concluded (Rust's `PolicyCheck`).
enum class PolicyCheckOutcome : std::uint8_t
{
    /// The condition is a boolean that may be true.
    Success,
    /// The condition is always false in this environment: the policy cannot apply to it. Errors
    /// may still have been found.
    Irrelevant,
    /// The condition does not typecheck.
    Fail,
};

struct TypecheckResult
{
    PolicyCheckOutcome outcome;

    /// The type errors found (50xx codes, Severity::Error, spans into the policy source), in the
    /// order found; one condition may be reported more than once.
    std::vector<Diagnostic> errors;
};

/// The type the typechecker gave each node of an arena while checking one condition in one
/// environment (Rust's type-annotated expression). A node `&&`, `||` or `if` skipped (an operand
/// that cannot be evaluated) was not typechecked; a typechecked node may still have no type (Rust's
/// `None` annotation: a failed operand, or one of an unexpected type).
class NodeTypes final
{
public:
    /// Null when the node at `index` (a folded node's written subtree) was not typechecked, else
    /// its type, nothing when it has none.
    [[nodiscard]] auto Find(const ExpressionIndex index) const noexcept -> const std::optional<Type>*
    {
        const auto position = static_cast<std::size_t>(std::to_underlying(index));
        if (position >= entries_.size() || !entries_[position].typed) { return nullptr; }
        return &entries_[position].type;
    }

    auto Record(const ExpressionIndex index, std::optional<Type> type) -> void
    {
        const auto position = static_cast<std::size_t>(std::to_underlying(index));
        if (position >= entries_.size()) { entries_.resize(position + 1); }
        entries_[position] = Entry{true, std::move(type)};
    }

private:
    struct Entry
    {
        bool typed = false;
        std::optional<Type> type{};
    };
    std::vector<Entry> entries_;
};

/// The flow-sensitive typechecker over one policy body (Rust's `Typechecker` and
/// `SingleEnvTypechecker`, `cedar-policy-core/src/validator/typecheck.rs`). It infers a type for
/// every node, tracking which attributes and tags `has`/`hasTag` tests make safe to read, and
/// singleton boolean types that let `&&`, `||` and `if` skip operands that cannot be evaluated.
/// Request-environment enumeration, slot linking and the scope checks (entity types, actions,
/// enumerated entities, action application) are the validator driver's.
class Typechecker final
{
public:
    /// Typechecks expressions of `arena`, whose spans point into `source`. All three must outlive
    /// this.
    Typechecker(const ValidatorSchema& schema, ValidationMode mode, const ExpressionArena& arena,
                SharedSourceText source);

    /// Typechecks the combined condition of `body`, which must outlive this.
    Typechecker(const ValidatorSchema& schema, ValidationMode mode, const Template& body)
        : Typechecker{schema, mode, body.Arena(), body.Source()}
    {
    }

    /// Typechecks the expression at `root`, which must be boolean, in `environment` (Rust's
    /// `single_env_typechecking`). With `nodeTypes`, also records the type of every node it
    /// typechecks there.
    [[nodiscard]] auto Check(ExpressionIndex root, const TypecheckEnvironment& environment,
                             NodeTypes* nodeTypes = nullptr) const -> TypecheckResult;

private:
    const ValidatorSchema& schema_;
    ValidationMode mode_;
    ExpressionShapes shapes_;
    SharedSourceText source_;
};

} // namespace cedar::detail
