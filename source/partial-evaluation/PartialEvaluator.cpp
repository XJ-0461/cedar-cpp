#include "partial-evaluation/PartialEvaluator.hpp"

#include "evaluation/Evaluator.hpp"
#include "evaluation/Operators.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Rust's `tpe::evaluator`, arm by arm. Each interpreted node becomes a node of a fresh arena,
// carrying a type: a Literal is Rust's `Residual::Concrete`, the call `error()` is
// `Residual::Error`, and anything else is `Residual::Partial`. A node keeps the type of the node it
// was interpreted from; an arm that returns an operand's result keeps that operand's type.
//
// Extension values need no normalization: a cedar-cpp Value holds no constructor text, so every
// extension value is already in its canonical form.

namespace cedar::detail
{

namespace
{

/// The store with no entities, for the operators that do not read entities.
struct NoEntities
{
    [[nodiscard]] auto Find(const EntityUid& /*uid*/) const noexcept -> const Entity* { return nullptr; }
};

constexpr NoEntities EmptyStore{};

class PartialInterpreter final
{
public:
    PartialInterpreter(const Residual& input, const PartialRequest& request, const PartialEntities& entities,
                       const ValidatorSchema& schema)
        : input_{input}, source_{input.Arena()}, request_{request}, entities_{entities}, schema_{schema},
          interpreted_(source_.Size())
    {
    }

    [[nodiscard]] auto Run() && -> Residual
    {
        const ExpressionIndex root = Interpret(input_.Root());
        types_.resize(arena_.Size());
        return Residual::Create(ResidualTree{std::move(arena_), std::move(types_), root});
    }

private:
    // ---- Output nodes ----

    auto Typed(const ExpressionIndex index, const Type& type) -> ExpressionIndex
    {
        types_.resize(arena_.Size());
        types_[std::to_underlying(index)] = type;
        return index;
    }

    /// Rust's `mk_concrete`.
    auto Concrete(const Value& value, const Type& type) -> ExpressionIndex
    {
        return Typed(builder_.Literal(value), type);
    }

    auto Concrete(const bool value, const Type& type) -> ExpressionIndex
    {
        return Concrete(Value::FromBool(value), type);
    }

    /// Rust's `mk_error`.
    auto Error(const Type& type) -> ExpressionIndex
    {
        return Typed(builder_.ExtensionCall(ResidualErrorName(), {}), type);
    }

    /// The value of an operator's result, or the error residual when it failed.
    auto ConcreteOrError(const OperatorResult& result, const Type& type) -> ExpressionIndex
    {
        return result ? Concrete(*result, type) : Error(type);
    }

    [[nodiscard]] auto ValueOf(const ExpressionIndex index) const noexcept -> const Value*
    {
        const auto* const literal = std::get_if<node::Literal>(&arena_.Node(index));
        return literal != nullptr ? &literal->value : nullptr;
    }

    [[nodiscard]] auto IsConcrete(const ExpressionIndex index) const noexcept -> bool
    {
        return ValueOf(index) != nullptr;
    }

    [[nodiscard]] auto IsError(const ExpressionIndex index) const noexcept -> bool
    {
        const auto* const call = std::get_if<node::ExtensionCall>(&arena_.Node(index));
        return call != nullptr && call->arguments.count == 0 && call->function == ResidualErrorName();
    }

    [[nodiscard]] auto TypeOf(const ExpressionIndex index) const noexcept -> const Type&
    {
        return types_[std::to_underlying(index)];
    }

    [[nodiscard]] auto CanError(const ExpressionIndex index) const -> bool
    {
        return CanErrorAssumingWellFormed(arena_, index);
    }

    // ---- interpret ----

    /// Rust's `interpret`. A node shared by several parents is interpreted once.
    auto Interpret(const ExpressionIndex index) -> ExpressionIndex
    {
        if (const auto& done = interpreted_[std::to_underlying(index)]) { return *done; }
        const ExpressionIndex result = InterpretNode(index);
        interpreted_[std::to_underlying(index)] = result;
        return result;
    }

