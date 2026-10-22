#include <cedar/Policy.hpp>

#include "syntax/Lexer.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <limits>
#include <variant>
#include <vector>

namespace cedar
{

// ---- Annotations ----

auto Annotations::Insert(const Symbol key, std::optional<std::string> value) -> bool
{
    assert(IsIdentifierSyntax(key.Text()));
    const auto position = std::ranges::lower_bound(entries_, key.Text(), {},
                                                   [](const Annotation& entry) { return entry.key.Text(); });
    if (position != entries_.end() && position->key == key) { return false; }
    entries_.insert(position, Annotation{key, std::move(value)});
    return true;
}

auto Annotations::Find(const std::string_view key) const noexcept -> std::optional<std::string_view>
{
    const auto position =
        std::ranges::lower_bound(entries_, key, {}, [](const Annotation& entry) { return entry.key.Text(); });
    if (position == entries_.end() || position->key.Text() != key) { return std::nullopt; }
    return position->value ? std::string_view{*position->value} : std::string_view{};
}

// ---- Template ----

namespace
{

// The expression for one entity reference: the literal uid, or the slot (Rust's
// `EntityReference::into_expr`).
auto BuildReference(ExpressionBuilder& builder, const EntityReference& reference, const SlotId slot)
    -> ExpressionIndex
{
    if (const auto* uid = std::get_if<EntityUid>(&reference)) { return builder.Entity(*uid); }
    return builder.Slot(slot, std::get<SlotReference>(reference).span);
}

// Rust's `PrincipalOrResourceConstraint::as_expr`.
auto BuildScope(ExpressionBuilder& builder, const PrincipalOrResourceConstraint& constraint,
                const Variable variable, const SlotId slot) -> ExpressionIndex
{
    return std::visit(
        Overloaded{
            [&](const ScopeAny&) { return builder.Bool(true); },
            [&](const ScopeEqual& equal) {
                const ExpressionIndex left = builder.Variable(variable);
                return builder.Equal(left, BuildReference(builder, equal.target, slot));
            },
            [&](const ScopeIn& in) {
                const ExpressionIndex left = builder.Variable(variable);
                return builder.In(left, BuildReference(builder, in.target, slot));
            },
            [&](const ScopeIs& is) { return builder.Is(builder.Variable(variable), is.type); },
            [&](const ScopeIsIn& isIn) {
                // Rust builds two separate variable nodes here, unlike the `is ... in` expression.
                const ExpressionIndex isType = builder.Is(builder.Variable(variable), isIn.type);
                const ExpressionIndex left = builder.Variable(variable);
                const ExpressionIndex in = builder.In(left, BuildReference(builder, isIn.target, slot));
                return builder.And(isType, in);
            },
        },
        constraint);
}

// The set literal of an `action in [..]` scope, folded: its elements are entity literals, so its
// value is known now.
auto BuildActionList(ExpressionBuilder& builder, const std::span<const EntityUid> actions) -> ExpressionIndex
{
    std::vector<ExpressionIndex> elements;
    elements.reserve(actions.size());
    for (const EntityUid& action : actions) { elements.push_back(builder.Entity(action)); }
    const ExpressionIndex written = builder.SetLiteral(elements);

    // The element values only need to live until Fold deep-copies the set into the arena.
    ValueArena scratch;
    std::vector<Value> values;
    values.reserve(actions.size());
    for (const EntityUid& action : actions) { values.push_back(Value::FromEntity(action)); }
    return builder.Fold(written, scratch.MakeSet(values));
}

// Calls `visit` with each direct child of the node at `index`, the written form of a folded node
// included.
template <typename Visit>
void ForEachChild(const ExpressionArena& arena, const ExpressionIndex index, Visit&& visit)
{
    arena.Visit(index, Overloaded{
        [](const node::Literal&) {},
        [&](const node::Folded& folded) { visit(folded.written); },
        [](const node::Variable&) {},
        [](const node::Slot&) {},
        [&](const node::If& branch) {
            visit(branch.condition);
            visit(branch.thenBranch);
            visit(branch.elseBranch);
        },
        [&](const node::And& conjunction) {
            visit(conjunction.left);
            visit(conjunction.right);
        },
        [&](const node::Or& disjunction) {
            visit(disjunction.left);
            visit(disjunction.right);
        },
        [&](const node::Unary& unary) { visit(unary.operand); },
        [&](const node::Binary& binary) {
            visit(binary.left);
            visit(binary.right);
        },
        [&](const node::ExtensionCall& call) {
            for (const ExpressionIndex argument : arena.Elements(call.arguments)) { visit(argument); }
        },
        [&](const node::GetAttribute& get) { visit(get.object); },
        [&](const node::HasAttribute& has) { visit(has.object); },
        [&](const node::Like& like) { visit(like.operand); },
        [&](const node::Is& is) { visit(is.operand); },
        [&](const node::SetLiteral& set) {
            for (const ExpressionIndex element : arena.Elements(set.elements)) { visit(element); }
        },
        [&](const node::RecordLiteral& record) {
            for (const RecordLiteralField& field : arena.Elements(record.fields)) { visit(field.value); }
        },
    });
}

// Which slots occur in the nodes reachable from `root`. Nodes the parser built but the condition
// does not use are ignored. The arena is a DAG, so each node is visited once.
auto CollectSlots(const ExpressionArena& arena, const ExpressionIndex root) -> std::array<bool, 2>
{
    std::array<bool, 2> found{};
    std::vector<bool> visited(arena.Size(), false);
    std::vector<ExpressionIndex> pending{root};
    visited[std::to_underlying(root)] = true;
    while (!pending.empty())
    {
        const ExpressionIndex index = pending.back();
        pending.pop_back();
        if (const auto* slot = std::get_if<node::Slot>(&arena.Node(index)))
        {
            found[static_cast<std::size_t>(slot->slot)] = true;
        }
        ForEachChild(arena, index, [&](const ExpressionIndex child) {
            if (visited[std::to_underlying(child)]) { return; }
            visited[std::to_underlying(child)] = true;
            pending.push_back(child);
        });
    }
    return found;
}

// Rust's `ActionConstraint::as_expr`.
auto BuildAction(ExpressionBuilder& builder, const ActionConstraint& constraint) -> ExpressionIndex
{
    return std::visit(Overloaded{
                          [&](const ActionAny&) { return builder.Bool(true); },
                          [&](const ActionEqual& equal) {
                              const ExpressionIndex left = builder.Variable(Variable::Action);
                              return builder.Equal(left, builder.Entity(equal.action));
                          },
                          [&](const ActionIn& in) {
                              const ExpressionIndex left = builder.Variable(Variable::Action);
                              return builder.In(left, BuildActionList(builder, in.actions));
                          },
                      },
                      constraint);
}

} // namespace

Template::Template(TemplateDefinition&& definition)
    : id_{std::move(definition.id)}, effect_{definition.effect}, annotations_{std::move(definition.annotations)},
      principal_{std::move(definition.principal)}, action_{std::move(definition.action)},
      resource_{std::move(definition.resource)}, nonScopeCondition_{definition.nonScopeCondition},
      source_{std::move(definition.source)}, span_{definition.span}
{
    ExpressionArena& arena = definition.arena;
    ExpressionBuilder builder{arena};
    const ExpressionIndex principal = BuildScope(builder, principal_.constraint, Variable::Principal, SlotId::Principal);
    const ExpressionIndex action = BuildAction(builder, action_);
    const ExpressionIndex resource = BuildScope(builder, resource_.constraint, Variable::Resource, SlotId::Resource);
    const ExpressionIndex nonScope = nonScopeCondition_ ? *nonScopeCondition_ : builder.Bool(true);
    const ExpressionIndex inner = builder.And(resource, nonScope, span_);
    const ExpressionIndex middle = builder.And(action, inner, span_);
    condition_ = builder.And(principal, middle, span_);

    // Rust collects the slots of the whole combined condition.
    const std::array<bool, 2> found = CollectSlots(arena, condition_);
    if (found[static_cast<std::size_t>(SlotId::Resource)]) { slots_[slotCount_++] = SlotId::Resource; }
    if (found[static_cast<std::size_t>(SlotId::Principal)]) { slots_[slotCount_++] = SlotId::Principal; }

    arena_ = std::make_shared<const ExpressionArena>(std::move(arena));
}

auto Template::Create(TemplateDefinition definition) -> std::shared_ptr<const Template>
{
    return std::shared_ptr<const Template>{new Template{std::move(definition)}};
}

auto Template::WithId(PolicyId id) const -> std::shared_ptr<const Template>
{
    auto renamed = std::shared_ptr<Template>{new Template{*this}};
    renamed->id_ = std::move(id);
    return renamed;
}

auto Template::HasSlot(const SlotId slot) const noexcept -> bool
{
    return std::ranges::find(Slots(), slot) != Slots().end();
}

auto Template::Annotation(const std::string_view key) const noexcept -> std::optional<std::string_view>
{
    // Rust parses `key` as an `AnyId` (`parse_anyid`): one identifier-shaped word, keywords
    // included, with whitespace and comments around it.
    if (key.size() >= std::numeric_limits<std::uint32_t>::max()) { return std::nullopt; }
    detail::Lexer lexer{key};
    const detail::Token word = lexer.Next();
    if (!detail::IsAnyIdentifier(word.kind) || lexer.Next().kind != detail::TokenKind::EndOfInput)
    {
        return std::nullopt;
    }
    return annotations_.Find(word.Text(key));
}

auto Template::OriginalText() const noexcept -> std::optional<std::string_view>
{
    if (!source_ || !span_) { return std::nullopt; }
    return std::string_view{source_->text}.substr(span_->begin, span_->Length());
}

// ---- Policy ----

namespace
{

// Rust's `Template::check_binding`: every slot of the template bound, nothing else bound.
auto CheckBindings(const Template& body, const SlotBindings& bindings) -> std::expected<void, PolicySetError>
{
    std::string unbound;
    std::string extra;
    for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
    {
        const bool used = body.HasSlot(slot);
        const bool bound = bindings.Contains(slot);
        if (used == bound) { continue; }
        std::string& list = used ? unbound : extra;
        if (!list.empty()) { list += ", "; }
        list += ToString(slot);
    }
    if (unbound.empty() && extra.empty()) { return {}; }

    std::string message = std::format("cannot link template `{}`:", body.Id().Text());
    if (!unbound.empty()) { message += std::format(" no value given for {}", unbound); }
    if (!unbound.empty() && !extra.empty()) { message += ";"; }
    if (!extra.empty()) { message += std::format(" the template has no slot {}", extra); }
    return std::unexpected{PolicySetError{Diagnostic{.code = DiagnosticCode::LinkWrongSlots, .message = message}}};
}

} // namespace

auto Policy::Static(std::shared_ptr<const Template> body) -> std::expected<Policy, PolicySetError>
{
    if (auto checked = CheckBindings(*body, SlotBindings{}); !checked) { return std::unexpected{checked.error()}; }
    return Policy{std::move(body), std::nullopt, SlotBindings{}};
}

auto Policy::Link(std::shared_ptr<const Template> body, PolicyId linkId, SlotBindings bindings)
    -> std::expected<Policy, PolicySetError>
{
    if (auto checked = CheckBindings(*body, bindings); !checked) { return std::unexpected{checked.error()}; }
    return Policy{std::move(body), std::move(linkId), std::move(bindings)};
}

namespace
{

// The constraint with its slot, if any, replaced by `bound` (Rust's `convert_entity_reference`).
auto Substitute(const PrincipalOrResourceConstraint& constraint, const EntityUid* bound) -> PrincipalOrResourceConstraint
{
    const auto resolve = [&](const EntityReference& reference) -> EntityReference {
        if (std::holds_alternative<SlotReference>(reference))
        {
            assert(bound != nullptr);
            return *bound;
        }
        return reference;
    };
    return std::visit(Overloaded{
                          [](const ScopeAny& any) -> PrincipalOrResourceConstraint { return any; },
                          [&](const ScopeEqual& equal) -> PrincipalOrResourceConstraint {
                              return ScopeEqual{resolve(equal.target)};
                          },
                          [&](const ScopeIn& in) -> PrincipalOrResourceConstraint { return ScopeIn{resolve(in.target)}; },
                          [](const ScopeIs& is) -> PrincipalOrResourceConstraint { return is; },
                          [&](const ScopeIsIn& isIn) -> PrincipalOrResourceConstraint {
                              return ScopeIsIn{isIn.type, resolve(isIn.target)};
                          },
                      },
                      constraint);
}

} // namespace

auto Policy::Principal() const -> PrincipalConstraint
{
    return PrincipalConstraint{Substitute(template_->Principal().constraint, Binding(SlotId::Principal))};
}

auto Policy::Resource() const -> ResourceConstraint
{
    return ResourceConstraint{Substitute(template_->Resource().constraint, Binding(SlotId::Resource))};
}

auto Policy::EntityLiterals() const -> std::vector<EntityUid>
{
    // Rust's `ExprIterator`: pop a node, push its children in order, so the last child is walked
    // first. The arena is a DAG; a shared node is walked once per parent, as in Rust's tree. A
    // folded node stands for its written form.
    const ExpressionArena& arena = Arena();
    std::vector<EntityUid> literals;
    std::vector<ExpressionIndex> pending{Condition()};
    while (!pending.empty())
    {
        const ExpressionIndex index = pending.back();
        pending.pop_back();
        if (const auto* literal = std::get_if<node::Literal>(&arena.Node(index)))
        {
            if (literal->value.Kind() == ValueKind::Entity) { literals.push_back(literal->value.AsEntity()); }
            continue;
        }
        ForEachChild(arena, index, [&](const ExpressionIndex child) { pending.push_back(child); });
    }
    return literals;
}

} // namespace cedar
