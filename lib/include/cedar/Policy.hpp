#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

// ---- PolicyId ----

/// The id of a static policy, a template or a template-linked policy: any string (Rust's
/// `PolicyID`). Ids come from user input, so they are owned strings, not interned symbols.
class PolicyId final
{
public:
    explicit PolicyId(std::string text) noexcept : text_{std::move(text)} {}
    explicit PolicyId(const std::string_view text) : text_{text} {}
    explicit PolicyId(const char* text) : text_{text} {}

    [[nodiscard]] auto Text() const noexcept -> std::string_view { return text_; }

    friend auto operator==(const PolicyId&, const PolicyId&) noexcept -> bool = default;
    friend auto operator==(const PolicyId& id, const std::string_view text) noexcept -> bool
    {
        return id.text_ == text;
    }
    friend auto operator<=>(const PolicyId&, const PolicyId&) noexcept -> std::strong_ordering = default;

private:
    std::string text_;
};

// ---- Effect ----

/// Whether a satisfied policy permits or forbids the request.
enum class Effect : std::uint8_t
{
    Permit,
    Forbid,
};

/// `permit` or `forbid`.
[[nodiscard]] constexpr auto ToString(const Effect effect) noexcept -> std::string_view
{
    return effect == Effect::Permit ? "permit" : "forbid";
}

// ---- Annotations ----

/// One annotation, `@key("value")`. An annotation written without a value (`@key`) has no value
/// here, so the JSON form can write it back as `null`; Rust's AST stores the empty string instead
/// (`Annotation::with_optional_value`), which is what Annotations::Find returns for it.
struct Annotation
{
    Symbol key;
    std::optional<std::string> value;

    friend auto operator==(const Annotation&, const Annotation&) -> bool = default;
};

/// A policy's annotations, ordered by key text (Rust's `BTreeMap<AnyId, Annotation>`). Keys have
/// identifier syntax and may be reserved keywords (`@if("x")` is allowed); each key appears once.
class Annotations final
{
public:
    /// Adds `@key(value)`, or `@key` when `value` is nothing. Returns false, changing nothing, when
    /// `key` is already present. The caller has checked that `key` has identifier syntax
    /// (IsIdentifierSyntax).
    [[nodiscard]] auto Insert(Symbol key, std::optional<std::string> value) -> bool;

    /// The value of `key` (the empty string for `@key` written without a value, as Rust's
    /// `annotation()` returns), or nothing when the policy has no such annotation.
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> std::optional<std::string_view>;

    [[nodiscard]] auto Entries() const noexcept -> std::span<const Annotation> { return entries_; }
    [[nodiscard]] auto Size() const noexcept -> std::size_t { return entries_.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return entries_.empty(); }
    [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] auto end() const noexcept { return entries_.end(); }

    /// The same keys with the same values; `@key` differs from `@key("")`.
    friend auto operator==(const Annotations&, const Annotations&) -> bool = default;

private:
    std::vector<Annotation> entries_;
};

// ---- Scope constraints ----

/// A slot in a scope constraint, `?principal` or `?resource`; which one is implied by the
/// constraint it appears in.
struct SlotReference
{
    std::optional<SourceSpan> span{};
};

/// The right-hand side of a principal or resource constraint: an entity literal or the slot.
using EntityReference = std::variant<EntityUid, SlotReference>;

/// `principal` with no constraint.
struct ScopeAny
{
};

/// `principal == target`.
struct ScopeEqual
{
    EntityReference target;
};

/// `principal in target`.
struct ScopeIn
{
    EntityReference target;
};

/// `principal is type`.
struct ScopeIs
{
    EntityType type;
};

/// `principal is type in target`.
struct ScopeIsIn
{
    EntityType type;
    EntityReference target;
};

/// A principal or resource scope constraint (Rust's `PrincipalOrResourceConstraint`).
using PrincipalOrResourceConstraint = std::variant<ScopeAny, ScopeEqual, ScopeIn, ScopeIs, ScopeIsIn>;

