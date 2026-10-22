#include "validation/Typechecker.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "model/FuzzyMatch.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cedar::detail
{

namespace
{

// ---- Answers ----

// Rust's `TypecheckAnswer` (`typecheck/typecheck_answer.rs`): a failed node still carries the
// type it would have (its recovery type), so the nodes above it keep checking and reporting.
enum class AnswerState : std::uint8_t
{
    Success,
    Fail,
    RecursionLimit,
};

struct Answer
{
    AnswerState state = AnswerState::Success;
    std::optional<Type> type{};
    // Only a successful answer carries capabilities.
    CapabilitySet capabilities{};
};

[[nodiscard]] auto Success(std::optional<Type> type, CapabilitySet capabilities = {}) -> Answer
{
    return Answer{AnswerState::Success, std::move(type), std::move(capabilities)};
}

[[nodiscard]] auto Fail(std::optional<Type> type) -> Answer { return Answer{AnswerState::Fail, std::move(type), {}}; }

[[nodiscard]] auto IntoFail(Answer answer) -> Answer
{
    if (answer.state == AnswerState::Success)
    {
        answer.state = AnswerState::Fail;
        answer.capabilities = {};
    }
    return answer;
}

// Rust's `then_typecheck`: `next` sees a failed answer with no capabilities, and its result fails
// too; the recursion limit passes through untouched.
template <typename Next>
[[nodiscard]] auto Then(Answer answer, Next&& next) -> Answer
{
    if (answer.state == AnswerState::RecursionLimit) { return answer; }
    if (answer.state == AnswerState::Fail) { return IntoFail(std::forward<Next>(next)(std::move(answer))); }
    return std::forward<Next>(next)(std::move(answer));
}

// Rust's `sequence_all_then_typecheck`.
template <typename Next>
[[nodiscard]] auto SequenceAll(std::vector<Answer> answers, Next&& next) -> Answer
{
    bool anyFailed = false;
    bool recursionLimit = false;
    std::size_t kept = 0;
    for (Answer& answer : answers)
    {
        anyFailed = anyFailed || answer.state != AnswerState::Success;
        if (answer.state == AnswerState::RecursionLimit)
        {
            recursionLimit = true;
            break;
        }
        if (answer.state == AnswerState::Fail) { answer.capabilities = {}; }
        ++kept;
    }
    answers.resize(kept);
    Answer result = std::forward<Next>(next)(std::move(answers));
    if (recursionLimit) { return Answer{AnswerState::RecursionLimit}; }
    return anyFailed ? IntoFail(std::move(result)) : result;
}

// ---- Types ----

[[nodiscard]] auto BooleanKind(const std::optional<Type>& type) noexcept -> std::optional<BooleanType>
{
    if (!type) { return std::nullopt; }
    const auto* boolean = std::get_if<types::Boolean>(&type->kind);
    return boolean != nullptr ? std::optional<BooleanType>{boolean->kind} : std::nullopt;
}

[[nodiscard]] auto IsSingleton(const std::optional<Type>& type, const BooleanType kind) noexcept -> bool
{
    return BooleanKind(type) == kind;
}

[[nodiscard]] auto BooleanOf(const BooleanType kind) -> Type { return Type{types::Boolean{kind}}; }

[[nodiscard]] auto IsEntityKind(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::AnyEntity>(type.kind) || std::holds_alternative<types::Entity>(type.kind);
}

[[nodiscard]] auto IsSetKind(const Type& type) noexcept -> bool { return std::holds_alternative<types::Set>(type.kind); }

[[nodiscard]] auto IsRecordKind(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::Record>(type.kind);
}

[[nodiscard]] auto IsStringKind(const Type& type) noexcept -> bool
{
    return std::holds_alternative<types::String>(type.kind);
}

// The validator type of an extension table kind.
[[nodiscard]] auto TypeOfKind(const ValueKind kind) -> Type
{
    switch (kind)
    {
    case ValueKind::Bool: return AnyBooleanType();
    case ValueKind::Long: return LongType();
    case ValueKind::String: return StringType();
    default: return ExtensionType(kind);
    }
}

// The extension types `<` and `<=` apply to (Rust's datetime extension declares both of its types
// with operator overloading; decimal and ipaddr declare none).
[[nodiscard]] constexpr auto HasOperatorOverloading(const ValueKind kind) noexcept -> bool
{
    return kind == ValueKind::Datetime || kind == ValueKind::Duration;
}

[[nodiscard]] auto IsValidComparisonType(const Type& type) noexcept -> bool
{
    if (std::holds_alternative<types::Long>(type.kind)) { return true; }
    const auto* extension = std::get_if<types::Extension>(&type.kind);
    return extension != nullptr && HasOperatorOverloading(extension->kind);
}

[[nodiscard]] auto ComparisonTypes() -> std::vector<Type>
{
    return {ExtensionType(ValueKind::Datetime), ExtensionType(ValueKind::Duration), LongType()};
}

// ---- Messages ----

// Rust's `UnexpectedTypeHelp`.
enum class UnexpectedTypeHelp : std::uint8_t
{
    TryUsingLike,
    TryUsingContains,
    TryUsingSingleContains,
    TryUsingHas,
    TryUsingIs,
    TryUsingIn,
    TryUsingEqualsEmptyString,
    TypeTestNotSupported,
    ConcatenationNotSupported,
    SetOperationsNotSupported,
};

[[nodiscard]] auto HelpText(const UnexpectedTypeHelp help) -> std::string_view
{
    switch (help)
    {
    case UnexpectedTypeHelp::TryUsingLike: return "try using `like` to examine the contents of a string";
    case UnexpectedTypeHelp::TryUsingContains:
        return "try using `contains`, `containsAny`, or `containsAll` to examine the contents of a set";
    case UnexpectedTypeHelp::TryUsingSingleContains: return "try using `contains` to test if a single element is in a set";
    case UnexpectedTypeHelp::TryUsingHas: return "try using `has` to test for an attribute";
    case UnexpectedTypeHelp::TryUsingIs: return "try using `is` to test for an entity type";
    case UnexpectedTypeHelp::TryUsingIn: return "try using `in` for entity hierarchy membership";
    case UnexpectedTypeHelp::TryUsingEqualsEmptyString: return "try using `== \"\"` to test if a string is empty";
    case UnexpectedTypeHelp::TypeTestNotSupported: return "Cedar only supports run time type tests for entities";
    case UnexpectedTypeHelp::ConcatenationNotSupported: return "Cedar does not support string concatenation";
    case UnexpectedTypeHelp::SetOperationsNotSupported:
        return "Cedar does not support computing the union, intersection, or difference of sets";
    }
    return {};
}

// Where incompatible types met (Rust's `LubContext`).
enum class LubContext : std::uint8_t
{
    Set,
    Conditional,
    Equality,
    Contains,
    ContainsAnyAll,
    GetTag,
};

[[nodiscard]] auto LubContextText(const LubContext context) -> std::string_view
{
    switch (context)
    {
    case LubContext::Set: return "elements of a set";
    case LubContext::Conditional: return "both branches of a conditional";
    case LubContext::Equality: return "both operands to a `==` expression";
    case LubContext::Contains: return "elements of the first operand and the second operand to a `contains` expression";
    case LubContext::ContainsAnyAll: return "elements of both set operands to a `containsAll` or `containsAny` expression";
    case LubContext::GetTag: return "tag types for a `.getTag()` operation";
    }
    return {};
}

[[nodiscard]] auto JoinTypes(const std::span<const Type> types, const std::string_view conjunction) -> std::string
{
    std::string text;
    for (std::size_t position = 0; position < types.size(); ++position)
    {
        if (position > 0) { text += position + 1 == types.size() ? std::format(" {} ", conjunction) : ", "; }
        AppendTypeText(text, types[position]);
    }
    return text;
}

[[nodiscard]] auto NoHelp(const Type& /*actual*/) -> std::optional<UnexpectedTypeHelp> { return std::nullopt; }

[[nodiscard]] auto HasOperandHelp(const Type& actual) -> std::optional<UnexpectedTypeHelp>
{
    if (IsSetKind(actual)) { return UnexpectedTypeHelp::TryUsingContains; }
    if (IsStringKind(actual)) { return UnexpectedTypeHelp::TryUsingLike; }
    return std::nullopt;
}

[[nodiscard]] auto ContainsOperandHelp(const Type& actual) -> std::optional<UnexpectedTypeHelp>
{
    if (IsEntityKind(actual)) { return UnexpectedTypeHelp::TryUsingIn; }
    if (IsRecordKind(actual)) { return UnexpectedTypeHelp::TryUsingHas; }
    if (IsStringKind(actual)) { return UnexpectedTypeHelp::TryUsingLike; }
    return std::nullopt;
}

// ---- The typechecker for one environment ----

class EnvironmentTypechecker final
{
public:
    EnvironmentTypechecker(const ValidatorSchema& schema, const ValidationMode mode, const ExpressionShapes& shapes,
                           const SharedSourceText& source, const TypecheckEnvironment& environment,
                           std::vector<Diagnostic>& errors, NodeTypes* nodeTypes)
        : schema_{schema}, mode_{mode}, shapes_{shapes}, arena_{shapes.Arena()}, source_{source},
          environment_{environment}, errors_{errors}, nodeTypes_{nodeTypes}
    {
    }

    template <typename Help>
    [[nodiscard]] auto ExpectOneOf(const CapabilitySet& prior, ExpressionIndex index, std::span<const Type> expected,
                                   Help&& help) -> Answer;

    template <typename Help>
    [[nodiscard]] auto Expect(const CapabilitySet& prior, const ExpressionIndex index, const Type& expected, Help&& help)
        -> Answer
    {
        return ExpectOneOf(prior, index, std::span<const Type>{&expected, 1}, std::forward<Help>(help));
    }

    [[nodiscard]] auto Typecheck(const CapabilitySet& prior, ExpressionIndex index) -> Answer;

private:
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Literal& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Variable& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Slot& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::If& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::And& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Or& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Unary& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::ExtensionCall& node)
        -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::GetAttribute& node)
        -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::HasAttribute& node)
        -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Like& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::Is& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::SetLiteral& node) -> Answer;
    [[nodiscard]] auto Node(const CapabilitySet& prior, ExpressionIndex index, const node::RecordLiteral& node)
        -> Answer;

    [[nodiscard]] auto Node(const CapabilitySet& /*prior*/, ExpressionIndex /*index*/, const node::Folded& /*node*/)
        -> Answer
    {
        // Typecheck resolves a folded node to its written subtree first.
        return Fail(std::nullopt);
    }

    [[nodiscard]] auto HasPath(const CapabilitySet& prior, ExpressionIndex index, const node::HasAttribute& node)
        -> Answer;
    [[nodiscard]] auto Equality(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;
    [[nodiscard]] auto Comparison(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node)
        -> Answer;
    [[nodiscard]] auto Arithmetic(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node)
        -> Answer;
    [[nodiscard]] auto In(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;
    [[nodiscard]] auto Contains(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;
    [[nodiscard]] auto ContainsAllOrAny(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node)
        -> Answer;
    [[nodiscard]] auto HasTag(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;
    [[nodiscard]] auto GetTag(const CapabilitySet& prior, ExpressionIndex index, const node::Binary& node) -> Answer;

    // Rust's `enforce_strict_equality`.
    [[nodiscard]] auto EnforceStrictEquality(ExpressionIndex index, const Type& resultType, const Type* left,
                                             const Type* right, LubContext context) -> Answer;
    // Rust's `least_upper_bound_or_error`.
    [[nodiscard]] auto LeastUpperBoundOrError(ExpressionIndex index, std::span<const std::optional<Type>> types,
                                              LubContext context) -> std::optional<Type>;
    // Rust's `type_of_equality`.
    [[nodiscard]] auto TypeOfEquality(ExpressionIndex left, const std::optional<Type>& leftType, ExpressionIndex right,
                                      const std::optional<Type>& rightType) -> Type;
    // The literal `index` is, with the action variable read as the environment's action (Rust's
    // `replace_action_var_with_euid`); null for any other node.
    [[nodiscard]] auto LiteralOrAction(ExpressionIndex index) const -> std::optional<Value>;
    [[nodiscard]] auto EntityLiteralsOrActions(ExpressionIndex index) const -> std::optional<std::vector<EntityUid>>;
    [[nodiscard]] auto AnyDescendantOf(const EntityLub& left, const EntityLub& right) const -> bool;
    [[nodiscard]] auto TagTypes(const Type& entity) const -> std::vector<Type>;

    // Rust's `type_of_has_attr_for_ext` and `type_of_get_attr_for_ext`: one step along `object
    // has a.b.c`, at `object.path...`.
    struct HasStep
    {
        BooleanType type;
        CapabilitySet capabilities;
    };
    [[nodiscard]] auto HasStepOf(const CapabilitySet& prior, const std::optional<Type>& objectType,
                                 ExpressionIndex object, std::span<const std::string_view> path,
                                 std::string_view attribute, ExpressionIndex at) -> std::optional<HasStep>;
    [[nodiscard]] auto GetStepOf(const CapabilitySet& prior, const std::optional<Type>& objectType,
                                 ExpressionIndex object, std::span<const std::string_view> path,
                                 std::string_view attribute, ExpressionIndex at) -> std::optional<Type>;

    [[nodiscard]] auto AttributeCapability(const ExpressionIndex object, const std::span<const std::string_view> path,
                                           const std::string_view attribute) const -> Capability
    {
        return Capability::ForAttribute(shapes_, object, path, attribute);
    }

    void Report(DiagnosticCode code, ExpressionIndex at, std::string message,
                std::optional<std::string> help = std::nullopt);
    void ReportUnexpectedType(ExpressionIndex at, std::span<const Type> expected, const Type& actual,
                              std::optional<UnexpectedTypeHelp> help);
    void ReportIncompatibleTypes(ExpressionIndex at, std::span<const Type> types, LubHelp help, LubContext context);
    void ReportUnsafeAttributeAccess(ExpressionIndex at, const Type& object, std::string_view attribute,
                                     bool mayExist);
    void ReportUnsafeOptionalAttributeAccess(ExpressionIndex at, const Type& object, std::string_view attribute);

    [[nodiscard]] auto Union(const CapabilitySet& left, const CapabilitySet& right) const -> CapabilitySet
    {
        return left.Union(right, shapes_);
    }

    [[nodiscard]] auto Intersect(const CapabilitySet& left, const CapabilitySet& right) const -> CapabilitySet
    {
        return left.Intersect(right, shapes_);
    }

    [[nodiscard]] auto IsStrict() const noexcept -> bool { return mode_ == ValidationMode::Strict; }

    const ValidatorSchema& schema_;
    ValidationMode mode_;
    const ExpressionShapes& shapes_;
    const ExpressionArena& arena_;
    const SharedSourceText& source_;
    const TypecheckEnvironment& environment_;
    std::vector<Diagnostic>& errors_;
    NodeTypes* nodeTypes_;
    std::uint32_t depth_ = 0;
};

// ---- Reporting ----

void EnvironmentTypechecker::Report(const DiagnosticCode code, const ExpressionIndex at, std::string message,
                                    std::optional<std::string> help)
{
    errors_.push_back(Diagnostic{.code = code,
                                 .severity = Severity::Error,
                                 .message = std::move(message),
                                 .help = std::move(help),
                                 .primary = arena_.Span(at),
                                 .labels = {},
                                 .source = source_});
}

void EnvironmentTypechecker::ReportUnexpectedType(const ExpressionIndex at, const std::span<const Type> expected,
                                                  const Type& actual, const std::optional<UnexpectedTypeHelp> help)
{
    Report(DiagnosticCode::UnexpectedType, at,
           std::format("unexpected type: expected {} but saw {}",
                       expected.size() == 1 ? ToString(expected.front()) : "one of " + JoinTypes(expected, "or"),
                       ToString(actual)),
           help ? std::optional<std::string>{std::string{HelpText(*help)}} : std::nullopt);
}

void EnvironmentTypechecker::ReportIncompatibleTypes(const ExpressionIndex at, const std::span<const Type> types,
                                                     const LubHelp help, const LubContext context)
{
    Report(DiagnosticCode::IncompatibleTypes, at, std::format("the types {} are not compatible", JoinTypes(types, "and")),
           std::format("{} must have compatible types. {}", LubContextText(context), LubHelpText(help)));
}

void EnvironmentTypechecker::ReportUnsafeAttributeAccess(const ExpressionIndex at, const Type& object,
                                                         const std::string_view attribute, const bool mayExist)
{
    // Rust's `UnsafeAttributeAccess` help: the nearest attribute the type does declare, then the note
    // that the schema may not list every attribute.
    const Attributes attributes = AllAttributes(schema_, object);
    std::vector<std::string_view> declared;
    for (const Attribute& each : attributes) { declared.push_back(each.name); }
    const std::optional<std::string> suggestion = FuzzySearch(attribute, declared);
    std::optional<std::string> help;
    if (suggestion && !mayExist) { help = std::format("did you mean `{}`?", *suggestion); }
    else if (suggestion)
    {
        help = std::format("did you mean `{}`? (there may also be additional attributes that the validator is "
                           "not able to reason about)",
                           *suggestion);
    }
    else if (mayExist) { help = "there may be additional attributes that the validator is not able to reason about"; }
    Report(DiagnosticCode::UnsafeAttributeAccess, at,
           std::format("attribute `{}` not found on a value of type {}", attribute, ToString(object)), std::move(help));
}

void EnvironmentTypechecker::ReportUnsafeOptionalAttributeAccess(const ExpressionIndex at, const Type& object,
                                                                 const std::string_view attribute)
{
    Report(DiagnosticCode::UnsafeOptionalAttributeAccess, at,
           std::format("unable to guarantee safety of access to optional attribute `{}` on a value of type {}",
                       attribute, ToString(object)),
           std::format("try testing for the attribute's presence with `e has {} && ..`", attribute));
}

// ---- Expectations ----

template <typename Help>
auto EnvironmentTypechecker::ExpectOneOf(const CapabilitySet& prior, const ExpressionIndex index,
                                         const std::span<const Type> expected, Help&& help) -> Answer
{
    return Then(Typecheck(prior, index),
                [&](Answer actual)
                {
                    if (!actual.type) { return Fail(std::nullopt); }
                    // Subtyping is permissive here even in strict mode (Rust's `expect_one_of_types`).
                    const bool matches = std::ranges::any_of(expected, [&](const Type& candidate)
                                                             { return IsSubtype(*actual.type, candidate,
                                                                                ValidationMode::Permissive); });
                    if (!matches)
                    {
                        ReportUnexpectedType(index, expected, *actual.type, help(*actual.type));
                        // Rust clears the operand's annotation too.
                        if (nodeTypes_ != nullptr) { nodeTypes_->Record(shapes_.Resolve(index), std::nullopt); }
                        return Fail(std::nullopt);
                    }
                    return actual;
                });
}

// ---- Dispatch ----

auto EnvironmentTypechecker::Typecheck(const CapabilitySet& prior, ExpressionIndex index) -> Answer
{
    if (depth_ >= MaximumTypecheckDepth) { return Answer{AnswerState::RecursionLimit}; }
    ++depth_;
    index = shapes_.Resolve(index);
    Answer answer = std::visit([&](const auto& node) { return Node(prior, index, node); }, arena_.Node(index));
    --depth_;
    if (nodeTypes_ != nullptr) { nodeTypes_->Record(index, answer.type); }
    return answer;
}

auto EnvironmentTypechecker::Node(const CapabilitySet& /*prior*/, const ExpressionIndex /*index*/,
                                  const node::Literal& node) -> Answer
{
    const Value& value = node.value;
    switch (value.Kind())
    {
    case ValueKind::Bool: return Success(SingletonBooleanType(value.AsBool()));
    case ValueKind::Long: return Success(LongType());
    case ValueKind::String: return Success(StringType());
    case ValueKind::Entity:
    {
        // An entity literal of an undeclared type or action: the scope checks report it.
        std::optional<Type> type = EntityUidLiteralType(value.AsEntity(), schema_);
        return type ? Success(std::move(type)) : Fail(std::nullopt);
    }
    default: return Fail(std::nullopt);
    }
}

auto EnvironmentTypechecker::Node(const CapabilitySet& /*prior*/, const ExpressionIndex /*index*/,
                                  const node::Variable& node) -> Answer
{
    switch (node.variable)
    {
    case Variable::Principal: return Success(NamedEntityType(environment_.principal));
    case Variable::Resource: return Success(NamedEntityType(environment_.resource));
    case Variable::Action:
    {
        std::optional<Type> type = EntityUidLiteralType(environment_.action->name, schema_);
        return type ? Success(std::move(type)) : Fail(std::nullopt);
    }
    case Variable::Context:
        return Success(environment_.action->context ? *environment_.action->context : EmptyRecord());
    }
    return Fail(std::nullopt);
}

auto EnvironmentTypechecker::Node(const CapabilitySet& /*prior*/, const ExpressionIndex /*index*/,
                                  const node::Slot& node) -> Answer
{
    const std::optional<EntityType>& linked =
        node.slot == SlotId::Principal ? environment_.principalSlot : environment_.resourceSlot;
    return Success(linked ? NamedEntityType(*linked) : AnyEntityType());
}

// ---- Boolean structure ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index, const node::If& node)
    -> Answer
{
    const Answer test = Expect(prior, node.condition, AnyBooleanType(), NoHelp);
    return Then(test,
                [&](Answer typedTest)
                {
                    const CapabilitySet& testCapabilities = typedTest.capabilities;
                    if (IsSingleton(typedTest.type, BooleanType::True))
                    {
                        return Then(Typecheck(Union(prior, testCapabilities), node.thenBranch),
                                    [&](Answer typedThen)
                                    {
                                        return Success(std::move(typedThen.type),
                                                       Union(typedThen.capabilities, testCapabilities));
                                    });
                    }
                    if (IsSingleton(typedTest.type, BooleanType::False))
                    {
                        return Then(Typecheck(prior, node.elseBranch),
                                    [&](Answer typedElse)
                                    { return Success(std::move(typedElse.type), std::move(typedElse.capabilities)); });
                    }
                    Answer thenAnswer = Typecheck(Union(prior, testCapabilities), node.thenBranch);
                    if (thenAnswer.state == AnswerState::Success)
                    {
                        thenAnswer.capabilities = Union(thenAnswer.capabilities, testCapabilities);
                    }
                    Answer elseAnswer = Typecheck(prior, node.elseBranch);
                    return Then(std::move(thenAnswer),
                                [&](Answer typedThen)
                                {
                                    return Then(std::move(elseAnswer),
                                                [&](Answer typedElse)
                                                {
                                                    const std::array branches{typedThen.type, typedElse.type};
                                                    std::optional<Type> lub = LeastUpperBoundOrError(
                                                        index, branches, LubContext::Conditional);
                                                    if (!lub) { return Fail(std::nullopt); }
                                                    return Success(std::move(lub),
                                                                   Intersect(typedElse.capabilities,
                                                                             typedThen.capabilities));
                                                });
                                });
                });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/, const node::And& node)
    -> Answer
{
    return Then(Expect(prior, node.left, AnyBooleanType(), NoHelp),
                [&](Answer left)
                {
                    if (IsSingleton(left.type, BooleanType::False)) { return Success(std::move(left.type)); }
                    return Then(Expect(Union(prior, left.capabilities), node.right, AnyBooleanType(), NoHelp),
                                [&](Answer right)
                                {
                                    if (!left.type || !right.type) { return Fail(AnyBooleanType()); }
                                    if (IsSingleton(right.type, BooleanType::False))
                                    {
                                        return Success(SingletonBooleanType(false));
                                    }
                                    if (IsSingleton(right.type, BooleanType::True))
                                    {
                                        return Success(std::move(left.type),
                                                       Union(left.capabilities, right.capabilities));
                                    }
                                    if (IsSingleton(left.type, BooleanType::True))
                                    {
                                        // Rust keeps only the right operand's capabilities here.
                                        return Success(std::move(right.type), std::move(right.capabilities));
                                    }
                                    return Success(AnyBooleanType(), Union(left.capabilities, right.capabilities));
                                });
                });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/, const node::Or& node)
    -> Answer
{
    return Then(Expect(prior, node.left, AnyBooleanType(), NoHelp),
                [&](Answer left)
                {
                    if (IsSingleton(left.type, BooleanType::True)) { return left; }
                    // The right operand is typed without the left's capabilities: it runs when the
                    // left is false.
                    return Then(Expect(prior, node.right, AnyBooleanType(), NoHelp),
                                [&](Answer right)
                                {
                                    if (!left.type || !right.type) { return Fail(AnyBooleanType()); }
                                    if (IsSingleton(right.type, BooleanType::True))
                                    {
                                        return Success(SingletonBooleanType(true), std::move(right.capabilities));
                                    }
                                    if (IsSingleton(right.type, BooleanType::False))
                                    {
                                        return Success(std::move(left.type), std::move(left.capabilities));
                                    }
                                    if (IsSingleton(left.type, BooleanType::False))
                                    {
                                        return Success(std::move(right.type), std::move(right.capabilities));
                                    }
                                    return Success(AnyBooleanType(), Intersect(right.capabilities, left.capabilities));
                                });
                });
}

// ---- Unary operators ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/,
                                  const node::Unary& node) -> Answer
{
    switch (node.operation)
    {
    case UnaryOperator::Not:
        return Then(Expect(prior, node.operand, AnyBooleanType(), NoHelp),
                    [](Answer operand)
                    {
                        if (!operand.type) { return Fail(AnyBooleanType()); }
                        if (IsSingleton(operand.type, BooleanType::True)) { return Success(SingletonBooleanType(false)); }
                        if (IsSingleton(operand.type, BooleanType::False)) { return Success(SingletonBooleanType(true)); }
                        return Success(AnyBooleanType());
                    });
    case UnaryOperator::Negate:
        return Then(Expect(prior, node.operand, LongType(), NoHelp), [](Answer) { return Success(LongType()); });
    case UnaryOperator::IsEmpty:
        return Then(Expect(prior, node.operand, AnySetType(),
                           [](const Type& actual) -> std::optional<UnexpectedTypeHelp>
                           {
                               if (IsStringKind(actual)) { return UnexpectedTypeHelp::TryUsingEqualsEmptyString; }
                               return std::nullopt;
                           }),
                    [](Answer) { return Success(AnyBooleanType()); });
    }
    return Fail(std::nullopt);
}

// ---- Binary operators ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index, const node::Binary& node)
    -> Answer
{
    switch (node.operation)
    {
    case BinaryOperator::Equal: return Equality(prior, index, node);
    case BinaryOperator::Less:
    case BinaryOperator::LessEqual: return Comparison(prior, index, node);
    case BinaryOperator::Add:
    case BinaryOperator::Subtract:
    case BinaryOperator::Multiply: return Arithmetic(prior, index, node);
    case BinaryOperator::In: return In(prior, index, node);
    case BinaryOperator::Contains: return Contains(prior, index, node);
    case BinaryOperator::ContainsAll:
    case BinaryOperator::ContainsAny: return ContainsAllOrAny(prior, index, node);
    case BinaryOperator::HasTag: return HasTag(prior, index, node);
    case BinaryOperator::GetTag: return GetTag(prior, index, node);
    }
    return Fail(std::nullopt);
}

auto EnvironmentTypechecker::LiteralOrAction(const ExpressionIndex index) const -> std::optional<Value>
{
    const ExpressionNode& node = arena_.Node(shapes_.Resolve(index));
    if (const auto* literal = std::get_if<node::Literal>(&node)) { return literal->value; }
    if (const auto* variable = std::get_if<node::Variable>(&node); variable != nullptr &&
                                                                     variable->variable == Variable::Action)
    {
        return Value::FromEntity(environment_.action->name);
    }
    return std::nullopt;
}

auto EnvironmentTypechecker::TypeOfEquality(const ExpressionIndex left, const std::optional<Type>& leftType,
                                            const ExpressionIndex right, const std::optional<Type>& rightType) -> Type
{
    if (leftType && rightType && AreTypesDisjoint(*leftType, *rightType)) { return SingletonBooleanType(false); }
    const std::optional<Value> leftLiteral = LiteralOrAction(left);
    const std::optional<Value> rightLiteral = LiteralOrAction(right);
    if (leftLiteral && rightLiteral) { return SingletonBooleanType(*leftLiteral == *rightLiteral); }
    return AnyBooleanType();
}

auto EnvironmentTypechecker::EnforceStrictEquality(const ExpressionIndex index, const Type& resultType,
                                                   const Type* left, const Type* right, const LubContext context)
    -> Answer
{
    if (IsSingleton(resultType, BooleanType::True) || IsSingleton(resultType, BooleanType::False))
    {
        return Success(resultType);
    }
    if (left != nullptr && right != nullptr)
    {
        if (auto lub = LeastUpperBound(*left, *right, mode_); !lub)
        {
            const std::array types{*left, *right};
            ReportIncompatibleTypes(index, types, lub.error(), context);
            return Fail(resultType);
        }
    }
    return Success(resultType);
}

auto EnvironmentTypechecker::LeastUpperBoundOrError(const ExpressionIndex index,
                                                    const std::span<const std::optional<Type>> types,
                                                    const LubContext context) -> std::optional<Type>
{
    std::vector<Type> known;
    known.reserve(types.size());
    for (const std::optional<Type>& type : types)
    {
        if (!type) { return std::nullopt; }
        known.push_back(*type);
    }
    auto lub = ReduceToLeastUpperBound(known, mode_);
    if (!lub)
    {
        ReportIncompatibleTypes(index, known, lub.error(), context);
        return std::nullopt;
    }
    return std::move(*lub);
}

auto EnvironmentTypechecker::Equality(const CapabilitySet& prior, const ExpressionIndex index, const node::Binary& node)
    -> Answer
{
    Answer leftAnswer = Typecheck(prior, node.left);
    Answer rightAnswer = Typecheck(prior, node.right);
    return Then(std::move(leftAnswer),
                [&](Answer left)
                {
                    return Then(std::move(rightAnswer),
                                [&](Answer right)
                                {
                                    const Type type = TypeOfEquality(node.left, left.type, node.right, right.type);
                                    if (!IsStrict()) { return Success(type); }
                                    return EnforceStrictEquality(index, type, left.type ? &*left.type : nullptr,
                                                                 right.type ? &*right.type : nullptr,
                                                                 LubContext::Equality);
                                });
                });
}

auto EnvironmentTypechecker::Comparison(const CapabilitySet& prior, const ExpressionIndex /*index*/,
                                        const node::Binary& node) -> Answer
{
    return Then(Typecheck(prior, node.left),
                [&](Answer left)
                {
                    return Then(Typecheck(prior, node.right),
                                [&](Answer right)
                                {
                                    const Answer failed = Fail(AnyBooleanType());
                                    if (!left.type || !right.type) { return failed; }
                                    const Type& leftType = *left.type;
                                    const Type& rightType = *right.type;
                                    const bool leftNever = std::holds_alternative<types::Never>(leftType.kind);
                                    const bool rightNever = std::holds_alternative<types::Never>(rightType.kind);
                                    const bool leftLong = std::holds_alternative<types::Long>(leftType.kind);
                                    const bool rightLong = std::holds_alternative<types::Long>(rightType.kind);
                                    if (leftNever && rightNever) { return failed; }
                                    if (leftNever || rightNever)
                                    {
                                        const Type& other = leftNever ? rightType : leftType;
                                        if (IsValidComparisonType(other)) { return Success(AnyBooleanType()); }
                                        ReportUnexpectedType(leftNever ? node.right : node.left, ComparisonTypes(),
                                                             other, std::nullopt);
                                        return failed;
                                    }
                                    if (leftType == rightType && IsValidComparisonType(leftType))
                                    {
                                        return Success(AnyBooleanType());
                                    }
                                    if (leftLong)
                                    {
                                        ReportUnexpectedType(node.right, std::array{LongType()}, rightType,
                                                             std::nullopt);
                                        return failed;
                                    }
                                    if (rightLong)
                                    {
                                        ReportUnexpectedType(node.left, std::array{LongType()}, leftType,
                                                             std::nullopt);
                                        return failed;
                                    }
                                    if (IsValidComparisonType(leftType))
                                    {
                                        ReportUnexpectedType(node.right, std::array{leftType}, rightType,
                                                             std::nullopt);
                                        return failed;
                                    }
                                    if (IsValidComparisonType(rightType))
                                    {
                                        ReportUnexpectedType(node.left, std::array{rightType}, leftType,
                                                             std::nullopt);
                                        return failed;
                                    }
                                    ReportUnexpectedType(node.left, ComparisonTypes(), leftType, std::nullopt);
                                    ReportUnexpectedType(node.right, ComparisonTypes(), rightType, std::nullopt);
                                    return failed;
                                });
                });
}

auto EnvironmentTypechecker::Arithmetic(const CapabilitySet& prior, const ExpressionIndex /*index*/,
                                        const node::Binary& node) -> Answer
{
    const bool isAdd = node.operation == BinaryOperator::Add;
    const auto help = [isAdd](const Type& actual) -> std::optional<UnexpectedTypeHelp>
    {
        if (isAdd && IsStringKind(actual)) { return UnexpectedTypeHelp::ConcatenationNotSupported; }
        if (IsSetKind(actual)) { return UnexpectedTypeHelp::SetOperationsNotSupported; }
        return std::nullopt;
    };
    return Then(Expect(prior, node.left, LongType(), help),
                [&](Answer)
                { return Then(Expect(prior, node.right, LongType(), help), [](Answer) { return Success(LongType()); }); });
}

auto EnvironmentTypechecker::Contains(const CapabilitySet& prior, const ExpressionIndex index, const node::Binary& node)
    -> Answer
{
    return Then(Expect(prior, node.left, AnySetType(), ContainsOperandHelp),
                [&](Answer set)
                {
                    return Then(Typecheck(prior, node.right),
                                [&](Answer element)
                                {
                                    if (!IsStrict()) { return Success(AnyBooleanType()); }
                                    const Type* elementType = nullptr;
                                    if (set.type)
                                    {
                                        if (const auto* asSet = std::get_if<types::Set>(&set.type->kind);
                                            asSet != nullptr && asSet->element != nullptr)
                                        {
                                            elementType = asSet->element.get();
                                        }
                                    }
                                    return EnforceStrictEquality(index, AnyBooleanType(), elementType,
                                                                 element.type ? &*element.type : nullptr,
                                                                 LubContext::Contains);
                                });
                });
}

auto EnvironmentTypechecker::ContainsAllOrAny(const CapabilitySet& prior, const ExpressionIndex index,
                                              const node::Binary& node) -> Answer
{
    return Then(Expect(prior, node.left, AnySetType(), ContainsOperandHelp),
                [&](Answer left)
                {
                    return Then(Expect(prior, node.right, AnySetType(),
                                       [](const Type&) -> std::optional<UnexpectedTypeHelp>
                                       { return UnexpectedTypeHelp::TryUsingSingleContains; }),
                                [&](Answer right)
                                {
                                    if (!IsStrict()) { return Success(AnyBooleanType()); }
                                    return EnforceStrictEquality(index, AnyBooleanType(),
                                                                 left.type ? &*left.type : nullptr,
                                                                 right.type ? &*right.type : nullptr,
                                                                 LubContext::ContainsAnyAll);
                                });
                });
}

auto EnvironmentTypechecker::TagTypes(const Type& entity) const -> std::vector<Type>
{
    std::vector<Type> found;
    const auto add = [&found](const ValidatorEntityType& declared)
    {
        if (declared.tags && std::ranges::find(found, *declared.tags) == found.end()) { found.push_back(*declared.tags); }
    };
    if (const EntityLub* lub = AsEntityLub(entity))
    {
        for (const EntityType& type : lub->Types())
        {
            if (const ValidatorEntityType* declared = schema_.FindEntityType(type)) { add(*declared); }
        }
    }
    else
    {
        for (const ValidatorEntityType& declared : schema_.EntityTypes()) { add(declared); }
    }
    return found;
}

auto EnvironmentTypechecker::HasTag(const CapabilitySet& prior, const ExpressionIndex index, const node::Binary& node)
    -> Answer
{
    return Then(Expect(prior, node.left, AnyEntityType(), NoHelp),
                [&](Answer entity)
                {
                    return Then(Expect(prior, node.right, StringType(), NoHelp),
                                [&](Answer)
                                {
                                    if (!entity.type) { return Fail(std::nullopt); }
                                    if (!IsEntityKind(*entity.type))
                                    {
                                        Report(DiagnosticCode::InternalInvariantViolation, index,
                                               "internal invariant violated: `hasTag` on a value that is not an entity");
                                        return Fail(std::nullopt);
                                    }
                                    const Capability tag = Capability::ForTag(shapes_, node.left, node.right);
                                    Type type = AnyBooleanType();
                                    if (TagTypes(*entity.type).empty()) { type = SingletonBooleanType(false); }
                                    else if (prior.Contains(tag, shapes_)) { type = SingletonBooleanType(true); }
                                    return Success(std::move(type), CapabilitySet::Singleton(tag));
                                });
                });
}

auto EnvironmentTypechecker::GetTag(const CapabilitySet& prior, const ExpressionIndex index, const node::Binary& node)
    -> Answer
{
    return Then(Expect(prior, node.left, AnyEntityType(), NoHelp),
                [&](Answer entity)
                {
                    return Then(Expect(prior, node.right, StringType(), NoHelp),
                                [&](Answer)
                                {
                                    if (!entity.type) { return Fail(std::nullopt); }
                                    if (!IsEntityKind(*entity.type))
                                    {
                                        Report(DiagnosticCode::InternalInvariantViolation, index,
                                               "internal invariant violated: `getTag` on a value that is not an entity");
                                        return Fail(std::nullopt);
                                    }
                                    const EntityLub* lub = AsEntityLub(*entity.type);
                                    if (!prior.Contains(Capability::ForTag(shapes_, node.left, node.right), shapes_))
                                    {
                                        Report(DiagnosticCode::UnsafeTagAccess, index,
                                               std::format("unable to guarantee safety of access to a tag of {}",
                                                           ToString(*entity.type)),
                                               "try testing for the tag's presence with `.hasTag(..) && ..`");
                                        return Fail(std::nullopt);
                                    }
                                    std::vector<Type> tagTypes = TagTypes(*entity.type);
                                    if (tagTypes.empty())
                                    {
                                        const EntityType* single = lub != nullptr ? lub->Single() : nullptr;
                                        Report(DiagnosticCode::NoTagsAllowed, index,
                                               std::format("`.getTag()` is not allowed on entities of {} because no "
                                                           "`tags` were declared on the entity type in the schema",
                                                           single != nullptr ? std::format("type `{}`",
                                                                                           single->GetName().ToString())
                                                                             : std::string{"this type"}));
                                        return Fail(std::nullopt);
                                    }
                                    auto tagType = ReduceToLeastUpperBound(tagTypes, mode_);
                                    if (!tagType)
                                    {
                                        ReportIncompatibleTypes(index, tagTypes, tagType.error(), LubContext::GetTag);
                                        return Fail(std::nullopt);
                                    }
                                    return Success(std::move(*tagType));
                                });
                });
}

// ---- `in` ----

auto EnvironmentTypechecker::EntityLiteralsOrActions(const ExpressionIndex index) const
    -> std::optional<std::vector<EntityUid>>
{
    if (const std::optional<Value> single = LiteralOrAction(index))
    {
        if (single->Kind() != ValueKind::Entity) { return std::nullopt; }
        return std::vector<EntityUid>{single->AsEntity()};
    }
    const auto* set = std::get_if<node::SetLiteral>(&arena_.Node(shapes_.Resolve(index)));
    if (set == nullptr) { return std::nullopt; }
    std::vector<EntityUid> uids;
    for (const ExpressionIndex element : arena_.Elements(set->elements))
    {
        const std::optional<Value> literal = LiteralOrAction(element);
        if (!literal || literal->Kind() != ValueKind::Entity) { return std::nullopt; }
        uids.push_back(literal->AsEntity());
    }
    return uids;
}

auto EnvironmentTypechecker::AnyDescendantOf(const EntityLub& left, const EntityLub& right) const -> bool
{
    // Rust's `ValidatorSchema::any_descendent_of`: an entity type is in another when it is that
    // type or one of its descendants, or when some action of the other type has a descendant
    // action of the first type.
    const auto descendantOf = [this](const EntityType& lower, const EntityType& upper)
    {
        if (lower == upper) { return true; }
        if (const ValidatorEntityType* declared = schema_.FindEntityType(upper);
            declared != nullptr && declared->HasDescendant(lower))
        {
            return true;
        }
        return std::ranges::any_of(schema_.Actions(),
                                   [&](const ValidatorAction& action)
                                   {
                                       return action.name.Type() == upper &&
                                              std::ranges::any_of(action.descendants, [&](const EntityUid& descendant)
                                                                  { return descendant.Type() == lower; });
                                   });
    };
    return std::ranges::any_of(left.Types(),
                               [&](const EntityType& lower)
                               {
                                   return std::ranges::any_of(right.Types(), [&](const EntityType& upper)
                                                              { return descendantOf(lower, upper); });
                               });
}

auto EnvironmentTypechecker::In(const CapabilitySet& prior, const ExpressionIndex /*index*/, const node::Binary& node)
    -> Answer
{
    Answer leftAnswer = Expect(prior, node.left, AnyEntityType(),
                               [](const Type&) -> std::optional<UnexpectedTypeHelp>
                               { return UnexpectedTypeHelp::TryUsingContains; });
    const std::array expectedRight{SetType(MakeShared(AnyEntityType())), AnyEntityType()};
    Answer rightAnswer = ExpectOneOf(prior, node.right, expectedRight, HasOperandHelp);
    const bool bothTypechecked =
        leftAnswer.state == AnswerState::Success && rightAnswer.state == AnswerState::Success;
    return Then(std::move(leftAnswer),
                [&](Answer left)
                {
                    return Then(std::move(rightAnswer),
                                [&](Answer right)
                                {
                                    if (!bothTypechecked) { return Fail(AnyBooleanType()); }
                                    const std::optional<Value> leftLiteral = LiteralOrAction(node.left);
                                    const std::optional<std::vector<EntityUid>> rightLiterals =
                                        EntityLiteralsOrActions(node.right);
                                    if (leftLiteral && leftLiteral->Kind() == ValueKind::Entity && rightLiterals &&
                                        leftLiteral->AsEntity().IsAction())
                                    {
                                        // Rust's `type_of_action_in_entity_literals` and
                                        // `type_of_action_in_actions`.
                                        const EntityUid& action = leftLiteral->AsEntity();
                                        bool anyAction = false;
                                        bool within = false;
                                        for (const EntityUid& group : *rightLiterals)
                                        {
                                            if (!group.IsAction()) { continue; }
                                            anyAction = true;
                                            const ValidatorAction* declared = schema_.FindAction(group);
                                            // An undeclared action: the scope checks report it.
                                            if (declared == nullptr) { return Fail(AnyBooleanType()); }
                                            within = within || group == action || declared->HasDescendant(action);
                                        }
                                        if (!anyAction) { return Success(SingletonBooleanType(false)); }
                                        return Success(SingletonBooleanType(within));
                                    }
                                    const EntityLub* leftTypes = left.type ? AsEntityLub(*left.type) : nullptr;
                                    const EntityLub* rightTypes = right.type ? AsSetOrEntityLub(*right.type) : nullptr;
                                    if (leftTypes != nullptr && rightTypes != nullptr &&
                                        !AnyDescendantOf(*leftTypes, *rightTypes))
                                    {
                                        return Success(SingletonBooleanType(false));
                                    }
                                    return Success(AnyBooleanType());
                                });
                });
}

// ---- Extension functions ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index,
                                  const node::ExtensionCall& node) -> Answer
{
    const std::span<const ExpressionIndex> arguments = arena_.Elements(node.arguments);
    const auto typecheckArguments = [&]() -> std::optional<std::vector<Answer>>
    {
        std::vector<Answer> answers;
        for (const ExpressionIndex argument : arguments)
        {
            Answer answer = Typecheck(prior, argument);
            if (answer.state == AnswerState::RecursionLimit) { return std::nullopt; }
            answers.push_back(std::move(answer));
        }
        return answers;
    };
    const ExtensionCall* function = FindExtensionCall(node.function.QualifiedText());
    if (function == nullptr)
    {
        Report(DiagnosticCode::UndefinedFunction, index,
               std::format("undefined extension function: {}", node.function.QualifiedText()));
        if (!typecheckArguments()) { return Answer{AnswerState::RecursionLimit}; }
        return Fail(std::nullopt);
    }
    const Type returnType = TypeOfKind(function->returnKind);
    const std::size_t parameterCount = function->arity;
    bool failed = false;
    if (function->lastArgumentRepeats ? arguments.size() < parameterCount : arguments.size() != parameterCount)
    {
        Report(DiagnosticCode::WrongNumberArguments, index,
               std::format("wrong number of arguments in extension function application. Expected {}, got {}",
                           parameterCount, arguments.size()));
        failed = true;
    }
    // A constructor checks its argument when it is one string literal: it must parse (Rust's
    // `ArgumentCheckFn`s). In strict mode its arguments must all be literals.
    const bool isConstructor = function->style == ExtensionCallStyle::Function;
    const auto literalOf = [&](const ExpressionIndex argument) -> const node::Literal*
    { return std::get_if<node::Literal>(&arena_.Node(shapes_.Resolve(argument))); };
    if (isConstructor && arguments.size() == 1)
    {
        const node::Literal* literal = literalOf(arguments.front());
        if (literal != nullptr && literal->value.Kind() == ValueKind::String)
        {
            const std::array argument{literal->value};
            if (auto parsed = function->body(argument); !parsed)
            {
                Report(DiagnosticCode::FunctionArgumentValidation, index,
                       std::format("error during extension function argument validation: failed to parse as {} "
                                   "value: `{}`",
                                   ExtensionTypeName(function->returnKind), ToCedar(literal->value)),
                       std::string{parsed.error().Message()});
                failed = true;
            }
        }
    }
    if (IsStrict() && isConstructor &&
        !std::ranges::all_of(arguments, [&](const ExpressionIndex argument) { return literalOf(argument) != nullptr; }))
    {
        Report(DiagnosticCode::NonLiteralExtensionConstructor, index,
               "extension constructors may not be called with non-literal expressions",
               "consider applying extension constructors inside attribute values when constructing entity or context "
               "data");
        failed = true;
    }
    if (failed)
    {
        if (!typecheckArguments()) { return Answer{AnswerState::RecursionLimit}; }
        return Fail(returnType);
    }
    std::vector<Answer> answers;
    answers.reserve(arguments.size());
    for (std::size_t position = 0; position < arguments.size(); ++position)
    {
        const std::size_t parameter = std::min(position, parameterCount - 1);
        answers.push_back(Expect(prior, arguments[position], TypeOfKind(function->argumentKinds[parameter]), NoHelp));
    }
    return SequenceAll(std::move(answers), [&](std::vector<Answer>) { return Success(returnType); });
}

// ---- Attributes ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index,
                                  const node::GetAttribute& node) -> Answer
{
    const std::array expected{AnyEntityType(), AnyRecordType()};
    return Then(ExpectOneOf(prior, node.object, expected, NoHelp),
                [&](Answer object)
                {
                    if (!object.type) { return Fail(std::nullopt); }
                    const std::optional<AttributeType> attribute =
                        LookupAttributeType(schema_, *object.type, node.attribute);
                    if (attribute)
                    {
                        Type type = *attribute->type;
                        if (attribute->required ||
                            prior.Contains(AttributeCapability(node.object, {}, node.attribute), shapes_))
                        {
                            return Success(std::move(type));
                        }
                        ReportUnsafeOptionalAttributeAccess(index, *object.type, node.attribute);
                        return Fail(std::move(type));
                    }
                    ReportUnsafeAttributeAccess(index, *object.type, node.attribute,
                                                MayHaveAttribute(schema_, *object.type, node.attribute));
                    return Fail(std::nullopt);
                });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index,
                                  const node::HasAttribute& node) -> Answer
{
    const std::span<const std::string_view> path = arena_.Elements(node.path);
    if (path.size() > 1) { return HasPath(prior, index, node); }
    const std::string_view name = path.front();
    const std::array expected{AnyEntityType(), AnyRecordType()};
    return Then(ExpectOneOf(prior, node.object, expected, HasOperandHelp),
                [&](Answer object)
                {
                    if (!object.type) { return Fail(AnyBooleanType()); }
                    const std::optional<AttributeType> attribute = LookupAttributeType(schema_, *object.type, name);
                    if (!attribute)
                    {
                        return Success(MayHaveAttribute(schema_, *object.type, name) ? AnyBooleanType()
                                                                                    : SingletonBooleanType(false));
                    }
                    const Capability capability = AttributeCapability(node.object, {}, name);
                    const bool known = prior.Contains(capability, shapes_) ||
                                       (attribute->required && IsRecordKind(*object.type));
                    return Success(known ? SingletonBooleanType(true) : AnyBooleanType(),
                                   CapabilitySet::Singleton(capability));
                });
}

