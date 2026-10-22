#include "evaluation/Evaluator.hpp"

#include "extensions/ExtensionCalls.hpp"

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

auto Quoted(const std::string_view text) -> std::string
{
    std::string quoted{"`"};
    quoted.append(text).append("`");
    return quoted;
}

[[gnu::cold]] auto TypeMismatch(const std::string_view expected, const Value& actual,
                                const std::optional<SourceSpan> span,
                                std::optional<std::string> help = std::nullopt) -> EvaluationError
{
    std::string message{"type error: expected "};
    message.append(expected).append(", got ").append(DescribeType(actual));
    return EvaluationError{EvaluationErrorKind::TypeError, span, std::move(message), std::move(help)};
}

[[gnu::cold]] auto EntityMissing(const EntityUid& uid, const std::optional<SourceSpan> span) -> EvaluationError
{
    return EvaluationError{EvaluationErrorKind::EntityDoesNotExist, span,
                           "entity " + Quoted(uid.ToString()) + " does not exist"};
}

[[gnu::cold]] auto Overflow(const std::string_view verb, const Value& left, const Value* right) -> EvaluationError
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

// The extension types `<` and `<=` accept (Rust `supports_operator_overloading`).
constexpr auto SupportsComparison(const ValueKind kind) noexcept -> bool
{
    return kind == ValueKind::Datetime || kind == ValueKind::Duration;
}

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
    const std::optional<SourceSpan> span = expressions_.Span(node.operand);
    switch (node.operation)
    {
    case UnaryOperator::Not:
        if (operand->Kind() != ValueKind::Bool) { return std::unexpected(TypeMismatch("bool", *operand, span)); }
        return Value::FromBool(!operand->AsBool());
    case UnaryOperator::Negate:
    {
        if (operand->Kind() != ValueKind::Long) { return std::unexpected(TypeMismatch("long", *operand, span)); }
        std::int64_t negated = 0;
        if (__builtin_sub_overflow(std::int64_t{0}, operand->AsLong(), &negated))
        {
            return std::unexpected(Overflow("negate", *operand, nullptr));
        }
        return Value::FromLong(negated);
    }
    case UnaryOperator::IsEmpty:
        if (operand->Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", *operand, span)); }
        return Value::FromBool(operand->AsSet().IsEmpty());
    }
    return operand;
}

// Rust evaluates both operands, left first, before any type check (evaluator.rs BinaryApp).
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Binary& node) -> Result
{
    const Result left = Evaluate(node.left);
    if (!left) { return left; }
    const Result right = Evaluate(node.right);
    if (!right) { return right; }
    return ApplyBinary(node, *left, *right);
}