/// The principal part of a policy scope.
struct PrincipalConstraint
{
    PrincipalOrResourceConstraint constraint{ScopeAny{}};
};

/// The resource part of a policy scope.
struct ResourceConstraint
{
    PrincipalOrResourceConstraint constraint{ScopeAny{}};
};

/// `action` with no constraint.
struct ActionAny
{
};

/// `action == Action::"view"`.
struct ActionEqual
{
    EntityUid action;
};

/// `action in [a1, a2, ...]`, in source order. `action in A` is the one-element list with
/// `writtenAsList` false, so the printers and the JSON writer can give back the form written (the
/// JSON form distinguishes `"entity"` from `"entities"`); the two forms mean the same.
struct ActionIn
{
    std::vector<EntityUid> actions;
    bool writtenAsList = true;
};

/// The action part of a policy scope (Rust's `ActionConstraint`). Slots are not allowed here.
using ActionConstraint = std::variant<ActionAny, ActionEqual, ActionIn>;

// ---- Slot bindings ----

/// The entity bound to each slot of a template-linked policy (Rust's `SlotEnv`). Cedar has exactly
/// two slots, so this is a fixed pair of optional uids.
class SlotBindings final
{
public:
    SlotBindings() = default;

    /// Binds `slot` to `uid`, replacing any earlier binding.
    auto Bind(const SlotId slot, EntityUid uid) -> SlotBindings&
    {
        bindings_[Position(slot)] = std::move(uid);
        return *this;
    }

    /// The uid bound to `slot`, or null when it is unbound. The pointer is valid while the
    /// bindings live and are not rebound.
    [[nodiscard]] auto Get(const SlotId slot) const noexcept -> const EntityUid*
    {
        const std::optional<EntityUid>& binding = bindings_[Position(slot)];
        return binding ? &*binding : nullptr;
    }

    [[nodiscard]] auto Contains(const SlotId slot) const noexcept -> bool { return Get(slot) != nullptr; }

    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return !bindings_[0] && !bindings_[1]; }

private:
    static constexpr auto Position(const SlotId slot) noexcept -> std::size_t
    {
        return static_cast<std::size_t>(slot);
    }

    std::array<std::optional<EntityUid>, 2> bindings_{};
};

// ---- Template ----

/// Everything a Template is made from. The parser and the JSON policy reader fill one in, with the
/// non-scope condition (the `when` and `unless` clauses already combined) built into `arena`.
struct TemplateDefinition
{
    PolicyId id;
    Effect effect{Effect::Permit};
    Annotations annotations{};
    PrincipalConstraint principal{};
    ActionConstraint action{ActionAny{}};
    ResourceConstraint resource{};
    /// Holds the non-scope condition; Template::Create appends the scope and the combined condition.
    ExpressionArena arena{};
    /// The root of the non-scope condition in `arena`, or nothing for a policy without `when` or
    /// `unless` clauses.
    std::optional<ExpressionIndex> nonScopeCondition{};
    /// The text the policy was read from (the whole input, shared by every policy in it) and the
    /// policy's extent in it, so the policy can be printed exactly as written. Both empty for a
    /// policy with no text form.
    SharedSourceText source{};
    std::optional<SourceSpan> span{};
};

/// A policy body: id, effect, annotations, scope and condition, with zero or more slots. A template
/// with no slots is the body of a static policy (they share the id). Immutable once created and
/// shared by every policy linked to it, so it is safe to read from any number of threads.
///
/// The scope constraints are kept as structured fields (for printing, linking and the scope index)
/// and also built, once, into the combined condition the evaluator runs, exactly as Rust's
/// `TemplateBody::condition` builds it (`cedar-policy-core/src/ast/policy.rs:1171`):
/// `principal && (action && (resource && nonScope))`, where an unconstrained scope part is the
/// literal `true` and a missing non-scope condition is `true`. The action list of `action in [..]`
/// is a node::Folded, so evaluation does not rebuild the set.
class Template final
{
public:
    /// Builds the scope expressions and the combined condition into the definition's arena.
    [[nodiscard]] static auto Create(TemplateDefinition definition) -> std::shared_ptr<const Template>;

