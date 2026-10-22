#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Names.hpp>
#include <cedar/Value.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

// ---- Vocabulary ----

/// The four request variables (Rust's `Var`).
enum class Variable : std::uint8_t
{
    Principal,
    Action,
    Resource,
    Context,
};

/// `principal`, `action`, `resource` or `context`.
[[nodiscard]] constexpr auto ToString(const Variable variable) noexcept -> std::string_view
{
    switch (variable)
    {
    case Variable::Principal: return "principal";
    case Variable::Action: return "action";
    case Variable::Resource: return "resource";
    case Variable::Context: return "context";
    }
    return {};
}

/// The built-in operators with one operand (Rust's `UnaryOp`).
enum class UnaryOperator : std::uint8_t
{
    Not,
    Negate,
    IsEmpty,
};

/// `!`, `-` or `isEmpty`, as Rust prints the operator.
[[nodiscard]] constexpr auto ToString(const UnaryOperator operation) noexcept -> std::string_view
{
    switch (operation)
    {
    case UnaryOperator::Not: return "!";
    case UnaryOperator::Negate: return "-";
    case UnaryOperator::IsEmpty: return "isEmpty";
    }
    return {};
}

/// The built-in operators with two operands (Rust's `BinaryOp`). `!=`, `>` and `>=` are not here:
/// they are built as `!(a == b)`, `!(a <= b)` and `!(a < b)`.
enum class BinaryOperator : std::uint8_t
{
    Equal,
    Less,
    LessEqual,
    Add,
    Subtract,
    Multiply,
    In,
    Contains,
    ContainsAll,
    ContainsAny,
    GetTag,
    HasTag,
};

/// `==`, `<`, `<=`, `+`, `-`, `*`, `in`, `contains`, `containsAll`, `containsAny`, `getTag` or
/// `hasTag`, as Rust prints the operator.
[[nodiscard]] constexpr auto ToString(const BinaryOperator operation) noexcept -> std::string_view
{
    switch (operation)
    {
    case BinaryOperator::Equal: return "==";
    case BinaryOperator::Less: return "<";
    case BinaryOperator::LessEqual: return "<=";
    case BinaryOperator::Add: return "+";
    case BinaryOperator::Subtract: return "-";
    case BinaryOperator::Multiply: return "*";
    case BinaryOperator::In: return "in";
    case BinaryOperator::Contains: return "contains";
    case BinaryOperator::ContainsAll: return "containsAll";
    case BinaryOperator::ContainsAny: return "containsAny";
    case BinaryOperator::GetTag: return "getTag";
    case BinaryOperator::HasTag: return "hasTag";
    }
    return {};
}

// ---- Indices and ranges ----

/// The position of a node in its ExpressionArena. A child's index is always lower than its
/// parent's.
enum class ExpressionIndex : std::uint32_t
{
};

/// A run of `count` elements starting at `first` in one of an ExpressionArena's side vectors; the
/// element type says which (ExpressionArena::Elements).
template <typename Element>
struct ArenaRange
{
    std::uint32_t first{};
    std::uint32_t count{};

    friend constexpr auto operator==(ArenaRange, ArenaRange) noexcept -> bool = default;
};

/// One field of a record literal: its key and the node computing its value.
struct RecordLiteralField
{
    std::string_view key;
    ExpressionIndex value;
};

// ---- Pattern ----

/// The pattern of a `like` expression, stored as the literal text between its wildcards: a pattern
/// with n wildcards has n + 1 literals, any of which may be empty. `a*b` is {"a", "b"}, `*` is
/// {"", ""}, `ab` is {"ab"}, and `a**` is {"a", "", ""}. The literals are UTF-8 after unescaping.
///
/// Which stars are wildcards depends on the input format. In Cedar text every unescaped `*` is a
/// wildcard, including one written as the escape `\u{2a}`; only the two-character escape `\*` is a
/// literal star (`cedar-policy-core/src/parser/unescape.rs:55-65`). In the JSON policy format the
/// pattern is a list of elements, and a `{"Literal": "*"}` element is a literal star.
///
/// Rust stores a sequence of characters and wildcards
/// (`cedar-policy-core/src/ast/pattern.rs`); matching literal byte runs instead is equivalent,
/// because a valid UTF-8 run can only match valid UTF-8 text at character boundaries.
class Pattern final
{
public:
    /// Wraps `literals`, which must be non-empty and outlive the pattern.
    explicit constexpr Pattern(const std::span<const std::string_view> literals) noexcept : literals_{literals} {}

