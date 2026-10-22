#include "partial-evaluation/Residual.hpp"

#include "validation/Typechecker.hpp"

#include <cedar/Diagnostic.hpp>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cedar::detail
{

namespace
{

/// The direct children of `node`, in order.
[[nodiscard]] auto ChildrenOf(const ExpressionArena& arena, const ExpressionNode& node) -> std::vector<ExpressionIndex>
{
    return Visit(node,
                 Overloaded{
                     [](const node::Literal&) -> std::vector<ExpressionIndex> { return {}; },
                     [](const node::Folded& folded) -> std::vector<ExpressionIndex> { return {folded.written}; },
                     [](const node::Variable&) -> std::vector<ExpressionIndex> { return {}; },
                     [](const node::Slot&) -> std::vector<ExpressionIndex> { return {}; },
                     [](const node::If& branch) -> std::vector<ExpressionIndex>
                     { return {branch.condition, branch.thenBranch, branch.elseBranch}; },
                     [](const node::And& operation) -> std::vector<ExpressionIndex>
                     { return {operation.left, operation.right}; },
                     [](const node::Or& operation) -> std::vector<ExpressionIndex>
                     { return {operation.left, operation.right}; },
                     [](const node::Unary& operation) -> std::vector<ExpressionIndex> { return {operation.operand}; },
                     [](const node::Binary& operation) -> std::vector<ExpressionIndex>
                     { return {operation.left, operation.right}; },
                     [&](const node::ExtensionCall& call) -> std::vector<ExpressionIndex>
                     {
                         const auto arguments = arena.Elements(call.arguments);
                         return {arguments.begin(), arguments.end()};
                     },
                     [](const node::GetAttribute& access) -> std::vector<ExpressionIndex> { return {access.object}; },
                     [](const node::HasAttribute& test) -> std::vector<ExpressionIndex> { return {test.object}; },
                     [](const node::Like& test) -> std::vector<ExpressionIndex> { return {test.operand}; },
                     [](const node::Is& test) -> std::vector<ExpressionIndex> { return {test.operand}; },
                     [&](const node::SetLiteral& set) -> std::vector<ExpressionIndex>
                     {
                         const auto elements = arena.Elements(set.elements);
                         return {elements.begin(), elements.end()};
                     },
                     [&](const node::RecordLiteral& record) -> std::vector<ExpressionIndex>
                     {
                         std::vector<ExpressionIndex> values;
                         for (const RecordLiteralField& field : arena.Elements(record.fields))
                         {
                             values.push_back(field.value);
                         }
                         return values;
                     },
                 });
}

/// Appends to `builder` the node `node` of `arena`, with each child `c` replaced by `map(c)`, which
/// must already be in the builder's arena. Returns the new node.
template <typename ChildMap>
[[nodiscard]] auto Rebuild(ExpressionBuilder& builder, const ExpressionArena& arena, const ExpressionNode& node,
                           const ChildMap& map) -> ExpressionIndex
{
    const auto mapAll = [&](const std::span<const ExpressionIndex> children)
    {
        std::vector<ExpressionIndex> mapped;
        mapped.reserve(children.size());
        for (const ExpressionIndex child : children) { mapped.push_back(map(child)); }
        return mapped;
    };
    return Visit(node,
                 Overloaded{
                     [&](const node::Literal& literal) { return builder.Literal(literal.value); },
                     [&](const node::Folded& folded) { return builder.Fold(map(folded.written), folded.value); },
                     [&](const node::Variable& variable) { return builder.Variable(variable.variable); },
                     [&](const node::Slot& slot) { return builder.Slot(slot.slot); },
                     [&](const node::If& branch)
                     { return builder.If(map(branch.condition), map(branch.thenBranch), map(branch.elseBranch)); },
                     [&](const node::And& operation) { return builder.And(map(operation.left), map(operation.right)); },
                     [&](const node::Or& operation) { return builder.Or(map(operation.left), map(operation.right)); },
                     [&](const node::Unary& operation)
                     { return builder.Unary(operation.operation, map(operation.operand)); },
                     [&](const node::Binary& operation)
                     { return builder.Binary(operation.operation, map(operation.left), map(operation.right)); },
                     [&](const node::ExtensionCall& call)
                     { return builder.ExtensionCall(call.function, mapAll(arena.Elements(call.arguments))); },
                     [&](const node::GetAttribute& access)
                     { return builder.GetAttribute(map(access.object), access.attribute); },
                     [&](const node::HasAttribute& test)
                     {
                         return test.extended ? builder.ExtendedHasAttribute(map(test.object), arena.Elements(test.path))
                                              : builder.HasAttribute(map(test.object), arena.Elements(test.path));
                     },
                     [&](const node::Like& test) { return builder.Like(map(test.operand), arena.Elements(test.pattern)); },
                     [&](const node::Is& test) { return builder.Is(map(test.operand), test.type); },
                     [&](const node::SetLiteral& set)
                     { return builder.SetLiteral(mapAll(arena.Elements(set.elements))); },
                     [&](const node::RecordLiteral& record)
                     {
                         std::vector<RecordLiteralField> fields;
                         for (const RecordLiteralField& field : arena.Elements(record.fields))
                         {
                             fields.push_back(RecordLiteralField{field.key, map(field.value)});
                         }
                         // The keys come from a record literal, so none repeats.
                         return *builder.RecordLiteral(fields);
                     },
                 });
}

/// `index`, or for a folded node the subtree it was written as.
[[nodiscard]] auto ResolveFolded(const ExpressionArena& arena, ExpressionIndex index) noexcept -> ExpressionIndex
{
    while (const auto* const folded = std::get_if<node::Folded>(&arena.Node(index))) { index = folded->written; }
    return index;
}

/// Builds a residual from a template's typed condition, one source node at a time. A source node
/// shared by several parents is converted once.
class TypedTreeConverter final
{
public:
    TypedTreeConverter(const Template& body, const SlotBindings& bindings, const NodeTypes& nodeTypes)
        : body_{body}, bindings_{bindings}, source_{body.Arena()}, nodeTypes_{nodeTypes}, converted_(source_.Size())
    {
    }

    [[nodiscard]] auto Convert(ExpressionIndex index) -> std::expected<ExpressionIndex, PartialEvaluationError>
    {
        index = ResolveFolded(source_, index);
        if (const auto& done = converted_[std::to_underlying(index)]) { return *done; }
        auto result = ConvertNode(index);
        if (result) { converted_[std::to_underlying(index)] = *result; }
        return result;
    }

    [[nodiscard]] auto Finish(const ExpressionIndex root) && -> Residual
    {
        return Residual::Create(ResidualTree{std::move(arena_), std::move(types_), root});
    }

private:
    [[nodiscard]] auto TypeAt(const ExpressionIndex index) const noexcept -> const Type*
    {
        const std::optional<Type>* const type = nodeTypes_.Find(index);
        return type != nullptr && type->has_value() ? &**type : nullptr;
    }

    [[nodiscard]] auto IsTypedSingleton(const ExpressionIndex index, const BooleanType kind) const noexcept -> bool
    {
        const Type* const type = TypeAt(ResolveFolded(source_, index));
        if (type == nullptr) { return false; }
        const auto* const boolean = std::get_if<types::Boolean>(&type->kind);
        return boolean != nullptr && boolean->kind == kind;
    }

    [[nodiscard]] auto Fail(const DiagnosticCode code, std::string message, const ExpressionIndex index) const
        -> std::unexpected<PartialEvaluationError>
    {
        return std::unexpected{PartialEvaluationError{Diagnostic{.code = code,
                                                                 .message = std::move(message),
                                                                 .primary = source_.Span(index),
                                                                 .source = body_.Source()}}};
    }

    /// Records `type` for the node the builder just returned.
    auto Typed(const ExpressionIndex index, const Type& type) -> ExpressionIndex
    {
        types_.resize(arena_.Size());
        types_[std::to_underlying(index)] = type;
        return index;
    }

    [[nodiscard]] auto ConvertNode(const ExpressionIndex index) -> std::expected<ExpressionIndex, PartialEvaluationError>
    {
        const ExpressionNode& node = source_.Node(index);

        // The typechecker short-circuits these two without typing the right operand; the typed
        // tree is the left operand alone.
        if (const auto* const operation = std::get_if<node::And>(&node);
            operation != nullptr && IsTypedSingleton(operation->left, BooleanType::False))
        {
            return Convert(operation->left);
        }
        if (const auto* const operation = std::get_if<node::Or>(&node);
            operation != nullptr && IsTypedSingleton(operation->left, BooleanType::True))
        {
            return Convert(operation->left);
        }

        const Type* const type = TypeAt(index);
        if (type == nullptr)
        {
            return Fail(DiagnosticCode::MissingTypeAnnotation, "the typechecker gave this expression no type", index);
        }

        if (const auto* const slot = std::get_if<node::Slot>(&node))
        {
            const EntityUid* const binding = bindings_.Get(slot->slot);
            if (binding == nullptr)
            {
                return Fail(DiagnosticCode::ResidualUnlinkedSlot,
                            std::format("the slot `{}` is not linked to an entity", ToString(slot->slot)), index);
            }
            return Typed(builder_.Entity(*binding), *type);
        }

        if (const auto* const branch = std::get_if<node::If>(&node))
        {
            // A test typed True or False keeps the test and puts the taken branch in both arms.
            const bool alwaysTrue = IsTypedSingleton(branch->condition, BooleanType::True);
            const bool alwaysFalse = IsTypedSingleton(branch->condition, BooleanType::False);
            if (alwaysTrue || alwaysFalse)
            {
                const auto test = Convert(branch->condition);
                if (!test) { return test; }
                const auto taken = Convert(alwaysTrue ? branch->thenBranch : branch->elseBranch);
                if (!taken) { return taken; }
                return Typed(builder_.If(*test, *taken, *taken), *type);
            }
        }

        for (const ExpressionIndex child : ChildrenOf(source_, node))
        {
            const auto converted = Convert(child);
            if (!converted) { return converted; }
        }
        const ExpressionIndex rebuilt =
            Rebuild(builder_, source_, node, [&](const ExpressionIndex child)
                    { return *converted_[std::to_underlying(ResolveFolded(source_, child))]; });
        return Typed(rebuilt, *type);
    }

    const Template& body_;
    const SlotBindings& bindings_;
    const ExpressionArena& source_;
    const NodeTypes& nodeTypes_;
    std::vector<std::optional<ExpressionIndex>> converted_;
    ExpressionArena arena_{};
    ExpressionBuilder builder_{arena_};
    std::vector<Type> types_{};
};

/// Copies the subtree of `arena` under `root` into `builder`'s arena, keeping shared nodes shared.
class TreeCopier final
{
public:
    TreeCopier(const ExpressionArena& arena, ExpressionBuilder& builder)
        : arena_{arena}, builder_{builder}, copied_(arena.Size())
    {
    }

    auto Copy(const ExpressionIndex index) -> ExpressionIndex
    {
        if (const auto& done = copied_[std::to_underlying(index)]) { return *done; }
        const ExpressionNode& node = arena_.Node(index);
        for (const ExpressionIndex child : ChildrenOf(arena_, node)) { Copy(child); }
        const ExpressionIndex result = Rebuild(builder_, arena_, node, [&](const ExpressionIndex child)
                                               { return *copied_[std::to_underlying(child)]; });
        copied_[std::to_underlying(index)] = result;
        return result;
    }

private:
    const ExpressionArena& arena_;
    ExpressionBuilder& builder_;
    std::vector<std::optional<ExpressionIndex>> copied_;
};

auto CollectValueUids(const Value& value, std::vector<EntityUid>& uids) -> void
{
    switch (value.Kind())
    {
    case ValueKind::Entity: uids.push_back(value.AsEntity()); break;
    case ValueKind::Set:
        for (const Value& element : value.AsSet()) { CollectValueUids(element, uids); }
        break;
    case ValueKind::Record:
        for (const RecordEntry& entry : value.AsRecord()) { CollectValueUids(entry.value, uids); }
        break;
    default: break;
    }
}

auto CollectUids(const ExpressionArena& arena, const ExpressionIndex index, std::vector<EntityUid>& uids) -> void
{
    const ExpressionNode& node = arena.Node(index);
    if (const auto* const literal = std::get_if<node::Literal>(&node))
    {
        CollectValueUids(literal->value, uids);
        return;
    }
    if (const auto* const folded = std::get_if<node::Folded>(&node))
    {
        CollectValueUids(folded->value, uids);
        return;
    }
    for (const ExpressionIndex child : ChildrenOf(arena, node)) { CollectUids(arena, child, uids); }
}

/// Rust's `can_error_assuming_well_formed`, arm by arm. A concrete value (a Literal, or a folded
/// one) cannot error; an extension call can, and the error residual is one.
auto CanError(const ExpressionArena& arena, const ExpressionIndex index) -> bool
{
    const auto any = [&](const std::span<const ExpressionIndex> children)
    { return std::ranges::any_of(children, [&](const ExpressionIndex child) { return CanError(arena, child); }); };
    return Visit(arena.Node(index),
                 Overloaded{
                     [](const node::Literal&) { return false; },
                     [](const node::Folded&) { return false; },
                     [](const node::Variable&) { return false; },
                     [](const node::Slot&) { return false; },
                     [&](const node::And& operation)
                     { return CanError(arena, operation.left) || CanError(arena, operation.right); },
                     [&](const node::Or& operation)
                     { return CanError(arena, operation.left) || CanError(arena, operation.right); },
                     [&](const node::If& branch)
                     {
                         return CanError(arena, branch.condition) || CanError(arena, branch.thenBranch) ||
                                CanError(arena, branch.elseBranch);
                     },
                     [&](const node::Is& test) { return CanError(arena, test.operand); },
                     [&](const node::Like& test) { return CanError(arena, test.operand); },
                     [&](const node::Binary& operation)
                     {
                         switch (operation.operation)
                         {
                         // Integer overflow.
                         case BinaryOperator::Add:
                         case BinaryOperator::Multiply:
                         case BinaryOperator::Subtract: return true;
                         // The entity may be missing from the store at reauthorization.
                         case BinaryOperator::GetTag: return true;
                         case BinaryOperator::Contains:
                         case BinaryOperator::ContainsAll:
                         case BinaryOperator::ContainsAny:
                         case BinaryOperator::Equal:
                         case BinaryOperator::HasTag:
                         case BinaryOperator::In:
                         case BinaryOperator::Less:
                         case BinaryOperator::LessEqual: break;
                         }
                         return CanError(arena, operation.left) || CanError(arena, operation.right);
                     },
                     [](const node::ExtensionCall&) { return true; },
                     // The entity may be missing from the store at reauthorization.
                     [](const node::GetAttribute&) { return true; },
                     [&](const node::HasAttribute& test) { return CanError(arena, test.object); },
                     [&](const node::Unary& operation)
                     {
                         switch (operation.operation)
                         {
                         // Integer overflow.
                         case UnaryOperator::Negate: return true;
                         case UnaryOperator::IsEmpty:
                         case UnaryOperator::Not: break;
                         }
                         return CanError(arena, operation.operand);
                     },
                     [&](const node::SetLiteral& set) { return any(arena.Elements(set.elements)); },
                     [&](const node::RecordLiteral& record)
                     {
                         return std::ranges::any_of(arena.Elements(record.fields), [&](const RecordLiteralField& field)
                                                    { return CanError(arena, field.value); });
                     },
                 });
}

} // namespace

auto ResidualErrorName() -> const Name&
{
    static const Name name = *Name::Parse("error");
    return name;
}

auto Residual::FromTypedPolicy(const Policy& policy, const NodeTypes& nodeTypes)
    -> std::expected<Residual, PartialEvaluationError>
{
    return FromTypedTemplate(policy.GetTemplate(), policy.Bindings(), nodeTypes);
}

auto Residual::FromTypedTemplate(const Template& body, const SlotBindings& bindings, const NodeTypes& nodeTypes)
    -> std::expected<Residual, PartialEvaluationError>
{
    TypedTreeConverter converter{body, bindings, nodeTypes};
    const auto root = converter.Convert(body.Condition());
    if (!root) { return std::unexpected{root.error()}; }
    return std::move(converter).Finish(*root);
}

auto Residual::Create(ResidualTree tree) -> Residual
{
    return Residual{std::make_shared<const ResidualTree>(std::move(tree))};
}

auto Residual::Error(Type type) -> Residual
{
    ResidualTree tree{.arena = {}, .types = {std::move(type)}, .root = {}};
    ExpressionBuilder builder{tree.arena};
    tree.root = builder.ExtensionCall(ResidualErrorName(), {});
    return Create(std::move(tree));
}

auto Residual::ConcreteValue() const noexcept -> const Value*
{
    const auto* const literal = std::get_if<node::Literal>(&Arena().Node(Root()));
    return literal != nullptr ? &literal->value : nullptr;
}

auto Residual::IsError() const noexcept -> bool
{
    const auto* const call = std::get_if<node::ExtensionCall>(&Arena().Node(Root()));
    return call != nullptr && call->arguments.count == 0 && call->function == ResidualErrorName();
}

auto CanErrorAssumingWellFormed(const ExpressionArena& arena, const ExpressionIndex root) -> bool
{
    return CanError(arena, root);
}

auto Residual::CanErrorAssumingWellFormed() const -> bool
{
    return CanError(Arena(), Root());
}

auto Residual::AllLiteralUids() const -> std::vector<EntityUid>
{
    std::vector<EntityUid> uids;
    CollectUids(Arena(), Root(), uids);
    std::ranges::sort(uids);
    const auto repeats = std::ranges::unique(uids);
    uids.erase(repeats.begin(), repeats.end());
    return uids;
}

auto Residual::ToPolicy(const Policy& source) const -> Policy
{
    TemplateDefinition definition{
        .id = source.Id(), .effect = source.GetEffect(), .annotations = source.GetAnnotations()};
    ExpressionBuilder builder{definition.arena};
    TreeCopier copier{Arena(), builder};
    definition.nonScopeCondition = copier.Copy(Root());
    // A body with no slots always makes a static policy.
    return *Policy::Static(Template::Create(std::move(definition)));
}

} // namespace cedar::detail