    auto InterpretNode(const ExpressionIndex index) -> ExpressionIndex
    {
        const Type& type = input_.TypeOf(index);
        const ExpressionNode& node = source_.Node(index);
        if (const auto* const literal = std::get_if<node::Literal>(&node)) { return Concrete(literal->value, type); }
        if (const auto* const call = std::get_if<node::ExtensionCall>(&node);
            call != nullptr && call->arguments.count == 0 && call->function == ResidualErrorName())
        {
            return Error(type);
        }

        // Rust's `stack_size_check`.
        if (depth_ >= MaximumEvaluationDepth) { return Error(type); }
        ++depth_;
        const ExpressionIndex result =
            Visit(node, [&](const auto& alternative) { return InterpretArm(alternative, type); });
        --depth_;
        return result;
    }

    auto InterpretArm(const node::Literal& literal, const Type& type) -> ExpressionIndex
    {
        return Concrete(literal.value, type);
    }

    auto InterpretArm(const node::Folded& folded, const Type& type) -> ExpressionIndex
    {
        return Concrete(folded.value, type);
    }

    // A residual holds no slot.
    auto InterpretArm(const node::Slot& /*slot*/, const Type& type) -> ExpressionIndex { return Error(type); }

    auto InterpretArm(const node::Variable& variable, const Type& type) -> ExpressionIndex
    {
        const auto knownUid = [&](const PartialEntityUid& uid) -> ExpressionIndex
        {
            if (!uid.id) { return Typed(builder_.Variable(variable.variable), type); }
            return Typed(builder_.Entity(EntityUid{uid.type, *uid.id}), type);
        };
        switch (variable.variable)
        {
        case Variable::Action: return Typed(builder_.Entity(request_.Action()), type);
        case Variable::Principal: return knownUid(request_.Principal());
        case Variable::Resource: return knownUid(request_.Resource());
        case Variable::Context:
            if (const std::optional<Context>& context = request_.GetContext()) { return Concrete(context->AsValue(), type); }
            return Typed(builder_.Variable(Variable::Context), type);
        }
        return Error(type);
    }

    /// `&&` when `isAnd`, `||` otherwise: the two arms are mirror images, with `absorbing` the
    /// value that decides the operation (false for `&&`, true for `||`).
    auto InterpretShortCircuit(const ExpressionIndex leftSource, const ExpressionIndex rightSource, const bool isAnd,
                               const Type& type) -> ExpressionIndex
    {
        const bool absorbing = !isAnd;
        const auto build = [&](const ExpressionIndex left, const ExpressionIndex right)
        { return Typed(isAnd ? builder_.And(left, right) : builder_.Or(left, right), type); };

        const ExpressionIndex left = Interpret(leftSource);
        if (const Value* const value = ValueOf(left))
        {
            if (value->Kind() != ValueKind::Bool) { return Error(type); }
            if (value->AsBool() == absorbing) { return Concrete(absorbing, type); }
            return Interpret(rightSource);
        }
        if (IsError(left)) { return Error(type); }

        const ExpressionIndex right = Interpret(rightSource);
        if (const Value* const value = ValueOf(right))
        {
            if (value->Kind() != ValueKind::Bool) { return build(left, Error(type)); }
            if (value->AsBool() != absorbing) { return left; }
            if (!CanError(left)) { return Concrete(absorbing, type); }
            return build(left, Concrete(absorbing, type));
        }
        return build(left, right);
    }

    auto InterpretArm(const node::And& operation, const Type& type) -> ExpressionIndex
    {
        return InterpretShortCircuit(operation.left, operation.right, true, type);
    }

    auto InterpretArm(const node::Or& operation, const Type& type) -> ExpressionIndex
    {
        return InterpretShortCircuit(operation.left, operation.right, false, type);
    }