auto EnvironmentTypechecker::HasStepOf(const CapabilitySet& prior, const std::optional<Type>& objectType,
                                       const ExpressionIndex object, const std::span<const std::string_view> path,
                                       const std::string_view attribute, const ExpressionIndex at)
    -> std::optional<HasStep>
{
    if (!objectType) { return std::nullopt; }
    bool knownToExist = false;
    if (IsRecordKind(*objectType)) { knownToExist = true; }
    else if (!IsEntityKind(*objectType))
    {
        const std::array expected{AnyEntityType(), AnyRecordType()};
        ReportUnexpectedType(at, expected, *objectType, HasOperandHelp(*objectType));
        return std::nullopt;
    }
    const std::optional<AttributeType> found = LookupAttributeType(schema_, *objectType, attribute);
    if (found)
    {
        const Capability capability = AttributeCapability(object, path, attribute);
        if (prior.Contains(capability, shapes_) || (found->required && knownToExist))
        {
            return HasStep{BooleanType::True, {}};
        }
        return HasStep{BooleanType::AnyBoolean, CapabilitySet::Singleton(capability)};
    }
    if (MayHaveAttribute(schema_, *objectType, attribute)) { return HasStep{BooleanType::AnyBoolean, {}}; }
    return HasStep{BooleanType::False, {}};
}

