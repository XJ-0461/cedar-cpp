#include "evaluation/Operators.hpp"

#include <utility>

namespace cedar::detail
{

namespace
{

auto TypeName(const ValueKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case ValueKind::Bool: return "bool";
    case ValueKind::Long: return "long";
    case ValueKind::String: return "string";
    case ValueKind::Entity: return "entity";
    case ValueKind::Set: return "set";
    case ValueKind::Record: return "record";
    case ValueKind::Datetime: return "datetime";
    case ValueKind::Decimal: return "decimal";
    case ValueKind::Duration: return "duration";
    case ValueKind::IpAddress: return "ipaddr";
    }
    return "value";
}

auto DescribeType(const Value& value) -> std::string
{
    if (value.Kind() == ValueKind::Entity)
    {
        std::string text{"(entity of type `"};
        text.append(value.AsEntity().Type().GetName().QualifiedText()).append("`)");
        return text;
    }
    return std::string{TypeName(value.Kind())};
}

} // namespace

auto Quoted(const std::string_view text) -> std::string
{
    std::string quoted{"`"};
    quoted.append(text).append("`");
    return quoted;
}

auto TypeMismatch(const std::string_view expected, const Value& actual, const std::optional<SourceSpan> span,
                  std::optional<std::string> help) -> EvaluationError
{
    std::string message{"type error: expected "};
    message.append(expected).append(", got ").append(DescribeType(actual));
    return EvaluationError{EvaluationErrorKind::TypeError, span, std::move(message), std::move(help)};
}

auto EntityMissing(const EntityUid& uid, const std::optional<SourceSpan> span) -> EvaluationError
{
    return EvaluationError{EvaluationErrorKind::EntityDoesNotExist, span,
                           "entity " + Quoted(uid.ToString()) + " does not exist"};
}

auto Overflow(const std::string_view verb, const Value& left, const Value* right) -> EvaluationError
{
    std::string message{"integer overflow while attempting to "};
    message.append(verb);
    if (right == nullptr) { message.append(" the value ").append(Quoted(ToCedar(left))); }
    else
    {
        message.append(" the values ").append(Quoted(ToCedar(left))).append(" and ").append(Quoted(ToCedar(*right)));
    }
    return EvaluationError{EvaluationErrorKind::IntegerOverflow, std::nullopt, std::move(message)};
}

// A long operand makes the other one the mistyped one; otherwise a comparable extension operand
// does; otherwise the left operand is wrong.
auto ComparisonTypeMismatch(const Value& left, const Value& right, const std::optional<SourceSpan> leftSpan,
                            const std::optional<SourceSpan> rightSpan) -> EvaluationError
{
    const ValueKind leftKind = left.Kind();
    const ValueKind rightKind = right.Kind();
    if (leftKind == ValueKind::Long) { return TypeMismatch("long", right, rightSpan); }
    if (rightKind == ValueKind::Long) { return TypeMismatch("long", left, leftSpan); }
    if (SupportsComparison(leftKind)) { return TypeMismatch(TypeName(leftKind), right, rightSpan); }
    if (SupportsComparison(rightKind)) { return TypeMismatch(TypeName(rightKind), left, leftSpan); }
    return TypeMismatch("long, datetime, or duration", left, leftSpan,
                        "only types long, datetime and duration support comparison");
}

auto InLeftTypeMismatch(const Value& left, const Value& right, const std::optional<SourceSpan> leftSpan)
    -> EvaluationError
{
    std::optional<std::string> help;
    if (right.Kind() == ValueKind::Set)
    {
        help = "`in` is for checking the entity hierarchy; use `.contains()` to test set membership";
    }
    else if (right.Kind() == ValueKind::Record)
    {
        help = "`in` is for checking the entity hierarchy; use `has` to test if a record has a key";
    }
    return TypeMismatch("entity", left, leftSpan, std::move(help));
}

auto RecordAttributeMissing(const std::string_view attribute) -> EvaluationError
{
    return EvaluationError{EvaluationErrorKind::RecordAttributeDoesNotExist, std::nullopt,
                           "record does not have the attribute " + Quoted(attribute)};
}

auto EntityAttributeMissing(const EntityUid& uid, const Entity& entity, const std::string_view attribute)
    -> EvaluationError
{
    std::optional<std::string> help;
    if (entity.FindTag(attribute) != nullptr)
    {
        help = "there is a tag named " + Quoted(attribute) + "; tags are read with `getTag`";
    }
    return EvaluationError{EvaluationErrorKind::EntityAttributeDoesNotExist, std::nullopt,
                           Quoted(uid.ToString()) + " does not have the attribute " + Quoted(attribute),
                           std::move(help)};
}

auto EntityTagMissing(const EntityUid& uid, const Entity& entity, const std::string_view tag) -> EvaluationError
{
    std::optional<std::string> help;
    if (entity.FindAttribute(tag) != nullptr)
    {
        help = "there is an attribute named " + Quoted(tag) + "; attributes are read with `.`";
    }
    return EvaluationError{EvaluationErrorKind::EntityTagDoesNotExist, std::nullopt,
                           Quoted(uid.ToString()) + " does not have the tag " + Quoted(tag), std::move(help)};
}

auto MistypedArgumentPosition(const std::string_view name, const std::span<const Value> arguments)
    -> std::optional<std::size_t>
{
    const ExtensionCall* function = FindExtensionCall(name);
    if (function == nullptr) { return std::nullopt; }
    for (std::size_t position = 0; position < arguments.size(); ++position)
    {
        const std::size_t slot = position < function->arity ? position : function->arity - 1U;
        if (arguments[position].Kind() != function->argumentKinds[slot]) { return position; }
    }
    return std::nullopt;
}

} // namespace cedar::detail
