#include <cedar/Expression.hpp>

#include "model/EscapeDebug.hpp"

#include <algorithm>
#include <cassert>
#include <vector>

namespace cedar
{

// ---- ExpressionArena ----

ExpressionArena::ExpressionArena() : values_{std::make_unique<ValueArena>()} {}
ExpressionArena::ExpressionArena(ExpressionArena&&) noexcept = default;
auto ExpressionArena::operator=(ExpressionArena&&) noexcept -> ExpressionArena& = default;
ExpressionArena::~ExpressionArena() = default;

// ---- ExpressionBuilder ----

auto ExpressionBuilder::Append(ExpressionNode node, const OptionalSpan span) -> ExpressionIndex
{
    assert(arena_->nodes_.size() < std::numeric_limits<std::uint32_t>::max());
    const auto index = static_cast<ExpressionIndex>(arena_->nodes_.size());
    arena_->nodes_.push_back(std::move(node));
    arena_->spans_.push_back(span.value_or(ExpressionArena::NoSpan));
    return index;
}

auto ExpressionBuilder::CheckChild([[maybe_unused]] const ExpressionIndex child) const noexcept -> void
{
    assert(std::to_underlying(child) < arena_->nodes_.size());
}

auto ExpressionBuilder::AppendChildren(const std::span<const ExpressionIndex> children) -> ArenaRange<ExpressionIndex>
{
    const auto first = static_cast<std::uint32_t>(arena_->children_.size());
    for (const ExpressionIndex child : children)
    {
        CheckChild(child);
        arena_->children_.push_back(child);
    }
    return {first, static_cast<std::uint32_t>(children.size())};
}

auto ExpressionBuilder::AppendTexts(const std::span<const std::string_view> texts) -> ArenaRange<std::string_view>
{
    const auto first = static_cast<std::uint32_t>(arena_->texts_.size());
    for (const std::string_view text : texts) { arena_->texts_.push_back(arena_->values_->CopyText(text)); }
    return {first, static_cast<std::uint32_t>(texts.size())};
}

auto ExpressionBuilder::Bool(const bool value, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Literal{Value::FromBool(value)}, span);
}

auto ExpressionBuilder::Long(const std::int64_t value, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Literal{Value::FromLong(value)}, span);
}

auto ExpressionBuilder::String(const std::string_view value, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Literal{arena_->values_->MakeString(value)}, span);
}

auto ExpressionBuilder::Entity(const EntityUid& value, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Literal{arena_->values_->MakeEntity(value)}, span);
}

auto ExpressionBuilder::Literal(const Value& value, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Literal{arena_->values_->DeepCopy(value)}, span);
}

auto ExpressionBuilder::Fold(const ExpressionIndex written, const Value& value, const OptionalSpan span)
    -> ExpressionIndex
{
    CheckChild(written);
    return Append(node::Folded{arena_->values_->DeepCopy(value), written}, span);
}

auto ExpressionBuilder::Variable(const cedar::Variable variable, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Variable{variable}, span);
}

auto ExpressionBuilder::Slot(const SlotId slot, const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::Slot{slot}, span);
}

auto ExpressionBuilder::If(const ExpressionIndex condition, const ExpressionIndex thenBranch,
                           const ExpressionIndex elseBranch, const OptionalSpan span) -> ExpressionIndex
{
    CheckChild(condition);
    CheckChild(thenBranch);
    CheckChild(elseBranch);
    return Append(node::If{condition, thenBranch, elseBranch}, span);
}

auto ExpressionBuilder::BoolLiterals(const ExpressionIndex left, const ExpressionIndex right) const noexcept
    -> std::optional<std::pair<bool, bool>>
{
    const auto* leftLiteral = std::get_if<node::Literal>(&arena_->Node(left));
    const auto* rightLiteral = std::get_if<node::Literal>(&arena_->Node(right));
    if (leftLiteral == nullptr || rightLiteral == nullptr || leftLiteral->value.Kind() != ValueKind::Bool ||
        rightLiteral->value.Kind() != ValueKind::Bool)
    {
        return std::nullopt;
    }
    return std::pair{leftLiteral->value.AsBool(), rightLiteral->value.AsBool()};
}

auto ExpressionBuilder::And(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span)
    -> ExpressionIndex
{
    CheckChild(left);
    CheckChild(right);
    if (const auto folded = BoolLiterals(left, right)) { return Bool(folded->first && folded->second, span); }
    return Append(node::And{left, right}, span);
}