    /// This template under the id `id`, sharing the arena (Rust's `Template::new_id`).
    [[nodiscard]] auto WithId(PolicyId id) const -> std::shared_ptr<const Template>;

    [[nodiscard]] auto Id() const noexcept -> const PolicyId& { return id_; }
    [[nodiscard]] auto GetEffect() const noexcept -> Effect { return effect_; }
    [[nodiscard]] auto GetAnnotations() const noexcept -> const Annotations& { return annotations_; }

    /// The value of the annotation `key` (the empty string for `@key` written without a value), or
    /// nothing when there is no such annotation or `key` is not an annotation key. As Rust's
    /// `Template::annotation`, `key` is read as an identifier, so whitespace and comments around
    /// it are allowed (`" id "` finds `@id`).
    [[nodiscard]] auto Annotation(std::string_view key) const noexcept -> std::optional<std::string_view>;

    /// The scope constraints as written; a slot is a SlotReference.
    [[nodiscard]] auto Principal() const noexcept -> const PrincipalConstraint& { return principal_; }
    [[nodiscard]] auto Action() const noexcept -> const ActionConstraint& { return action_; }
    [[nodiscard]] auto Resource() const noexcept -> const ResourceConstraint& { return resource_; }

    /// The arena holding every expression of this template.
    [[nodiscard]] auto Arena() const noexcept -> const ExpressionArena& { return *arena_; }

    /// The combined condition, the arena's root.
    [[nodiscard]] auto Condition() const noexcept -> ExpressionIndex { return condition_; }

    /// The `when`/`unless` part alone, or nothing when the policy has none.
    [[nodiscard]] auto NonScopeCondition() const noexcept -> std::optional<ExpressionIndex>
    {
        return nonScopeCondition_;
    }

    /// The slots that appear in the combined condition (nodes reachable from Condition(), including
    /// the written form under a folded node), each once, in the order Rust's `Template::slots`
    /// gives them: its walk of the condition reaches the resource scope before the principal
    /// scope, so Resource comes before Principal.
    [[nodiscard]] auto Slots() const noexcept -> std::span<const SlotId> { return {slots_.data(), slotCount_}; }

    [[nodiscard]] auto HasSlot(SlotId slot) const noexcept -> bool;

    /// True when the template has no slots.
    [[nodiscard]] auto IsStatic() const noexcept -> bool { return slotCount_ == 0; }

    /// The input the template was read from, or null.
    [[nodiscard]] auto Source() const noexcept -> const SharedSourceText& { return source_; }

    /// The template's extent in Source(), or nothing.
    [[nodiscard]] auto SourceExtent() const noexcept -> std::optional<SourceSpan> { return span_; }

    /// The template's text exactly as written, or nothing when it has no text form.
    [[nodiscard]] auto OriginalText() const noexcept -> std::optional<std::string_view>;

private:
    explicit Template(TemplateDefinition&& definition);

    PolicyId id_;
    Effect effect_;
    Annotations annotations_;
    PrincipalConstraint principal_;
    ActionConstraint action_;
    ResourceConstraint resource_;
    std::shared_ptr<const ExpressionArena> arena_;
    ExpressionIndex condition_{};
    std::optional<ExpressionIndex> nonScopeCondition_;
    std::array<SlotId, 2> slots_{};
    std::size_t slotCount_ = 0;
    SharedSourceText source_;
    std::optional<SourceSpan> span_;
};

// ---- Policy ----

/// An executable policy: a template, the id of this link, and the entities bound to the
/// template's slots (Rust's `ast::Policy`). A static policy has no link id of its own; its id is
/// its template's. A template-linked policy has its own id and binds every slot of its template.
/// Copies share the template.
class Policy final
{
public:
    /// The static policy whose body is `body`. Fails (LinkWrongSlots) when `body` has slots.
    [[nodiscard]] static auto Static(std::shared_ptr<const Template> body) -> std::expected<Policy, PolicySetError>;