    /// The literal text between wildcards, `WildcardCount() + 1` entries.
    [[nodiscard]] constexpr auto Literals() const noexcept -> std::span<const std::string_view> { return literals_; }

    [[nodiscard]] constexpr auto WildcardCount() const noexcept -> std::size_t { return literals_.size() - 1; }

    /// Whether `text` matches: every literal in order, with each wildcard matching any run of
    /// characters, including none (Rust's `Pattern::wildcard_match`).
    [[nodiscard]] constexpr auto Matches(std::string_view text) const noexcept -> bool
    {
        const std::string_view first = literals_.front();
        if (literals_.size() == 1) { return text == first; }
        const std::string_view last = literals_.back();
        if (text.size() < first.size() + last.size() || !text.starts_with(first) || !text.ends_with(last))
        {
            return false;
        }
        text = text.substr(first.size(), text.size() - first.size() - last.size());
        for (const std::string_view literal : literals_.subspan(1, literals_.size() - 2))
        {
            const std::size_t found = text.find(literal);
            if (found == std::string_view::npos) { return false; }
            text.remove_prefix(found + literal.size());
        }
        return true;
    }

private:
    std::span<const std::string_view> literals_;
};

// ---- Nodes ----

/// The node types, one per Cedar expression construct (Rust's `ExprKind` minus partial evaluation's
/// `Unknown`). Children are ExpressionIndex values into the same arena; variadic children are
/// ArenaRanges into the arena's side vectors. Text (attribute names, record keys, pattern literals)
/// is a view into storage the arena owns.
namespace node
{

/// A bool, long, string or entity literal.
struct Literal
{
    Value value;
};

/// A subtree whose value is known when the expression is built: an extension constructor call
/// with a constant argument (`ip("10.0.0.0/8")`) or a set literal of constants, such as the action
/// list of a scope. The evaluator returns `value` without evaluating anything; the printers, the
/// JSON writer and the validator visit `written`, the subtree as written, which is in the same
/// arena and has a lower index.
struct Folded
{
    // Overlaps Value's tail padding, keeping the node, and so ExpressionNode, at 32 bytes.
    [[no_unique_address]] Value value;
    ExpressionIndex written;
};

/// `principal`, `action`, `resource` or `context`.
struct Variable
{
    cedar::Variable variable;
};

/// `?principal` or `?resource`; evaluates to the policy's binding for the slot.
struct Slot
{
    SlotId slot;
};

/// `if condition then thenBranch else elseBranch`.
struct If
{
    ExpressionIndex condition;
    ExpressionIndex thenBranch;
    ExpressionIndex elseBranch;
};

/// `left && right`, short-circuiting.
struct And
{
    ExpressionIndex left;
    ExpressionIndex right;
};

/// `left || right`, short-circuiting.
struct Or
{
    ExpressionIndex left;
    ExpressionIndex right;
};

/// A built-in unary operator applied to `operand`.
struct Unary
{
    UnaryOperator operation;
    ExpressionIndex operand;
};

/// A built-in binary operator applied to `left` and `right`.
struct Binary
{
    BinaryOperator operation;
    ExpressionIndex left;
    ExpressionIndex right;
};

/// A call of the extension function `function`. For a method-style call (`a.lessThan(b)`) the
/// receiver is the first argument.
struct ExtensionCall
{
    Name function;
    ArenaRange<ExpressionIndex> arguments;
};

/// `object.attribute` (or `object["attribute"]`) on an entity or a record.
struct GetAttribute
{
    ExpressionIndex object;
    std::string_view attribute;
};

/// `object has a.b.c`: true when `object` has `a`, `object.a` has `b`, and so on, evaluated left to
/// right with short-circuiting (Rust's `HasAttr` for one attribute and `ExtHasAttr` for more). The
/// path is never empty.
struct HasAttribute
{
    ExpressionIndex object;
    ArenaRange<std::string_view> path;
};

/// `operand like pattern`; ExpressionArena::PatternOf gives the Pattern.
struct Like
{
    ExpressionIndex operand;
    ArenaRange<std::string_view> pattern;
};

/// `operand is type`.
struct Is
{
    ExpressionIndex operand;
    EntityType type;
};

/// `[e1, e2, ...]`, in source order.
struct SetLiteral
{
    ArenaRange<ExpressionIndex> elements;
};

/// `{k1: e1, k2: e2, ...}`, fields sorted by key, byte-wise, with no duplicate key (Rust keeps a
/// `BTreeMap`, so fields are evaluated and printed in key order).
struct RecordLiteral
{
    ArenaRange<RecordLiteralField> fields;
};

} // namespace node

