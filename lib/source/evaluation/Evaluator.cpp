#include "evaluation/Evaluator.hpp"

#include "evaluation/Operators.hpp"

#include <cedar/Authorizer.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cedar::detail
{

namespace
{

using Result = std::expected<Value, EvaluationError>;

} // namespace

auto Evaluator::Evaluate(const ExpressionIndex index) -> Result
{
    if (depth_ >= MaximumEvaluationDepth)
    {
        return std::unexpected(EvaluationError{EvaluationErrorKind::RecursionLimit, expressions_.Span(index)});
    }
    ++depth_;
    Result result = std::visit([this](const auto& node) { return EvaluateNode(node); },
                               expressions_.Node(index));
    --depth_;
    if (!result)
    {
        if (const std::optional<SourceSpan> span = expressions_.Span(index))
        {
            result.error().AttachSpanIfMissing(*span);
        }
    }
    return result;
}

auto Evaluator::EvaluateNode(const node::Literal& node) -> Result { return node.value; }

auto Evaluator::EvaluateNode(const node::Folded& node) -> Result { return node.value; }

auto Evaluator::EvaluateNode(const node::Variable& node) -> Result
{
    switch (node.variable)
    {
    case Variable::Principal: return Value::FromEntity(request_.Principal());
    case Variable::Action: return Value::FromEntity(request_.Action());
    case Variable::Resource: return Value::FromEntity(request_.Resource());
    case Variable::Context: return request_.GetContext().AsValue();
    }
    return Value::FromEntity(request_.Principal());
}

auto Evaluator::EvaluateNode(const node::Slot& node) -> Result
{
    const EntityUid* bound = node.slot == SlotId::Principal ? slots_.principal : slots_.resource;
    if (bound == nullptr)
    {
        return std::unexpected(EvaluationError{EvaluationErrorKind::UnlinkedSlot, std::nullopt,
                                               "template slot " + Quoted(ToString(node.slot)) + " was not linked"});
    }
    return Value::FromEntity(*bound);
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::If& node) -> Result
{
    const Result condition = Evaluate(node.condition);
    if (!condition) { return condition; }
    if (condition->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *condition, expressions_.Span(node.condition)));
    }
    return Evaluate(condition->AsBool() ? node.thenBranch : node.elseBranch);
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::And& node) -> Result
{
    const Result left = Evaluate(node.left);
    if (!left) { return left; }
    if (left->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *left, expressions_.Span(node.left)));
    }
    if (!left->AsBool()) { return Value::FromBool(false); }
    const Result right = Evaluate(node.right);
    if (!right) { return right; }
    if (right->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *right, expressions_.Span(node.right)));
    }
    return right;
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Or& node) -> Result
{
    const Result left = Evaluate(node.left);
    if (!left) { return left; }
    if (left->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *left, expressions_.Span(node.left)));
    }
    if (left->AsBool()) { return Value::FromBool(true); }
    const Result right = Evaluate(node.right);
    if (!right) { return right; }
    if (right->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *right, expressions_.Span(node.right)));
    }
    return right;
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Unary& node) -> Result
{
    const Result operand = Evaluate(node.operand);
    if (!operand) { return operand; }
    return ApplyUnary(node.operation, *operand, [&] { return expressions_.Span(node.operand); });
}

// Rust evaluates both operands, left first, before any type check (evaluator.rs BinaryApp).
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Binary& node) -> Result
{
    const Result left = Evaluate(node.left);
    if (!left) { return left; }
    const Result right = Evaluate(node.right);
    if (!right) { return right; }
    return ApplyBinary(
        node.operation, *left, *right, entities_, [&] { return expressions_.Span(node.left); },
        [&] { return expressions_.Span(node.right); });
}