auto EnvironmentTypechecker::GetStepOf(const CapabilitySet& prior, const std::optional<Type>& objectType,
                                       const ExpressionIndex object, const std::span<const std::string_view> path,
                                       const std::string_view attribute, const ExpressionIndex at)
    -> std::optional<Type>
{
    if (!objectType) { return std::nullopt; }
    const std::optional<AttributeType> found = LookupAttributeType(schema_, *objectType, attribute);
    if (found)
    {
        if (found->required || prior.Contains(AttributeCapability(object, path, attribute), shapes_))
        {
            return *found->type;
        }
        ReportUnsafeOptionalAttributeAccess(at, *objectType, attribute);
        return std::nullopt;
    }
    ReportUnsafeAttributeAccess(at, *objectType, attribute, MayHaveAttribute(schema_, *objectType, attribute));
    return std::nullopt;
}

// `object has a.b.c` (Rust's `ExtHasAttr`): tests each step, short-circuiting at a step known to
// be false; each step may read the attribute the steps before it tested.
auto EnvironmentTypechecker::HasPath(const CapabilitySet& prior, const ExpressionIndex index,
                                     const node::HasAttribute& node) -> Answer
{
    const std::span<const std::string_view> path = arena_.Elements(node.path);
    const std::array expected{AnyEntityType(), AnyRecordType()};
    return Then(ExpectOneOf(prior, node.object, expected, HasOperandHelp),
                [&](Answer object)
                {
                    if (!object.type) { return Fail(AnyBooleanType()); }
                    std::optional<Type> currentType = std::move(object.type);
                    CapabilitySet current = prior;
                    std::vector<HasStep> steps;
                    steps.reserve(path.size());
                    for (std::size_t position = 0; position < path.size(); ++position)
                    {
                        const std::span<const std::string_view> prefix = path.first(position);
                        // The first step reads the object as written; later steps read an access
                        // chain that only the `has` node's span covers.
                        const ExpressionIndex at = position == 0 ? node.object : index;
                        std::optional<HasStep> step =
                            HasStepOf(current, currentType, node.object, prefix, path[position], at);
                        if (!step) { return Fail(AnyBooleanType()); }
                        const bool shortCircuits = step->type == BooleanType::False;
                        const CapabilitySet next = Union(current, step->capabilities);
                        steps.push_back(std::move(*step));
                        if (position + 1 == path.size() || shortCircuits) { break; }
                        std::optional<Type> nextType =
                            GetStepOf(next, currentType, node.object, prefix, path[position], at);
                        if (!nextType) { return Fail(AnyBooleanType()); }
                        current = next;
                        currentType = std::move(nextType);
                    }
                    BooleanType resultType = steps.back().type;
                    CapabilitySet resultCapabilities = std::move(steps.back().capabilities);
                    steps.pop_back();
                    while (!steps.empty())
                    {
                        HasStep step = std::move(steps.back());
                        steps.pop_back();
                        switch (resultType)
                        {
                        case BooleanType::False: resultCapabilities = {}; break;
                        case BooleanType::True:
                            resultType = step.type;
                            resultCapabilities = Union(step.capabilities, resultCapabilities);
                            break;
                        case BooleanType::AnyBoolean:
                            resultCapabilities = Union(step.capabilities, resultCapabilities);
                            break;
                        }
                    }
                    return Success(BooleanOf(resultType), std::move(resultCapabilities));
                });
}