    auto InterpretArm(const node::If& branch, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex test = Interpret(branch.condition);
        if (const Value* const value = ValueOf(test))
        {
            if (value->Kind() != ValueKind::Bool) { return Error(type); }
            return Interpret(value->AsBool() ? branch.thenBranch : branch.elseBranch);
        }
        if (IsError(test)) { return Error(type); }
        const ExpressionIndex thenBranch = Interpret(branch.thenBranch);
        const ExpressionIndex elseBranch = Interpret(branch.elseBranch);
        return Typed(builder_.If(test, thenBranch, elseBranch), type);
    }

    auto InterpretArm(const node::Is& test, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex operand = Interpret(test.operand);
        if (const std::optional<bool> decided = TryDecideIs(operand, test.type)) { return Concrete(*decided, type); }
        if (const Value* const value = ValueOf(operand))
        {
            return ConcreteOrError(ApplyIs(*value, test.type, NoSpan{}), type);
        }
        if (IsError(operand)) { return Error(type); }
        return Typed(builder_.Is(operand, test.type), type);
    }

    auto InterpretArm(const node::Like& test, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex operand = Interpret(test.operand);
        if (const Value* const value = ValueOf(operand))
        {
            return ConcreteOrError(ApplyLike(*value, source_.PatternOf(test), NoSpan{}), type);
        }
        if (IsError(operand)) { return Error(type); }
        return Typed(builder_.Like(operand, source_.Elements(test.pattern)), type);
    }

    auto InterpretArm(const node::Binary& operation, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex left = Interpret(operation.left);
        const ExpressionIndex right = Interpret(operation.right);
        if (const std::optional<bool> decided = TryDecideBinary(operation.operation, left, right))
        {
            return Concrete(*decided, type);
        }
        const auto residual = [&] { return Typed(builder_.Binary(operation.operation, left, right), type); };
        const Value* const leftValue = ValueOf(left);
        const Value* const rightValue = ValueOf(right);
        if (leftValue == nullptr || rightValue == nullptr)
        {
            if (IsError(left) || IsError(right)) { return Error(type); }
            return residual();
        }
        // Copies: building nodes may move the arena's node storage, not the values it owns.
        const Value first = *leftValue;
        const Value second = *rightValue;

        switch (operation.operation)
        {
        case BinaryOperator::Equal:
        case BinaryOperator::Less:
        case BinaryOperator::LessEqual:
        case BinaryOperator::Add:
        case BinaryOperator::Subtract:
        case BinaryOperator::Multiply:
        case BinaryOperator::Contains:
        case BinaryOperator::ContainsAll:
        case BinaryOperator::ContainsAny:
            return ConcreteOrError(
                ApplyBinary(operation.operation, first, second, EntityStoreView{EmptyStore}, NoSpan{}, NoSpan{}), type);

        case BinaryOperator::In:
        {
            if (first.Kind() != ValueKind::Entity) { return Error(type); }
            const EntityUid& descendant = first.AsEntity();
            const PartialEntity* const entity = entities_.Find(descendant);
            const std::optional<EntityUidSetView> ancestors =
                entity != nullptr ? entity->Ancestors() : std::optional<EntityUidSetView>{};
            if (second.Kind() == ValueKind::Entity)
            {
                if (descendant == second.AsEntity()) { return Concrete(true, type); }
                if (ancestors) { return Concrete(ancestors->Contains(second.AsEntity()), type); }
                return residual();
            }
            if (second.Kind() != ValueKind::Set) { return Error(type); }
            const Set candidates = second.AsSet();
            for (const Value& element : candidates)
            {
                if (element.Kind() != ValueKind::Entity) { return Error(type); }
            }
            for (const Value& element : candidates)
            {
                if (element.AsEntity() == descendant || (ancestors && ancestors->Contains(element.AsEntity())))
                {
                    return Concrete(true, type);
                }
            }
            if (!candidates.IsEmpty() && !ancestors) { return residual(); }
            return Concrete(false, type);
        }

        case BinaryOperator::GetTag:
        case BinaryOperator::HasTag:
        {
            if (first.Kind() != ValueKind::Entity) { return Error(type); }
            if (second.Kind() != ValueKind::String) { return Error(type); }
            const PartialEntity* const entity = entities_.Find(first.AsEntity());
            const std::optional<Record> tags = entity != nullptr ? entity->Tags() : std::optional<Record>{};
            if (!tags) { return residual(); }
            const Value* const tag = tags->Find(second.AsString());
            if (operation.operation == BinaryOperator::HasTag) { return Concrete(tag != nullptr, type); }
            return tag != nullptr ? Concrete(*tag, type) : Error(type);
        }
        }
        return Error(type);
    }