/// One expression node of an ExpressionArena, addressed by its 32-bit ExpressionIndex.
using ExpressionNode = std::variant<node::Literal, node::Folded, node::Variable, node::Slot, node::If, node::And, node::Or,
                                    node::Unary, node::Binary, node::ExtensionCall, node::GetAttribute,
                                    node::HasAttribute, node::Like, node::Is, node::SetLiteral, node::RecordLiteral>;

static_assert(sizeof(node::Folded) == sizeof(Value));
static_assert(sizeof(ExpressionNode) <= 32);

// ---- Visiting ----

/// Combines lambdas into one overloaded callable for `std::visit`.
template <typename... Callables>
struct Overloaded : Callables...
{
    using Callables::operator()...;
};

template <typename... Callables>
Overloaded(Callables...) -> Overloaded<Callables...>;

namespace detail
{
template <typename Visitor, typename Variant>
struct VisitsEveryAlternative : std::false_type
{
};

template <typename Visitor, typename... Alternatives>
struct VisitsEveryAlternative<Visitor, std::variant<Alternatives...>>
    : std::bool_constant<(std::invocable<Visitor, const Alternatives&> && ...)>
{
};
} // namespace detail

/// A callable that accepts every node type by const reference.
template <typename Visitor>
concept ExpressionNodeVisitor = detail::VisitsEveryAlternative<Visitor, ExpressionNode>::value;

/// Calls `visitor` with the node's alternative.
template <ExpressionNodeVisitor Visitor>
constexpr auto Visit(const ExpressionNode& node, Visitor&& visitor) -> decltype(auto)
{
    return std::visit(std::forward<Visitor>(visitor), node);
}

// ---- Arena ----

/// The storage for one expression tree, normally a policy's whole condition: the nodes in
/// post-order (every child before its parent, so the root is the last node appended), a parallel
/// vector of source spans, the side vectors that variadic nodes index into, and a ValueArena that
/// owns literal strings, entity uids, folded composite literals, attribute names and pattern text.
///
/// A node may be the child of more than one parent (`x is T in y` shares `x`). Nodes are only
/// appended, through ExpressionBuilder. Once built, an arena is immutable and safe to read from any
/// number of threads. Movable, not copyable; Values in it point into its own ValueArena, which
/// does not move.
class ExpressionArena final
{
public:
    ExpressionArena();
    ExpressionArena(ExpressionArena&&) noexcept;
    auto operator=(ExpressionArena&&) noexcept -> ExpressionArena&;
    ExpressionArena(const ExpressionArena&) = delete;
    auto operator=(const ExpressionArena&) -> ExpressionArena& = delete;
    ~ExpressionArena();

    /// The number of nodes.
    [[nodiscard]] auto Size() const noexcept -> std::size_t { return nodes_.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return nodes_.empty(); }

    /// The last node appended. Precondition: not empty.
    [[nodiscard]] auto Root() const noexcept -> ExpressionIndex
    {
        return static_cast<ExpressionIndex>(nodes_.size() - 1);
    }

    [[nodiscard]] auto Node(const ExpressionIndex index) const noexcept -> const ExpressionNode&
    {
        return nodes_[std::to_underlying(index)];
    }

    /// Where the node was written, or nothing for a node with no source text (JSON input, nodes
    /// synthesized from the scope, hand-built expressions).
    [[nodiscard]] auto Span(const ExpressionIndex index) const noexcept -> std::optional<SourceSpan>
    {
        const SourceSpan span = spans_[std::to_underlying(index)];
        if (span == NoSpan) { return std::nullopt; }
        return span;
    }