// ---- Other constructs ----

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/, const node::Like& node)
    -> Answer
{
    return Then(Expect(prior, node.operand, StringType(),
                       [](const Type& actual) -> std::optional<UnexpectedTypeHelp>
                       {
                           if (IsEntityKind(actual)) { return UnexpectedTypeHelp::TryUsingIs; }
                           return std::nullopt;
                       }),
                [](Answer) { return Success(AnyBooleanType()); });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/, const node::Is& node)
    -> Answer
{
    return Then(Expect(prior, node.operand, AnyEntityType(),
                       [](const Type&) -> std::optional<UnexpectedTypeHelp>
                       { return UnexpectedTypeHelp::TypeTestNotSupported; }),
                [&](Answer operand)
                {
                    if (!operand.type) { return Fail(AnyBooleanType()); }
                    if (const EntityLub* lub = AsEntityLub(*operand.type))
                    {
                        if (!lub->Contains(node.type)) { return Success(SingletonBooleanType(false)); }
                        const EntityType* single = lub->Single();
                        return Success(single != nullptr && *single == node.type ? SingletonBooleanType(true)
                                                                                 : AnyBooleanType());
                    }
                    if (std::holds_alternative<types::AnyEntity>(operand.type->kind))
                    {
                        return Success(AnyBooleanType());
                    }
                    return Fail(AnyBooleanType());
                });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex index,
                                  const node::SetLiteral& node) -> Answer
{
    const std::span<const ExpressionIndex> elements = arena_.Elements(node.elements);
    std::vector<Answer> answers;
    answers.reserve(elements.size());
    for (const ExpressionIndex element : elements) { answers.push_back(Typecheck(prior, element)); }
    return SequenceAll(std::move(answers),
                       [&](std::vector<Answer> typed)
                       {
                           std::vector<std::optional<Type>> types;
                           types.reserve(typed.size());
                           for (Answer& answer : typed) { types.push_back(std::move(answer.type)); }
                           std::optional<Type> lub = LeastUpperBoundOrError(index, types, LubContext::Set);
                           if (IsStrict() && elements.empty())
                           {
                               Report(DiagnosticCode::EmptySetForbidden, index,
                                      "empty set literals are forbidden in policies");
                               return Fail(std::nullopt);
                           }
                           if (!lub) { return Fail(std::nullopt); }
                           return Success(SetType(MakeShared(std::move(*lub))));
                       });
}