auto ExpressionBuilder::Or(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span)
    -> ExpressionIndex
{
    CheckChild(left);
    CheckChild(right);
    if (const auto folded = BoolLiterals(left, right)) { return Bool(folded->first || folded->second, span); }
    return Append(node::Or{left, right}, span);
}

auto ExpressionBuilder::Unary(const UnaryOperator operation, const ExpressionIndex operand, const OptionalSpan span)
    -> ExpressionIndex
{
    CheckChild(operand);
    return Append(node::Unary{operation, operand}, span);
}

auto ExpressionBuilder::Binary(const BinaryOperator operation, const ExpressionIndex left,
                               const ExpressionIndex right, const OptionalSpan span) -> ExpressionIndex
{
    CheckChild(left);
    CheckChild(right);
    return Append(node::Binary{operation, left, right}, span);
}

auto ExpressionBuilder::NotEqual(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span)
    -> ExpressionIndex
{
    return Not(Binary(BinaryOperator::Equal, left, right, span), span);
}

auto ExpressionBuilder::Greater(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span)
    -> ExpressionIndex
{
    return Not(Binary(BinaryOperator::LessEqual, left, right, span), span);
}

auto ExpressionBuilder::GreaterEqual(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span)
    -> ExpressionIndex
{
    return Not(Binary(BinaryOperator::Less, left, right, span), span);
}

auto ExpressionBuilder::IsIn(const ExpressionIndex operand, const EntityType& type, const ExpressionIndex target,
                             const OptionalSpan span) -> ExpressionIndex
{
    const ExpressionIndex isType = Is(operand, type, span);
    const ExpressionIndex isIn = In(operand, target, span);
    return And(isType, isIn, span);
}

auto ExpressionBuilder::ExtensionCall(const Name& function, const std::span<const ExpressionIndex> arguments,
                                      const OptionalSpan span) -> ExpressionIndex
{
    return Append(node::ExtensionCall{function, AppendChildren(arguments)}, span);
}

auto ExpressionBuilder::GetAttribute(const ExpressionIndex object, const std::string_view attribute,
                                     const OptionalSpan span) -> ExpressionIndex
{
    CheckChild(object);
    return Append(node::GetAttribute{object, arena_->values_->CopyText(attribute)}, span);
}

auto ExpressionBuilder::HasAttribute(const ExpressionIndex object, const std::string_view attribute,
                                     const OptionalSpan span) -> ExpressionIndex
{
    return HasAttribute(object, std::span{&attribute, 1}, span);
}

auto ExpressionBuilder::HasAttribute(const ExpressionIndex object, const std::span<const std::string_view> path,
                                     const OptionalSpan span) -> ExpressionIndex
{
    assert(!path.empty());
    CheckChild(object);
    return Append(node::HasAttribute{object, AppendTexts(path)}, span);
}

auto ExpressionBuilder::Like(const ExpressionIndex operand, const std::span<const std::string_view> literals,
                             const OptionalSpan span) -> ExpressionIndex
{
    assert(!literals.empty());
    CheckChild(operand);
    return Append(node::Like{operand, AppendTexts(literals)}, span);
}

auto ExpressionBuilder::Is(const ExpressionIndex operand, const EntityType& type, const OptionalSpan span)
    -> ExpressionIndex
{
    CheckChild(operand);
    return Append(node::Is{operand, type}, span);
}

auto ExpressionBuilder::SetLiteral(const std::span<const ExpressionIndex> elements, const OptionalSpan span)
    -> ExpressionIndex
{
    return Append(node::SetLiteral{AppendChildren(elements)}, span);
}

auto ExpressionBuilder::RecordLiteral(const std::span<const RecordLiteralField> fields, const OptionalSpan span)
    -> std::expected<ExpressionIndex, DuplicateRecordKey>
{
    std::vector<RecordLiteralField> sorted(fields.begin(), fields.end());
    std::ranges::stable_sort(sorted, {}, &RecordLiteralField::key);
    const auto duplicate = std::ranges::adjacent_find(sorted, {}, &RecordLiteralField::key);
    if (duplicate != sorted.end()) { return std::unexpected{DuplicateRecordKey{duplicate->key}}; }

    const auto first = static_cast<std::uint32_t>(arena_->fields_.size());
    for (const RecordLiteralField& field : sorted)
    {
        CheckChild(field.value);
        arena_->fields_.push_back({arena_->values_->CopyText(field.key), field.value});
    }
    return Append(node::RecordLiteral{{first, static_cast<std::uint32_t>(sorted.size())}}, span);
}