    /// The policy `linkId` linking `body` with `bindings`. Fails (LinkWrongSlots) when a slot of
    /// `body` is unbound or a bound slot is not in `body` (Rust's `Template::check_binding`).
    [[nodiscard]] static auto Link(std::shared_ptr<const Template> body, PolicyId linkId, SlotBindings bindings)
        -> std::expected<Policy, PolicySetError>;

    /// The link id, or the template's id for a static policy.
    [[nodiscard]] auto Id() const noexcept -> const PolicyId& { return linkId_ ? *linkId_ : template_->Id(); }

    /// True for a static policy, false for a template-linked one.
    [[nodiscard]] auto IsStatic() const noexcept -> bool { return !linkId_; }

    [[nodiscard]] auto GetTemplate() const noexcept -> const Template& { return *template_; }
    [[nodiscard]] auto SharedTemplate() const noexcept -> const std::shared_ptr<const Template>& { return template_; }

    [[nodiscard]] auto Bindings() const noexcept -> const SlotBindings& { return bindings_; }

    /// The uid bound to `slot`, or null. What a Slot node evaluates to.
    [[nodiscard]] auto Binding(const SlotId slot) const noexcept -> const EntityUid* { return bindings_.Get(slot); }

    /// The id of the template this policy links, or null for a static policy (Rust's
    /// `Policy::template_id`).
    [[nodiscard]] auto TemplateId() const noexcept -> const PolicyId*
    {
        return linkId_ ? &template_->Id() : nullptr;
    }

    [[nodiscard]] auto GetEffect() const noexcept -> Effect { return template_->GetEffect(); }
    [[nodiscard]] auto Arena() const noexcept -> const ExpressionArena& { return template_->Arena(); }
    [[nodiscard]] auto Condition() const noexcept -> ExpressionIndex { return template_->Condition(); }

    /// The template's annotations; a link has its template's.
    [[nodiscard]] auto GetAnnotations() const noexcept -> const Annotations& { return template_->GetAnnotations(); }

    /// Template::Annotation of this policy's template.
    [[nodiscard]] auto Annotation(const std::string_view key) const noexcept -> std::optional<std::string_view>
    {
        return template_->Annotation(key);
    }

    /// The scope constraints with each slot replaced by the entity bound to it, so no
    /// SlotReference remains (Rust's `Policy::principal_constraint` and friends).
    [[nodiscard]] auto Principal() const -> PrincipalConstraint;
    [[nodiscard]] auto Action() const noexcept -> const ActionConstraint& { return template_->Action(); }
    [[nodiscard]] auto Resource() const -> ResourceConstraint;

    /// Every entity literal in the combined condition, scope included, one per occurrence, in the
    /// order Rust's `Policy::entity_literals` lists them (its subexpression walk: a node, then its
    /// children last to first). Slots are not literals, so a link's bound entities are not listed.
    [[nodiscard]] auto EntityLiterals() const -> std::vector<EntityUid>;

private:
    Policy(std::shared_ptr<const Template> body, std::optional<PolicyId> linkId, SlotBindings bindings) noexcept
        : template_{std::move(body)}, linkId_{std::move(linkId)}, bindings_{std::move(bindings)}
    {
    }

    std::shared_ptr<const Template> template_;
    std::optional<PolicyId> linkId_;
    SlotBindings bindings_;
};

} // namespace cedar

template <>
struct std::hash<cedar::PolicyId>
{
    /// Transparent: a map keyed by PolicyId can be searched with a string_view.
    using is_transparent = void;

    [[nodiscard]] auto operator()(const cedar::PolicyId& id) const noexcept -> std::size_t
    {
        return std::hash<std::string_view>{}(id.Text());
    }
    [[nodiscard]] auto operator()(const std::string_view text) const noexcept -> std::size_t
    {
        return std::hash<std::string_view>{}(text);
    }
};
