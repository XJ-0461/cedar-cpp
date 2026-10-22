#include "syntax/Folding.hpp"

#include <cedar/Value.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <variant>
#include <vector>

namespace cedar::detail
{

auto ConstantValue(const ExpressionArena& arena, const ExpressionIndex index) -> const Value*
{
    const ExpressionNode& node = arena.Node(index);
    if (const auto* literal = std::get_if<node::Literal>(&node)) { return &literal->value; }
    if (const auto* folded = std::get_if<node::Folded>(&node)) { return &folded->value; }
    return nullptr;
}

auto FoldConstructorCall(const ExpressionArena& arena, ExpressionBuilder builder, const ExtensionCall& function,
                         const ExpressionIndex call, const std::span<const ExpressionIndex> arguments,
                         const std::optional<SourceSpan> span) -> ExpressionIndex
{
    std::array<Value, 2> values{Value::FromBool(false), Value::FromBool(false)};
    if (function.style != ExtensionCallStyle::Function || function.lastArgumentRepeats ||
        arguments.size() != function.arity || arguments.size() > values.size())
    {
        return call;
    }
    for (std::size_t position = 0; position < arguments.size(); ++position)
    {
        const auto* literal = std::get_if<node::Literal>(&arena.Node(arguments[position]));
        if (literal == nullptr || literal->value.Kind() != function.argumentKinds[position]) { return call; }
        values[position] = literal->value;
    }
    const std::expected<Value, EvaluationError> value = function.body(std::span{values.data(), arguments.size()});
    if (!value) { return call; }
    return builder.Fold(call, *value, span);
}

auto FoldConstantSet(const ExpressionArena& arena, ExpressionBuilder builder, const ExpressionIndex set,
                     const std::span<const ExpressionIndex> elements, const std::optional<SourceSpan> span)
    -> ExpressionIndex
{
    std::vector<Value> values;
    values.reserve(elements.size());
    for (const ExpressionIndex element : elements)
    {
        const Value* value = ConstantValue(arena, element);
        if (value == nullptr) { return set; }
        values.push_back(*value);
    }
    ValueArena scratch;
    return builder.Fold(set, scratch.MakeSet(values), span);
}

} // namespace cedar::detail