auto Evaluator::ApplyBinary(const node::Binary& node, const Value& left, const Value& right) -> Result
{
    const auto leftSpan = [&] { return expressions_.Span(node.left); };
    const auto rightSpan = [&] { return expressions_.Span(node.right); };
    switch (node.operation)
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
    // supports operator overloading (datetime, duration); the arms below pick the type error.
    case BinaryOperator::Less:
    case BinaryOperator::LessEqual:
    {
        const bool strict = node.operation == BinaryOperator::Less;
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
        if (leftKind == ValueKind::Long) { return std::unexpected(TypeMismatch("long", right, rightSpan())); }
        if (rightKind == ValueKind::Long) { return std::unexpected(TypeMismatch("long", left, leftSpan())); }
        if (SupportsComparison(leftKind))
        {
            return std::unexpected(TypeMismatch(TypeName(leftKind), right, rightSpan()));
        }
        if (SupportsComparison(rightKind))
        {
            return std::unexpected(TypeMismatch(TypeName(rightKind), left, leftSpan()));
        }
        return std::unexpected(TypeMismatch("long, datetime, or duration", left, leftSpan(),
                                            "only types long, datetime and duration support comparison"));
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
        switch (node.operation)
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

    case BinaryOperator::In: return EvaluateIn(node, left, right);

    case BinaryOperator::Contains:
        if (left.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", left, leftSpan())); }
        return Value::FromBool(left.AsSet().Contains(right));

    case BinaryOperator::ContainsAll:
    case BinaryOperator::ContainsAny:
        if (left.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", left, leftSpan())); }
        if (right.Kind() != ValueKind::Set) { return std::unexpected(TypeMismatch("set", right, rightSpan())); }
        return Value::FromBool(node.operation == BinaryOperator::ContainsAll ? left.AsSet().ContainsAll(right.AsSet())
                                                                             : left.AsSet().ContainsAny(right.AsSet()));

    case BinaryOperator::GetTag:
    case BinaryOperator::HasTag: return EvaluateTag(node, left, right);
    }
    return Value::FromBool(false);
}

// Rust `eval_in` (evaluator.rs:811, entity.rs:606): reflexive equality, or membership in the left
// entity's closed ancestor set; a missing left entity is equal to itself only. Every element of a
// set right-hand side is checked to be an entity before any is compared. An element carries no
// location, so its type error takes the whole expression's span (Rust points at the element of a
// set literal, and at the whole expression for a set from entity or context data).
auto Evaluator::EvaluateIn(const node::Binary& node, const Value& left, const Value& right) -> Result
{
    if (left.Kind() != ValueKind::Entity)
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
        return std::unexpected(TypeMismatch("entity", left, expressions_.Span(node.left), std::move(help)));
    }
    const EntityUid& descendant = left.AsEntity();
    const Entity* const entity = entities_.Find(descendant);
    const auto isIn = [&](const EntityUid& ancestor) {
        return descendant == ancestor || (entity != nullptr && entity->IsDescendantOf(ancestor));
    };

    if (right.Kind() == ValueKind::Entity) { return Value::FromBool(isIn(right.AsEntity())); }
    if (right.Kind() != ValueKind::Set)
    {
        return std::unexpected(TypeMismatch("set or entity", right, expressions_.Span(node.right)));
    }
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

auto Evaluator::EvaluateTag(const node::Binary& node, const Value& left, const Value& right) -> Result
{
    if (left.Kind() != ValueKind::Entity)
    {
        return std::unexpected(TypeMismatch("entity", left, expressions_.Span(node.left)));
    }
    if (right.Kind() != ValueKind::String)
    {
        return std::unexpected(TypeMismatch("string", right, expressions_.Span(node.right)));
    }
    const Entity* const entity = entities_.Find(left.AsEntity());
    if (node.operation == BinaryOperator::HasTag)
    {
        return Value::FromBool(entity != nullptr && entity->FindTag(right.AsString()) != nullptr);
    }
    if (entity == nullptr) { return std::unexpected(EntityMissing(left.AsEntity(), expressions_.Span(node.left))); }
    if (const Value* tag = entity->FindTag(right.AsString())) { return *tag; }
    std::optional<std::string> help;
    if (entity->FindAttribute(right.AsString()) != nullptr)
    {
        help = "there is an attribute named " + Quoted(right.AsString()) + "; attributes are read with `.`";
    }
    return std::unexpected(EvaluationError{EvaluationErrorKind::EntityTagDoesNotExist, std::nullopt,
                                           Quoted(left.AsEntity().ToString()) + " does not have the tag " +
                                               Quoted(right.AsString()),
                                           std::move(help)});
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

    const std::string_view name = node.function.QualifiedText();
    Result result = CallExtensionFunction(name, arguments, arena_);
    if (!result && result.error().Kind() == EvaluationErrorKind::TypeError)
    {
        if (const ExtensionCall* function = FindExtensionCall(name))
        {
            for (std::size_t position = 0; position < arguments.size(); ++position)
            {
                const std::size_t slot = position < function->arity ? position : function->arity - 1U;
                if (arguments[position].Kind() == function->argumentKinds[slot]) { continue; }
                if (const std::optional<SourceSpan> span = expressions_.Span(argumentNodes[position]))
                {
                    result.error().AttachSpanIfMissing(*span);
                }
                break;
            }
        }
    }
    return result;
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::GetAttribute& node) -> Result
{
    const Result object = Evaluate(node.object);
    if (!object) { return object; }
    if (object->Kind() == ValueKind::Record)
    {
        if (const Value* value = object->AsRecord().Find(node.attribute)) { return *value; }
        return std::unexpected(EvaluationError{EvaluationErrorKind::RecordAttributeDoesNotExist, std::nullopt,
                                               "record does not have the attribute " + Quoted(node.attribute)});
    }
    if (object->Kind() == ValueKind::Entity)
    {
        const EntityUid& uid = object->AsEntity();
        const Entity* const entity = entities_.Find(uid);
        if (entity == nullptr) { return std::unexpected(EntityMissing(uid, expressions_.Span(node.object))); }
        if (const Value* value = entity->FindAttribute(node.attribute)) { return *value; }
        std::optional<std::string> help;
        if (entity->FindTag(node.attribute) != nullptr)
        {
            help = "there is a tag named " + Quoted(node.attribute) + "; tags are read with `getTag`";
        }
        return std::unexpected(EvaluationError{EvaluationErrorKind::EntityAttributeDoesNotExist, std::nullopt,
                                               Quoted(uid.ToString()) + " does not have the attribute " +
                                                   Quoted(node.attribute),
                                               std::move(help)});
    }
    return std::unexpected(TypeMismatch("record or entity", *object, expressions_.Span(node.object)));
}

// Rust `HasAttr` and `eval_extended_has_attr_value` (evaluator.rs:892): each step reads a record
// key or an entity attribute; a missing key, attribute or entity makes the whole test false, and
// a step on any other type is a type error. A type error on the first step points at the object;
// a later step's value carries no location, so its error takes the whole expression's span.
[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::HasAttribute& node) -> Result
{
    const Result object = Evaluate(node.object);
    if (!object) { return object; }
    Value current = *object;
    std::optional<SourceSpan> currentSpan = expressions_.Span(node.object);
    for (const std::string_view attribute : expressions_.Elements(node.path))
    {
        const Value* next = nullptr;
        if (current.Kind() == ValueKind::Record) { next = current.AsRecord().Find(attribute); }
        else if (current.Kind() == ValueKind::Entity)
        {
            const Entity* const entity = entities_.Find(current.AsEntity());
            if (entity != nullptr) { next = entity->FindAttribute(attribute); }
        }
        else { return std::unexpected(TypeMismatch("record or entity", current, currentSpan)); }
        if (next == nullptr) { return Value::FromBool(false); }
        current = *next;
        currentSpan = std::nullopt;
    }
    return Value::FromBool(true);
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Like& node) -> Result
{
    const Result operand = Evaluate(node.operand);
    if (!operand) { return operand; }
    if (operand->Kind() != ValueKind::String)
    {
        return std::unexpected(TypeMismatch("string", *operand, expressions_.Span(node.operand)));
    }
    return Value::FromBool(expressions_.PatternOf(node).Matches(operand->AsString()));
}

[[gnu::noinline]] auto Evaluator::EvaluateNode(const node::Is& node) -> Result
{
    const Result operand = Evaluate(node.operand);
    if (!operand) { return operand; }
    if (operand->Kind() != ValueKind::Entity)
    {
        return std::unexpected(TypeMismatch("entity", *operand, expressions_.Span(node.operand)));
    }
    return Value::FromBool(operand->AsEntity().Type() == node.type);
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