// Arguments are evaluated left to right, then the call checks the function, the argument count
// and the argument types in Rust's order. A type error points at the first argument of the wrong
// kind, as Rust's does (the argument value's location).
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::ExtensionCall& node) -> Result
{
    const std::span<const ExpressionIndex> argumentNodes = expressions_.Elements(node.arguments);
    std::array<Value, 2> inlineArguments{Value::FromBool(false), Value::FromBool(false)};
    const std::span<Value> arguments =
        argumentNodes.size() <= inlineArguments.size()
            ? std::span<Value>{inlineArguments.data(), argumentNodes.size()}
            : arena_.AllocateValues(argumentNodes.size());
    for (std::size_t position = 0; position < argumentNodes.size(); ++position)
    {
        Result argument = Evaluate(argumentNodes[position]);
        if (!argument) { return argument; }
        arguments[position] = *argument;
    }

    return ApplyExtensionCall(node.function.QualifiedText(), arguments, arena_,
                              [&](const std::size_t position) { return expressions_.Span(argumentNodes[position]); });
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::GetAttribute& node) -> Result
{
    const Result object = Evaluate(node.object);
    if (!object) { return object; }
    return GetAttribute(*object, node.attribute, entities_, [&] { return expressions_.Span(node.object); });
}

// A type error on a later step of the path has no location of its own, so it takes the whole
// expression's span.
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::HasAttribute& node) -> Result
{
    const Result object = Evaluate(node.object);
    if (!object) { return object; }
    return HasAttributePath(*object, expressions_.Elements(node.path), entities_,
                            [&] { return expressions_.Span(node.object); });
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Like& node) -> Result
{
    const Result operand = Evaluate(node.operand);
    if (!operand) { return operand; }
    return ApplyLike(*operand, expressions_.PatternOf(node), [&] { return expressions_.Span(node.operand); });
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Is& node) -> Result
{
    const Result operand = Evaluate(node.operand);
    if (!operand) { return operand; }
    return ApplyIs(*operand, node.type, [&] { return expressions_.Span(node.operand); });
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::SetLiteral& node) -> Result
{
    const std::span<const ExpressionIndex> elementNodes = expressions_.Elements(node.elements);
    if (elementNodes.empty()) { return Value::FromSet(Set{}); }
    const std::span<Value> elements = arena_.AllocateValues(elementNodes.size());
    for (std::size_t position = 0; position < elementNodes.size(); ++position)
    {
        Result element = Evaluate(elementNodes[position]);
        if (!element) { return element; }
        elements[position] = *element;
    }
    return arena_.AdoptSet(elements);
}

// Fields are stored sorted by key with no duplicate, so they are evaluated in key order, as Rust's
// BTreeMap is, and the entries need no sorting.
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::RecordLiteral& node) -> Result
{
    const std::span<const RecordLiteralField> fields = expressions_.Elements(node.fields);
    if (fields.empty()) { return Value::FromRecord(Record{}); }
    const std::span<RecordEntry> entries = arena_.AllocateRecordEntries(fields.size());
    for (std::size_t position = 0; position < fields.size(); ++position)
    {
        Result value = Evaluate(fields[position].value);
        if (!value) { return value; }
        entries[position] = RecordEntry{fields[position].key, *value};
    }
    return Value::FromRecord(Record::FromSortedUnique(entries));
}

auto EvaluateCondition(const Policy& policy, const Request& request, const EntityStoreView entities)
    -> std::expected<bool, EvaluationError>
{
    alignas(std::max_align_t) std::array<std::byte, EvaluationArenaBufferBytes> buffer;
    ValueArena arena{buffer};
    Evaluator evaluator{request, entities, policy.Arena(),
                        SlotValues{policy.Binding(SlotId::Principal), policy.Binding(SlotId::Resource)}, arena};
    const ExpressionIndex root = policy.Condition();
    const Result value = evaluator.Evaluate(root);
    if (!value) { return std::unexpected(value.error()); }
    if (value->Kind() != ValueKind::Bool)
    {
        return std::unexpected(TypeMismatch("bool", *value, policy.Arena().Span(root)));
    }
    return value->AsBool();
}

} // namespace cedar::detail

namespace cedar
{

auto EvaluateExpression(const Request& request, const EntityStoreView entities, const Expression& expression)
    -> std::expected<OwnedValue, EvaluationError>
{
    alignas(std::max_align_t) std::array<std::byte, detail::EvaluationArenaBufferBytes> buffer;
    ValueArena arena{buffer};
    detail::Evaluator evaluator{request, entities, expression.Arena(), detail::SlotValues{}, arena};
    const std::expected<Value, EvaluationError> value = evaluator.Evaluate(expression.Root());
    if (!value) { return std::unexpected(value.error()); }
    return OwnedValue::Copy(*value);
}

} // namespace cedar