    auto InterpretArm(const node::GetAttribute& access, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex object = Interpret(access.object);
        if (const Value* const value = ValueOf(object))
        {
            if (value->Kind() == ValueKind::Entity)
            {
                const PartialEntity* const entity = entities_.Find(value->AsEntity());
                const std::optional<Record> attributes =
                    entity != nullptr ? entity->Attributes() : std::optional<Record>{};
                if (!attributes) { return Typed(builder_.GetAttribute(object, access.attribute), type); }
                const Value* const attribute = attributes->Find(access.attribute);
                return attribute != nullptr ? Concrete(*attribute, type) : Error(type);
            }
            return ConcreteOrError(GetAttribute(*value, access.attribute, EntityStoreView{EmptyStore}, NoSpan{}), type);
        }
        if (IsError(object)) { return Error(type); }
        return Typed(builder_.GetAttribute(object, access.attribute), type);
    }

    auto InterpretArm(const node::HasAttribute& test, const Type& type) -> ExpressionIndex
    {
        const std::span<const std::string_view> path = source_.Elements(test.path);
        return test.extended ? InterpretExtendedHasAttribute(test.object, path, type)
                             : InterpretHasAttribute(test.object, path, type);
    }

    /// Rust's `HasAttr` arm.
    auto InterpretHasAttribute(const ExpressionIndex objectSource, const std::span<const std::string_view> path,
                               const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex object = Interpret(objectSource);
        if (const std::optional<bool> decided = TryDecideHas(object, path.front())) { return Concrete(*decided, type); }
        if (const Value* const value = ValueOf(object))
        {
            if (value->Kind() == ValueKind::Entity)
            {
                const PartialEntity* const entity = entities_.Find(value->AsEntity());
                const std::optional<Record> attributes =
                    entity != nullptr ? entity->Attributes() : std::optional<Record>{};
                if (!attributes) { return Typed(builder_.HasAttribute(object, path), type); }
                return Concrete(attributes->Contains(path.front()), type);
            }
            return ConcreteOrError(HasAttributePath(*value, path, EntityStoreView{EmptyStore}, NoSpan{}), type);
        }
        if (IsError(object)) { return Error(type); }
        return Typed(builder_.HasAttribute(object, path), type);
    }

    /// Rust's `ExtHasAttr` arm: the path is walked with short-circuiting, and the rest of it stays a
    /// residual from the first entity whose attributes are unknown.
    auto InterpretExtendedHasAttribute(const ExpressionIndex objectSource,
                                       const std::span<const std::string_view> path, const Type& type)
        -> ExpressionIndex
    {
        const auto tryDecide = [&](const ExpressionIndex object, const std::span<const std::string_view> rest)
        {
            if (const std::optional<bool> decided = TryDecideHas(object, rest.front()))
            {
                return Concrete(*decided, type);
            }
            return Typed(builder_.ExtendedHasAttribute(object, rest), type);
        };

        const ExpressionIndex object = Interpret(objectSource);
        if (const Value* const value = ValueOf(object))
        {
            Value current = *value;
            for (std::size_t position = 0; position < path.size(); ++position)
            {
                std::optional<Record> attributes;
                if (current.Kind() == ValueKind::Record) { attributes = current.AsRecord(); }
                else if (current.Kind() == ValueKind::Entity)
                {
                    const PartialEntity* const entity = entities_.Find(current.AsEntity());
                    if (entity != nullptr) { attributes = entity->Attributes(); }
                    if (!attributes)
                    {
                        const ExpressionIndex reached =
                            Concrete(current, NamedEntityType(current.AsEntity().Type()));
                        return tryDecide(reached, path.subspan(position));
                    }
                }
                else
                {
                    return Error(type);
                }
                const Value* const next = attributes->Find(path[position]);
                if (next == nullptr) { return Concrete(false, type); }
                current = *next;
            }
            return Concrete(true, type);
        }
        if (IsError(object)) { return Error(type); }
        return tryDecide(object, path);
    }

