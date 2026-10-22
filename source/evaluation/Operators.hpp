#pragma once

#include "extensions/ExtensionCalls.hpp"

#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Value.hpp>

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// The value semantics of Cedar's operators, applied to operands that are already values. The
// evaluator and the partial evaluator both call these, so an operator's result and its error are
// the same whichever one applies it.
//
// Where an error points is the caller's knowledge, not the operator's: each operand comes with a
// span locator, a callable that gives the operand's source span. A locator is called only when an
// error is built, so the success path never looks a span up. NoSpan is the locator for an operand
// that has no location.
//
// The functions are templates on their locators and live in this header, so the evaluator's
// dispatch inlines them; the error builders they call are cold and out of line.

namespace cedar::detail
{

using OperatorResult = std::expected<Value, EvaluationError>;

/// A callable that gives an operand's source span, or none.
template <class Locator>
concept SpanLocator = std::is_invocable_r_v<std::optional<SourceSpan>, const Locator&>;

/// A callable that gives the source span of the extension call argument at a position, or none.
template <class Locator>
concept ArgumentSpanLocator = std::is_invocable_r_v<std::optional<SourceSpan>, const Locator&, std::size_t>;

/// The locator of an operand with no location.
struct NoSpan
{
    [[nodiscard]] constexpr auto operator()() const noexcept -> std::optional<SourceSpan> { return std::nullopt; }
    [[nodiscard]] constexpr auto operator()(std::size_t /*position*/) const noexcept -> std::optional<SourceSpan>
    {
        return std::nullopt;
    }
};

/// `text` between backticks, as Rust's error messages quote names and values.
[[nodiscard]] auto Quoted(std::string_view text) -> std::string;

/// "type error: expected <expected>, got <actual's type>".
[[nodiscard, gnu::cold]] auto TypeMismatch(std::string_view expected, const Value& actual,
                                           std::optional<SourceSpan> span,
                                           std::optional<std::string> help = std::nullopt) -> EvaluationError;

[[nodiscard, gnu::cold]] auto EntityMissing(const EntityUid& uid, std::optional<SourceSpan> span) -> EvaluationError;

/// Integer overflow in `verb` (add, subtract, multiply, negate); `right` is null for negation.
[[nodiscard, gnu::cold]] auto Overflow(std::string_view verb, const Value& left, const Value* right)
    -> EvaluationError;

/// The type error of `<` or `<=` on operands that are not two longs or two comparable extension
/// values of one type.
[[nodiscard, gnu::cold]] auto ComparisonTypeMismatch(const Value& left, const Value& right,
                                                     std::optional<SourceSpan> leftSpan,
                                                     std::optional<SourceSpan> rightSpan) -> EvaluationError;

/// The type error of `in` whose left operand is not an entity.
[[nodiscard, gnu::cold]] auto InLeftTypeMismatch(const Value& left, const Value& right,
                                                 std::optional<SourceSpan> leftSpan) -> EvaluationError;

[[nodiscard, gnu::cold]] auto RecordAttributeMissing(std::string_view attribute) -> EvaluationError;

[[nodiscard, gnu::cold]] auto EntityAttributeMissing(const EntityUid& uid, const Entity& entity,
                                                     std::string_view attribute) -> EvaluationError;

[[nodiscard, gnu::cold]] auto EntityTagMissing(const EntityUid& uid, const Entity& entity, std::string_view tag)
    -> EvaluationError;

/// The position of the first argument whose kind is not the one extension function `name` takes
/// there, or none.
[[nodiscard]] auto MistypedArgumentPosition(std::string_view name, std::span<const Value> arguments)
    -> std::optional<std::size_t>;

/// The extension types `<` and `<=` accept (Rust `supports_operator_overloading`).
constexpr auto SupportsComparison(const ValueKind kind) noexcept -> bool
{
    return kind == ValueKind::Datetime || kind == ValueKind::Duration;
}

/// Rust `unary_app`: `!`, unary `-` and `isEmpty`.
[[nodiscard]] inline auto ApplyUnary(const UnaryOperator operation, const Value& operand,
                                     const SpanLocator auto& operandSpan) -> OperatorResult
{
    switch (operation)
    {
    case UnaryOperator::Not:
        if (operand.Kind() != ValueKind::Bool) { return std::unexpected(TypeMismatch("bool", operand, operandSpan())); }
        return Value::FromBool(!operand.AsBool());
    case UnaryOperator::Negate:
    {
        if (operand.Kind() != ValueKind::Long) { return std::unexpected(TypeMismatch("long", operand, operandSpan())); }
        std::int64_t negated = 0;
        if (__builtin_sub_overflow(std::int64_t{0}, operand.AsLong(), &negated))
        {
            return std::unexpected(Overflow("negate", operand, nullptr));
        }
        return Value::FromLong(negated);
    }
    case UnaryOperator::IsEmpty:
        if (operand.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", operand, operandSpan())); }
        return Value::FromBool(operand.AsSet().IsEmpty());
    }
    return operand;
}

// Rust `eval_in` (evaluator.rs:811, entity.rs:606): reflexive equality, or membership in the left
// entity's closed ancestor set; a missing left entity is equal to itself only. Every element of a
// set right-hand side is checked to be an entity before any is compared. An element carries no
// location, so its type error has none, and the caller's enclosing span applies (Rust points at
// the element of a set literal, and at the whole expression for a set from entity or context data).
[[nodiscard]] inline auto ApplyIn(const Value& left, const Value& right, const EntityStoreView entities,
                                  const SpanLocator auto& leftSpan, const SpanLocator auto& rightSpan)
    -> OperatorResult
{
    if (left.Kind() != ValueKind::Entity) { return std::unexpected(InLeftTypeMismatch(left, right, leftSpan())); }
    const EntityUid& descendant = left.AsEntity();
    const Entity* const entity = entities.Find(descendant);
    const auto isIn = [&](const EntityUid& ancestor) {
        return descendant == ancestor || (entity != nullptr && entity->IsDescendantOf(ancestor));
    };

    if (right.Kind() == ValueKind::Entity) { return Value::FromBool(isIn(right.AsEntity())); }
    if (right.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set or entity", right, rightSpan())); }
    const Set ancestors = right.AsSet();
    for (const Value& element : ancestors)
    {
        if (element.Kind() != ValueKind::Entity)
        {
            return std::unexpected(TypeMismatch("entity", element, std::nullopt));
        }
    }
    for (const Value& element : ancestors)
    {
        if (isIn(element.AsEntity())) { return Value::FromBool(true); }
    }
    return Value::FromBool(false);
}

/// `hasTag` and `getTag`. A missing entity has no tags; reading a tag from one is an error.
[[nodiscard]] inline auto ApplyTag(const BinaryOperator operation, const Value& left, const Value& right,
                                   const EntityStoreView entities, const SpanLocator auto& leftSpan,
                                   const SpanLocator auto& rightSpan) -> OperatorResult
{
    if (left.Kind() != ValueKind::Entity) { return std::unexpected(TypeMismatch("entity", left, leftSpan())); }
    if (right.Kind() != ValueKind::String) { return std::unexpected(TypeMismatch("string", right, rightSpan())); }
    const Entity* const entity = entities.Find(left.AsEntity());
    if (operation == BinaryOperator::HasTag)
    {
        return Value::FromBool(entity != nullptr && entity->FindTag(right.AsString()) != nullptr);
    }
    if (entity == nullptr) { return std::unexpected(EntityMissing(left.AsEntity(), leftSpan())); }
    if (const Value* tag = entity->FindTag(right.AsString())) { return *tag; }
    return std::unexpected(EntityTagMissing(left.AsEntity(), *entity, right.AsString()));
}

/// Every binary operator on two evaluated operands (Rust `BinaryApp`, `binary_relation`,
/// `binary_arith`). `entities` serves `in`, `hasTag` and `getTag`.
[[nodiscard]] inline auto ApplyBinary(const BinaryOperator operation, const Value& left, const Value& right,
                                      const EntityStoreView entities, const SpanLocator auto& leftSpan,
                                      const SpanLocator auto& rightSpan) -> OperatorResult
{
    switch (operation)
    {
    case BinaryOperator::Equal:
        // Scalars compare inline; Value::Equal is an out-of-line call.
        if (left.Kind() == ValueKind::Long && right.Kind() == ValueKind::Long)
        {
            return Value::FromBool(left.AsLong() == right.AsLong());
        }
        if (left.Kind() == ValueKind::Bool && right.Kind() == ValueKind::Bool)
        {
            return Value::FromBool(left.AsBool() == right.AsBool());
        }
        return Value::FromBool(Value::Equal(left, right));

    // Rust `binary_relation`: long against long, or two extension values of one type that
    // supports operator overloading (datetime, duration).
    case BinaryOperator::Less:
    case BinaryOperator::LessEqual:
    {
        const bool strict = operation == BinaryOperator::Less;
        const ValueKind leftKind = left.Kind();
        const ValueKind rightKind = right.Kind();
        if (leftKind == ValueKind::Long && rightKind == ValueKind::Long)
        {
            return Value::FromBool(strict ? left.AsLong() < right.AsLong() : left.AsLong() <= right.AsLong());
        }
        if (leftKind == rightKind && SupportsComparison(leftKind))
        {
            const std::strong_ordering order = Value::Compare(left, right);
            return Value::FromBool(strict ? order < 0 : order <= 0);
        }
        return std::unexpected(ComparisonTypeMismatch(left, right, leftSpan(), rightSpan()));
    }

    case BinaryOperator::Add:
    case BinaryOperator::Subtract:
    case BinaryOperator::Multiply:
    {
        if (left.Kind() != ValueKind::Long) { return std::unexpected(TypeMismatch("long", left, leftSpan())); }
        if (right.Kind() != ValueKind::Long) { return std::unexpected(TypeMismatch("long", right, rightSpan())); }
        std::int64_t result = 0;
        bool overflowed = false;
        std::string_view verb;
        switch (operation)
        {
        case BinaryOperator::Add:
            overflowed = __builtin_add_overflow(left.AsLong(), right.AsLong(), &result);
            verb = "add";
            break;
        case BinaryOperator::Subtract:
            overflowed = __builtin_sub_overflow(left.AsLong(), right.AsLong(), &result);
            verb = "subtract";
            break;
        default:
            overflowed = __builtin_mul_overflow(left.AsLong(), right.AsLong(), &result);
            verb = "multiply";
            break;
        }
        if (overflowed) { return std::unexpected(Overflow(verb, left, &right)); }
        return Value::FromLong(result);
    }

    case BinaryOperator::In: return ApplyIn(left, right, entities, leftSpan, rightSpan);

    case BinaryOperator::Contains:
        if (left.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", left, leftSpan())); }
        return Value::FromBool(left.AsSet().Contains(right));

    case BinaryOperator::ContainsAll:
    case BinaryOperator::ContainsAny:
        if (left.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", left, leftSpan())); }
        if (right.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", right, rightSpan())); }
        return Value::FromBool(operation == BinaryOperator::ContainsAll ? left.AsSet().ContainsAll(right.AsSet())
                                                                        : left.AsSet().ContainsAny(right.AsSet()));

    case BinaryOperator::GetTag:
    case BinaryOperator::HasTag: return ApplyTag(operation, left, right, entities, leftSpan, rightSpan);
    }
    return Value::FromBool(false);
}

/// `operand like pattern`.
[[nodiscard]] inline auto ApplyLike(const Value& operand, const Pattern& pattern,
                                    const SpanLocator auto& operandSpan) -> OperatorResult
{
    if (operand.Kind() != ValueKind::String) { return std::unexpected(TypeMismatch("string", operand, operandSpan())); }
    return Value::FromBool(pattern.Matches(operand.AsString()));
}

/// `operand is type`.
[[nodiscard]] inline auto ApplyIs(const Value& operand, const EntityType& type, const SpanLocator auto& operandSpan)
    -> OperatorResult
{
    if (operand.Kind() != ValueKind::Entity) { return std::unexpected(TypeMismatch("entity", operand, operandSpan())); }
    return Value::FromBool(operand.AsEntity().Type() == type);
}

/// `object.attribute`: a record key, or an attribute of an entity that is in `entities`.
[[nodiscard]] inline auto GetAttribute(const Value& object, const std::string_view attribute,
                                       const EntityStoreView entities, const SpanLocator auto& objectSpan)
    -> OperatorResult
{
    if (object.Kind() == ValueKind::Record)
    {
        if (const Value* value = object.AsRecord().Find(attribute)) { return *value; }
        return std::unexpected(RecordAttributeMissing(attribute));
    }
    if (object.Kind() == ValueKind::Entity)
    {
        const EntityUid& uid = object.AsEntity();
        const Entity* const entity = entities.Find(uid);
        if (entity == nullptr) { return std::unexpected(EntityMissing(uid, objectSpan())); }
        if (const Value* value = entity->FindAttribute(attribute)) { return *value; }
        return std::unexpected(EntityAttributeMissing(uid, *entity, attribute));
    }
    return std::unexpected(TypeMismatch("record or entity", object, objectSpan()));
}

// Rust `HasAttr` and `eval_extended_has_attr_value` (evaluator.rs:892): each step reads a record
// key or an entity attribute; a missing key, attribute or entity makes the whole test false, and
// a step on any other type is a type error. A type error on the first step points at the object;
// a later step's value carries no location, so its error has none and the caller's enclosing span
// applies.
[[nodiscard]] inline auto HasAttributePath(const Value& object, const std::span<const std::string_view> path,
                                           const EntityStoreView entities, const SpanLocator auto& objectSpan)
    -> OperatorResult
{
    Value current = object;
    bool first = true;
    for (const std::string_view attribute : path)
    {
        const Value* next = nullptr;
        if (current.Kind() == ValueKind::Record) { next = current.AsRecord().Find(attribute); }
        else if (current.Kind() == ValueKind::Entity)
        {
            const Entity* const entity = entities.Find(current.AsEntity());
            if (entity != nullptr) { next = entity->FindAttribute(attribute); }
        }
        else
        {
            return std::unexpected(
                TypeMismatch("record or entity", current, first ? objectSpan() : std::optional<SourceSpan>{}));
        }
        if (next == nullptr) { return Value::FromBool(false); }
        current = *next;
        first = false;
    }
    return Value::FromBool(true);
}

/// The extension function `name` applied to `arguments`, which checks the function, the argument
/// count and the argument types in Rust's order (CallExtensionFunction). A type error points at
/// the first argument of the wrong kind, as Rust's does.
[[nodiscard]] inline auto ApplyExtensionCall(const std::string_view name, const std::span<const Value> arguments,
                                             ValueArena& arena, const ArgumentSpanLocator auto& argumentSpan)
    -> OperatorResult
{
    OperatorResult result = CallExtensionFunction(name, arguments, arena);
    if (!result && result.error().Kind() == EvaluationErrorKind::TypeError)
    {
        if (const std::optional<std::size_t> position = MistypedArgumentPosition(name, arguments))
        {
            if (const std::optional<SourceSpan> span = argumentSpan(*position))
            {
                result.error().AttachSpanIfMissing(*span);
            }
        }
    }
    return result;
}

} // namespace cedar::detail