// ---- DumpExpression ----

namespace
{

void AppendQuoted(std::string& output, const std::string_view text)
{
    output.push_back('"');
    detail::AppendEscapeDebug(output, text);
    output.push_back('"');
}

class Dumper
{
public:
    explicit Dumper(const ExpressionArena& arena, std::string& output) : arena_{arena}, output_{output} {}

    void Dump(const ExpressionIndex index)
    {
        arena_.Visit(index, Overloaded{
            [&](const node::Literal& literal) { AppendCedar(output_, literal.value); },
            [&](const node::Folded& folded) {
                output_ += "(folded ";
                AppendCedar(output_, folded.value);
                output_.push_back(' ');
                Dump(folded.written);
                output_.push_back(')');
            },
            [&](const node::Variable& variable) { output_ += ToString(variable.variable); },
            [&](const node::Slot& slot) { output_ += ToString(slot.slot); },
            [&](const node::If& branch) { List("if", {branch.condition, branch.thenBranch, branch.elseBranch}); },
            [&](const node::And& conjunction) { List("&&", {conjunction.left, conjunction.right}); },
            [&](const node::Or& disjunction) { List("||", {disjunction.left, disjunction.right}); },
            [&](const node::Unary& unary) {
                List(unary.operation == UnaryOperator::Negate ? "neg" : ToString(unary.operation), {unary.operand});
            },
            [&](const node::Binary& binary) { List(ToString(binary.operation), {binary.left, binary.right}); },
            [&](const node::ExtensionCall& call) {
                output_ += "(call ";
                output_ += call.function.QualifiedText();
                for (const ExpressionIndex argument : arena_.Elements(call.arguments))
                {
                    output_.push_back(' ');
                    Dump(argument);
                }
                output_.push_back(')');
            },
            [&](const node::GetAttribute& get) {
                output_ += "(. ";
                Dump(get.object);
                output_.push_back(' ');
                AppendQuoted(output_, get.attribute);
                output_.push_back(')');
            },
            [&](const node::HasAttribute& has) {
                output_ += "(has ";
                Dump(has.object);
                for (const std::string_view attribute : arena_.Elements(has.path))
                {
                    output_.push_back(' ');
                    AppendQuoted(output_, attribute);
                }
                output_.push_back(')');
            },
            [&](const node::Like& like) {
                output_ += "(like ";
                Dump(like.operand);
                output_ += " \"";
                bool firstLiteral = true;
                for (const std::string_view literal : arena_.PatternOf(like).Literals())
                {
                    if (!firstLiteral) { output_.push_back('*'); }
                    firstLiteral = false;
                    std::string_view rest = literal;
                    for (std::size_t star = rest.find('*'); star != std::string_view::npos; star = rest.find('*'))
                    {
                        detail::AppendEscapeDebug(output_, rest.substr(0, star));
                        output_ += "\\*";
                        rest.remove_prefix(star + 1);
                    }
                    detail::AppendEscapeDebug(output_, rest);
                }
                output_ += "\")";
            },
            [&](const node::Is& is) {
                output_ += "(is ";
                Dump(is.operand);
                output_.push_back(' ');
                output_ += is.type.GetName().QualifiedText();
                output_.push_back(')');
            },
            [&](const node::SetLiteral& set) {
                output_ += "(set";
                for (const ExpressionIndex element : arena_.Elements(set.elements))
                {
                    output_.push_back(' ');
                    Dump(element);
                }
                output_.push_back(')');
            },
            [&](const node::RecordLiteral& record) {
                output_ += "(record";
                for (const RecordLiteralField& field : arena_.Elements(record.fields))
                {
                    output_ += " (";
                    AppendQuoted(output_, field.key);
                    output_.push_back(' ');
                    Dump(field.value);
                    output_.push_back(')');
                }
                output_.push_back(')');
            },
        });
    }

private:
    void List(const std::string_view head, const std::initializer_list<ExpressionIndex> children)
    {
        output_.push_back('(');
        output_ += head;
        for (const ExpressionIndex child : children)
        {
            output_.push_back(' ');
            Dump(child);
        }
        output_.push_back(')');
    }

    const ExpressionArena& arena_;
    std::string& output_;
};

} // namespace

auto DumpExpression(const ExpressionArena& arena, const ExpressionIndex root) -> std::string
{
    std::string output;
    Dumper{arena, output}.Dump(root);
    return output;
}

} // namespace cedar