    auto InterpretArm(const node::Unary& operation, const Type& type) -> ExpressionIndex
    {
        const ExpressionIndex operand = Interpret(operation.operand);
        if (const Value* const value = ValueOf(operand))
        {
            return ConcreteOrError(ApplyUnary(operation.operation, *value, NoSpan{}), type);
        }
        if (IsError(operand)) { return Error(type); }
        return Typed(builder_.Unary(operation.operation, operand), type);
    }

    /// Interprets each of `sources` in order. Whether all are concrete, and whether any is an error.
    struct Interpreted
    {
        std::vector<ExpressionIndex> nodes;
        bool allConcrete = true;
        bool anyError = false;
    };

    auto InterpretAll(const std::span<const ExpressionIndex> sources) -> Interpreted
    {
        Interpreted result;
        result.nodes.reserve(sources.size());
        for (const ExpressionIndex source : sources)
        {
            const ExpressionIndex node = Interpret(source);
            result.allConcrete = result.allConcrete && IsConcrete(node);
            result.anyError = result.anyError || IsError(node);
            result.nodes.push_back(node);
        }
        return result;
    }

    [[nodiscard]] auto ValuesOf(const std::span<const ExpressionIndex> nodes) const -> std::vector<Value>
    {
        std::vector<Value> values;
        values.reserve(nodes.size());
        for (const ExpressionIndex node : nodes) { values.push_back(*ValueOf(node)); }
        return values;
    }

    auto InterpretArm(const node::ExtensionCall& call, const Type& type) -> ExpressionIndex
    {
        const Interpreted arguments = InterpretAll(source_.Elements(call.arguments));
        if (arguments.allConcrete)
        {
            const std::vector<Value> values = ValuesOf(arguments.nodes);
            return ConcreteOrError(ApplyExtensionCall(call.function.QualifiedText(), values, scratch_, NoSpan{}),
                                   type);
        }
        if (arguments.anyError) { return Error(type); }
        return Typed(builder_.ExtensionCall(call.function, arguments.nodes), type);
    }

    auto InterpretArm(const node::SetLiteral& set, const Type& type) -> ExpressionIndex
    {
        const Interpreted elements = InterpretAll(source_.Elements(set.elements));
        if (elements.allConcrete)
        {
            const std::vector<Value> values = ValuesOf(elements.nodes);
            return Concrete(scratch_.MakeSet(values), type);
        }
        if (elements.anyError) { return Error(type); }
        return Typed(builder_.SetLiteral(elements.nodes), type);
    }

    auto InterpretArm(const node::RecordLiteral& record, const Type& type) -> ExpressionIndex
    {
        const std::span<const RecordLiteralField> fields = source_.Elements(record.fields);
        std::vector<ExpressionIndex> sources;
        sources.reserve(fields.size());
        for (const RecordLiteralField& field : fields) { sources.push_back(field.value); }
        const Interpreted values = InterpretAll(sources);
        if (values.allConcrete)
        {
            std::vector<RecordEntry> entries;
            entries.reserve(fields.size());
            for (std::size_t position = 0; position < fields.size(); ++position)
            {
                entries.push_back(RecordEntry{fields[position].key, *ValueOf(values.nodes[position])});
            }
            // The keys come from a record literal, so none repeats.
            return Concrete(*scratch_.MakeRecord(entries), type);
        }
        if (values.anyError) { return Error(type); }
        std::vector<RecordLiteralField> rebuilt;
        rebuilt.reserve(fields.size());
        for (std::size_t position = 0; position < fields.size(); ++position)
        {
            rebuilt.push_back(RecordLiteralField{fields[position].key, values.nodes[position]});
        }
        return Typed(*builder_.RecordLiteral(rebuilt), type);
    }