auto EnvironmentTypechecker::Node(const CapabilitySet& prior, const ExpressionIndex /*index*/,
                                  const node::RecordLiteral& node) -> Answer
{
    const std::span<const RecordLiteralField> fields = arena_.Elements(node.fields);
    std::vector<Answer> answers;
    answers.reserve(fields.size());
    for (const RecordLiteralField& field : fields) { answers.push_back(Typecheck(prior, field.value)); }
    return SequenceAll(std::move(answers),
                       [&](std::vector<Answer> typed)
                       {
                           std::vector<Attribute> attributes;
                           attributes.reserve(typed.size());
                           for (std::size_t position = 0; position < typed.size(); ++position)
                           {
                               if (!typed[position].type) { return Fail(std::nullopt); }
                               attributes.push_back(
                                   Attribute{std::string{fields[position].key},
                                             AttributeType{MakeShared(std::move(*typed[position].type)), true}});
                           }
                           return Success(
                               Type{types::Record{Attributes{std::move(attributes)}, OpenTag::ClosedAttributes}});
                       });
}

} // namespace

Typechecker::Typechecker(const ValidatorSchema& schema, const ValidationMode mode, const ExpressionArena& arena,
                         SharedSourceText source)
    : schema_{schema}, mode_{mode}, shapes_{arena}, source_{std::move(source)}
{
}

auto Typechecker::Check(const ExpressionIndex root, const TypecheckEnvironment& environment,
                        NodeTypes* const nodeTypes) const -> TypecheckResult
{
    TypecheckResult result{PolicyCheckOutcome::Fail, {}};
    EnvironmentTypechecker checker{schema_, mode_, shapes_, source_, environment, result.errors, nodeTypes};
    const Answer answer = checker.Expect(CapabilitySet{}, root, AnyBooleanType(), NoHelp);
    const bool alwaysFalse =
        answer.state != AnswerState::RecursionLimit && IsSingleton(answer.type, BooleanType::False);
    if (alwaysFalse) { result.outcome = PolicyCheckOutcome::Irrelevant; }
    else if (answer.state == AnswerState::Success) { result.outcome = PolicyCheckOutcome::Success; }
    return result;
}

} // namespace cedar::detail