    /// Calls `visitor` with the node at `index`.
    template <ExpressionNodeVisitor Visitor>
    auto Visit(const ExpressionIndex index, Visitor&& visitor) const -> decltype(auto)
    {
        return std::visit(std::forward<Visitor>(visitor), Node(index));
    }

    [[nodiscard]] auto Elements(const ArenaRange<ExpressionIndex> range) const noexcept
        -> std::span<const ExpressionIndex>
    {
        return std::span{children_}.subspan(range.first, range.count);
    }
    [[nodiscard]] auto Elements(const ArenaRange<RecordLiteralField> range) const noexcept
        -> std::span<const RecordLiteralField>
    {
        return std::span{fields_}.subspan(range.first, range.count);
    }
    [[nodiscard]] auto Elements(const ArenaRange<std::string_view> range) const noexcept
        -> std::span<const std::string_view>
    {
        return std::span{texts_}.subspan(range.first, range.count);
    }

    [[nodiscard]] auto PatternOf(const node::Like& like) const noexcept -> Pattern
    {
        return Pattern{Elements(like.pattern)};
    }

private:
    friend class ExpressionBuilder;

    static constexpr SourceSpan NoSpan{std::numeric_limits<std::uint32_t>::max(),
                                       std::numeric_limits<std::uint32_t>::max()};

    std::vector<ExpressionNode> nodes_;
    std::vector<SourceSpan> spans_;
    std::vector<ExpressionIndex> children_;
    std::vector<RecordLiteralField> fields_;
    std::vector<std::string_view> texts_;
    std::unique_ptr<ValueArena> values_;
};

/// Appends nodes to an ExpressionArena. The one builder the policy parser, the JSON policy reader
/// and tests share (Rust's `ExprBuilder` for the AST, `cedar-policy-core/src/expr_builder.rs`).
///
/// Every method appends one node (or, for the derived forms, a few nodes) whose children must
/// already be in the arena, and returns the index of the new top node. Text and values are copied
/// into the arena. The optional `span` is the node's source location; the derived forms give it to
/// every node they make, as Rust does.
class ExpressionBuilder final
{
public:
    explicit ExpressionBuilder(ExpressionArena& arena) noexcept : arena_{&arena} {}

    using OptionalSpan = std::optional<SourceSpan>;