    // ---- The reductions by type ----

    /// Rust's `try_decide_is_residual`.
    [[nodiscard]] auto TryDecideIs(const ExpressionIndex operand, const EntityType& entityType) const
        -> std::optional<bool>
    {
        if (CanError(operand)) { return std::nullopt; }
        const EntityLub* const lub = AsEntityLub(TypeOf(operand));
        if (lub == nullptr) { return std::nullopt; }
        if (!lub->Contains(entityType)) { return false; }
        if (const EntityType* const single = lub->Single(); single != nullptr && *single == entityType) { return true; }
        return std::nullopt;
    }

    /// Rust's `try_decide_binary_residual`: false or nothing.
    [[nodiscard]] auto TryDecideBinary(const BinaryOperator operation, const ExpressionIndex left,
                                       const ExpressionIndex right) const -> std::optional<bool>
    {
        if (CanError(left) || CanError(right)) { return std::nullopt; }
        const Type& leftType = TypeOf(left);
        const Type& rightType = TypeOf(right);
        bool mustBeFalse = false;
        switch (operation)
        {
        case BinaryOperator::In:
        {
            const EntityLub* const leftLub = AsEntityLub(leftType);
            const EntityLub* const rightLub = AsSetOrEntityLub(rightType);
            if (leftLub == nullptr || rightLub == nullptr) { return std::nullopt; }
            mustBeFalse = !schema_.AnyDescendantOf(*leftLub, *rightLub);
            break;
        }
        case BinaryOperator::Equal: mustBeFalse = AreTypesDisjoint(leftType, rightType); break;
        case BinaryOperator::HasTag:
            if (!std::holds_alternative<types::String>(rightType.kind)) { return std::nullopt; }
            mustBeFalse = HasDeclaredEntityTypes(schema_, leftType) && !MayHaveTags(schema_, leftType);
            break;
        default: return std::nullopt;
        }
        if (mustBeFalse) { return false; }
        return std::nullopt;
    }

    /// Rust's `try_decide_has_residual`: false or nothing.
    [[nodiscard]] auto TryDecideHas(const ExpressionIndex object, const std::string_view attribute) const
        -> std::optional<bool>
    {
        if (CanError(object)) { return std::nullopt; }
        const Type& type = TypeOf(object);
        if (!std::holds_alternative<types::Record>(type.kind) && !std::holds_alternative<types::Entity>(type.kind) &&
            !std::holds_alternative<types::AnyEntity>(type.kind))
        {
            return std::nullopt;
        }
        if (HasDeclaredEntityTypes(schema_, type) && !MayHaveAttribute(schema_, type, attribute)) { return false; }
        return std::nullopt;
    }

    const Residual& input_;
    const ExpressionArena& source_;
    const PartialRequest& request_;
    const PartialEntities& entities_;
    const ValidatorSchema& schema_;
    std::vector<std::optional<ExpressionIndex>> interpreted_;
    ExpressionArena arena_{};
    ExpressionBuilder builder_{arena_};
    std::vector<Type> types_{};
    // Sets, records and extension values built from concrete operands, before Concrete copies them
    // into the arena.
    ValueArena scratch_{};
    std::uint32_t depth_ = 0;
};

} // namespace

auto Interpret(const Residual& residual, const PartialRequest& request, const PartialEntities& entities,
               const Schema& schema) -> Residual
{
    return PartialInterpreter{residual, request, entities, SchemaAccess::Validator(schema)}.Run();
}

} // namespace cedar::detail