    // Leaves.
    auto Bool(bool value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Long(std::int64_t value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto String(std::string_view value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Entity(const EntityUid& value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// Any value, deep-copied into the arena.
    auto Literal(const Value& value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// The subtree `written`, already built, whose value is known to be `value` (deep-copied into
    /// the arena); see node::Folded.
    auto Fold(ExpressionIndex written, const Value& value, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Variable(cedar::Variable variable, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Slot(SlotId slot, OptionalSpan span = std::nullopt) -> ExpressionIndex;

    // Control flow.
    auto If(ExpressionIndex condition, ExpressionIndex thenBranch, ExpressionIndex elseBranch,
            OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// `left && right`, or the bool literal it equals when both are bool literals (as Rust's
    /// builder folds them, so `true && false` is the literal `false`).
    auto And(ExpressionIndex left, ExpressionIndex right, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// `left || right`, folded like And.
    auto Or(ExpressionIndex left, ExpressionIndex right, OptionalSpan span = std::nullopt) -> ExpressionIndex;

    // Operators.
    auto Unary(UnaryOperator operation, ExpressionIndex operand, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Binary(BinaryOperator operation, ExpressionIndex left, ExpressionIndex right,
                OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto Not(const ExpressionIndex operand, const OptionalSpan span = std::nullopt) -> ExpressionIndex
    {
        return Unary(UnaryOperator::Not, operand, span);
    }
    auto Equal(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span = std::nullopt)
        -> ExpressionIndex
    {
        return Binary(BinaryOperator::Equal, left, right, span);
    }
    auto In(const ExpressionIndex left, const ExpressionIndex right, const OptionalSpan span = std::nullopt)
        -> ExpressionIndex
    {
        return Binary(BinaryOperator::In, left, right, span);
    }

    // Derived forms, built exactly as Rust builds them.
    /// `!(left == right)`.
    auto NotEqual(ExpressionIndex left, ExpressionIndex right, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// `!(left <= right)`.
    auto Greater(ExpressionIndex left, ExpressionIndex right, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// `!(left < right)`.
    auto GreaterEqual(ExpressionIndex left, ExpressionIndex right, OptionalSpan span = std::nullopt)
        -> ExpressionIndex;
    /// `(operand is type) && (operand in target)`, sharing the operand node.
    auto IsIn(ExpressionIndex operand, const EntityType& type, ExpressionIndex target,
              OptionalSpan span = std::nullopt) -> ExpressionIndex;

    // Calls, attributes, tests.
    auto ExtensionCall(const Name& function, std::span<const ExpressionIndex> arguments,
                       OptionalSpan span = std::nullopt) -> ExpressionIndex;
    auto GetAttribute(ExpressionIndex object, std::string_view attribute, OptionalSpan span = std::nullopt)
        -> ExpressionIndex;
    auto HasAttribute(ExpressionIndex object, std::string_view attribute, OptionalSpan span = std::nullopt)
        -> ExpressionIndex;
    /// `object has path[0].path[1]...`. Precondition: `path` is not empty.
    auto HasAttribute(ExpressionIndex object, std::span<const std::string_view> path,
                      OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// `operand like pattern`, with the pattern given as its literals (see Pattern). Precondition:
    /// `literals` is not empty.
    auto Like(ExpressionIndex operand, std::span<const std::string_view> literals, OptionalSpan span = std::nullopt)
        -> ExpressionIndex;
    auto Is(ExpressionIndex operand, const EntityType& type, OptionalSpan span = std::nullopt) -> ExpressionIndex;

    // Composite literals.
    auto SetLiteral(std::span<const ExpressionIndex> elements, OptionalSpan span = std::nullopt) -> ExpressionIndex;
    /// Sorts the fields by key. Fails, naming the key (a view of the caller's field key), when a
    /// key appears twice; Cedar reports that as a parse error for policy text and a JSON error for
    /// the JSON format.
    auto RecordLiteral(std::span<const RecordLiteralField> fields, OptionalSpan span = std::nullopt)
        -> std::expected<ExpressionIndex, DuplicateRecordKey>;

private:
    auto Append(ExpressionNode node, OptionalSpan span) -> ExpressionIndex;
    auto AppendChildren(std::span<const ExpressionIndex> children) -> ArenaRange<ExpressionIndex>;
    auto AppendTexts(std::span<const std::string_view> texts) -> ArenaRange<std::string_view>;
    auto CheckChild(ExpressionIndex child) const noexcept -> void;
    /// The two values when both nodes are bool literals.
    auto BoolLiterals(ExpressionIndex left, ExpressionIndex right) const noexcept
        -> std::optional<std::pair<bool, bool>>;

    ExpressionArena* arena_;
};

// ---- Standalone expression ----

/// An expression on its own, outside any policy (Rust's public `Expression`): the arena holding it,
/// its root, and the text it was read from (null for a hand-built expression). Immutable; copies
/// share the arena.
class Expression final
{
public:
    /// The expression rooted at `root` in `arena`. Precondition: `root` is a node of `arena`.
    Expression(ExpressionArena arena, const ExpressionIndex root, SharedSourceText source = {})
        : arena_{std::make_shared<const ExpressionArena>(std::move(arena))}, root_{root}, source_{std::move(source)}
    {
    }

    [[nodiscard]] auto Arena() const noexcept -> const ExpressionArena& { return *arena_; }
    [[nodiscard]] auto Root() const noexcept -> ExpressionIndex { return root_; }

    /// The text the expression was read from, or null. Spans in the arena are offsets into it.
    [[nodiscard]] auto Source() const noexcept -> const SharedSourceText& { return source_; }

private:
    std::shared_ptr<const ExpressionArena> arena_;
    ExpressionIndex root_;
    SharedSourceText source_;
};

/// A deterministic one-line S-expression of the tree under `root`, for tests and debugging:
/// `(&& (== principal ?principal) (in action (set Action::"view")))`. Literals print as ToCedar,
/// operators as Rust prints them except integer negation (`neg`), attribute names, record keys
/// and patterns quoted, and a folded subtree as `(folded <value> <written>)`. Not Cedar syntax.
[[nodiscard]] auto DumpExpression(const ExpressionArena& arena, ExpressionIndex root) -> std::string;

} // namespace cedar
