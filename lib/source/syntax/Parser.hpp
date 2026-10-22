#pragma once

#include "syntax/Lexer.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cedar::detail
{

/// A recursive-descent parser for Cedar policy text (`cedar-policy-core/src/parser/grammar.lalrpop`),
/// building AST nodes directly, with the checks Rust applies when it converts its CST to the AST
/// (`parser/cst_to_ast.rs`) done as each construct is read.
///
/// Errors come in two kinds, as in Rust:
/// - A syntax error (including every lexical error) stops the parse: from then on the cursor reads
///   as end of input and nothing more is reported, until Resume.
/// - A conversion error (a reserved word as a name, an unknown function, a duplicate record key,
///   ...) is reported and parsing goes on; the construct it was in yields no node.
/// Rust converts the CST only when the whole text parsed, so when any syntax error was reported,
/// TakeDiagnostics returns only the syntax errors.
///
/// Nodes go to the arena given to UseArena. Every node gets the span of the text it was read from.
class Parser final
{
public:
    /// Which text a parsed policy keeps as its original text (Template::OriginalText).
    enum class PolicyText : std::uint8_t
    {
        Policy,     ///< From its first annotation or its effect through its `;`, as in a policy set.
        WholeInput, ///< The whole input, as Rust's `Policy::parse` and `Template::parse` keep it.
    };

    /// The deepest nesting of expressions accepted (parentheses, brackets, braces, call arguments,
    /// `if` branches: each is one level). Rust has no explicit limit; its recursive CST conversion
    /// overflows the stack somewhere between 700 and 1000 nested parentheses, so every expression
    /// it accepts is within this limit. The parser uses about nine stack frames per level.
    static constexpr std::uint32_t MaximumNestingDepth = 1000;

    /// Reads the tokens of `source->text`. Precondition: the text is below 4 GiB.
    explicit Parser(SharedSourceText source);

    /// Directs the nodes built from here on into `arena`, which must outlive their building.
    auto UseArena(ExpressionArena& arena) noexcept -> void { arena_ = &arena; }

    // ---- Grammar entry points ----

    /// `Expr`: one expression. Nothing when it could not be built (the reason is reported).
    [[nodiscard]] auto ParseExpression() -> std::optional<ExpressionIndex>;

    /// `Ref` in its uid form, `Path::To::Type::"id"` (Rust `parse_euid`). Builds no node.
    [[nodiscard]] auto ParseEntityUidLiteral() -> std::optional<EntityUid>;

    /// `Policy`: one policy or template named `id`, its slots allowed. Null when it could not be
    /// built (the reason is reported).
    [[nodiscard]] auto ParsePolicy(PolicyId id, PolicyText text) -> std::shared_ptr<const Template>;

    /// `Policies`: every policy and template to the end of the input, named `policy0`, `policy1`,
    /// ... by position (Rust `with_generated_policyids`). After a syntax error, reading resumes
    /// after the next `;`, so every policy's errors are reported; a lexical error ends the read.
    [[nodiscard]] auto ParsePolicies() -> std::vector<std::shared_ptr<const Template>>;

    /// Reports a syntax error unless the cursor is at the end of the input.
    auto ExpectEndOfInput() -> bool;

    // ---- Diagnostics ----

    [[nodiscard]] auto HasErrors() const noexcept -> bool
    {
        return !syntaxDiagnostics_.empty() || !conversionDiagnostics_.empty();
    }

    /// The syntax errors when there are any, otherwise the conversion errors; both lists are left
    /// empty.
    [[nodiscard]] auto TakeDiagnostics() -> std::vector<Diagnostic>;

    [[nodiscard]] auto Source() const noexcept -> const SharedSourceText& { return source_; }

private:
    // What a construct read so far stands for, before it is used (Rust's `ExprOrSpecial`). Names,
    // variables and string and boolean literals stay unbuilt because some positions take them as
    // something other than an expression: a record key, an attribute after `has`, a pattern, an
    // entity type after `is`, an index, a function name.
    enum class OperandKind : std::uint8_t
    {
        Error, ///< Already reported; using it reports nothing more.
        Expression,
        Variable,
        Name,
        String,
        Bool,
    };

    struct Operand
    {
        OperandKind kind{OperandKind::Error};
        SourceSpan span{};
        ExpressionIndex expression{};
        cedar::Variable variable{};
        bool boolean{};
        Token token{};            ///< The String token, escapes still in place.
        std::optional<Name> name; ///< A valid name; its components were checked when it was read.
    };

    // One `.field`, `(arguments)` or `[index]` after a primary (Rust's `AstAccessor`).
    enum class AccessKind : std::uint8_t
    {
        Field,
        Call,
        Index,
    };

    struct Access
    {
        AccessKind kind{};
        SourceSpan span{};
        std::string text; ///< The field name, or the unescaped index.
        std::vector<ExpressionIndex> arguments;
    };

    // ---- Cursor ----

    [[nodiscard]] auto Current() const noexcept -> const Token&
    {
        return stopped_ ? tokens_.back() : tokens_[position_];
    }
    /// The token `offset` places after the current one (end of input past the end).
    [[nodiscard]] auto Peek(std::size_t offset) const noexcept -> const Token&;
    [[nodiscard]] auto At(const TokenKind kind) const noexcept -> bool { return Current().kind == kind; }
    [[nodiscard]] auto TextOf(const Token& token) const noexcept -> std::string_view
    {
        return token.Text(source_->text);
    }
    auto Advance() -> void;
    auto Accept(TokenKind kind) -> bool;
    /// Consumes a `kind` token, or reports a syntax error naming it.
    auto Expect(TokenKind kind) -> bool;
    /// The span from `begin` to the end of the last token consumed.
    [[nodiscard]] auto SpanFrom(std::uint32_t begin) const noexcept -> SourceSpan;
    /// Ends the stop after a syntax error, so a caller can skip to a synchronization point.
    auto Resume() noexcept -> void { stopped_ = false; }

    // ---- Reporting ----

    auto ReportSyntax(DiagnosticCode code, SourceSpan span, std::string message) -> void;
    auto ReportUnexpected() -> void;
    auto Report(DiagnosticCode code, SourceSpan span, std::string message,
                std::optional<std::string> help = std::nullopt) -> void;

    // ---- Policies ----

    // The scope constraints of one policy as they are read.
    struct Scope
    {
        PrincipalConstraint principal{};
        ActionConstraint action{ActionAny{}};
        ResourceConstraint resource{};
        std::size_t count{}; ///< Scope elements read.
        bool valid{true};
    };

    // What a scope element's right-hand side may be (Rust `RefKind`).
    enum class ReferenceKind : std::uint8_t
    {
        EntityOrSlot, ///< `principal == E`, `resource in E`: an entity uid or the matching slot.
        Entity,       ///< `action == A`, and each element of an action list.
        EntityOrList, ///< `action in A`, `action in [A, ...]`.
    };

    // A scope right-hand side, converted: the slot, one entity uid, or the uids of `[A, ...]`.
    struct Reference
    {
        std::vector<EntityUid> uids;
        std::optional<SourceSpan> slot; ///< Set for the slot, whose node may lack a span.
        bool isSlot{};
        bool isList{};
    };

    auto ParseAnnotation(Annotations& annotations) -> bool;
    /// `VariableDef`: the next element of the scope, converted into `scope`. Its expressions are
    /// built into `scratch`, which only the conversion reads.
    auto ParseScopeElement(Scope& scope, ExpressionArena& scratch) -> void;
    /// One `when` or `unless` clause; its condition (negated for `unless`) or nothing.
    [[nodiscard]] auto ParseCondition(bool& valid) -> std::optional<ExpressionIndex>;
    /// `Name` after `principal:`, read for its syntax only.
    auto ParseTypeConstraintName() -> void;
    [[nodiscard]] auto ConvertReference(const Operand& operand, const ExpressionArena& arena, Variable variable,
                                        ReferenceKind kind) -> std::optional<Reference>;
    [[nodiscard]] auto ConvertEntityNode(const ExpressionArena& arena, ExpressionIndex index, Variable variable,
                                         ReferenceKind kind, SourceSpan span) -> std::optional<Reference>;
    [[nodiscard]] static auto ExpectationOf(ReferenceKind kind) noexcept -> std::string_view;
    /// The entity type an `is` operand names (Rust `into_entity_type`).
    [[nodiscard]] auto EntityTypeOf(const Operand& operand) -> std::optional<EntityType>;
    /// Skips past the next `;` after a syntax error. False when the error was lexical, which ends
    /// the read as it does in Rust.
    auto Synchronize() -> bool;

    // ---- Expressions, one function per grammar rule ----

    //
    // Nested expressions recurse through ParseExpressionOperand, ParseOr, ParseAnd, ParseRelation,
    // ParseAdd, ParseMultiply, ParseUnary, ParseMember and ParsePrimary, so those keep small
    // frames: each construct's own work is in a function of its own, kept out of line, which is
    // on the stack only while that construct is read. That matters most in the sanitized build,
    // whose frames are several times larger.

    [[nodiscard]] auto ParseExpressionOperand() -> Operand;
    [[nodiscard, gnu::noinline]] auto ReportNestingTooDeep() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseIf() -> Operand;
    [[nodiscard]] auto ParseOr() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseOrAfter(const Operand& first, std::uint32_t begin) -> Operand;
    [[nodiscard]] auto ParseAnd() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseAndAfter(const Operand& first, std::uint32_t begin) -> Operand;
    [[nodiscard]] auto ParseRelation() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseComparison(const Operand& first, std::uint32_t begin) -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseLike(const Operand& first, std::uint32_t begin) -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseIs(const Operand& first, std::uint32_t begin) -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseHas(const Operand& target, std::uint32_t begin) -> Operand;
    [[nodiscard]] auto ParseAdd() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseAddAfter(Operand first, std::uint32_t begin) -> Operand;
    [[nodiscard]] auto ParseMultiply() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseMultiplyAfter(Operand first, std::uint32_t begin) -> Operand;
    [[nodiscard]] auto ParseUnary() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseUnaryOperators() -> Operand;
    [[nodiscard]] auto ParseMember() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseAccessChain(const Operand& primary, std::uint32_t begin) -> Operand;
    /// Reads every access at the cursor into `accesses`; false when one was refused.
    auto ParseAccesses(std::vector<Access>& accesses) -> bool;
    auto ParseAccess(Access& access) -> bool;
    [[nodiscard]] auto ParsePrimary() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseNumber() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseOtherSlot() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseParenthesized() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseNameOrReference() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseSetLiteral() -> Operand;
    [[nodiscard, gnu::noinline]] auto ParseRecordLiteral() -> Operand;
    /// The `{ key: literal, ... }` form after `Type::`, read for its syntax and refused.
    auto ParseReferenceInitializers(std::uint32_t begin) -> void;
    /// `Comma<E>` up to `closing`, calling `element` for each; consumes `closing`.
    template <typename Element>
    auto ParseCommaList(TokenKind closing, Element element) -> void;

    // ---- Conversions ----

    /// The expression an operand stands for, built if need be (Rust `into_expr`).
    [[nodiscard]] auto Materialize(const Operand& operand) -> std::optional<ExpressionIndex>;
    /// The operand as a record key (Rust `into_valid_attr`).
    [[nodiscard]] auto AttributeOf(const Operand& operand) -> std::optional<std::string>;
    /// The unescaped text of a String token.
    [[nodiscard]] auto UnescapeToken(const Token& token) -> std::optional<std::string>;
    /// The name with these components, each checked (Rust `to_name`).
    [[nodiscard]] auto ResolveName(std::span<const Token> components, SourceSpan span) -> std::optional<Name>;
    /// Checks a field or attribute name after `.` (Rust `to_unreserved_ident`).
    auto CheckFieldName(const Token& token) -> bool;
    [[nodiscard]] auto BuildMethodCall(ExpressionIndex receiver, const Access& method,
                                       std::span<const ExpressionIndex> arguments, SourceSpan span)
        -> std::optional<ExpressionIndex>;
    [[nodiscard]] auto BuildFunctionCall(const Name& function, std::span<const ExpressionIndex> arguments,
                                         SourceSpan span) -> std::optional<ExpressionIndex>;
    [[nodiscard]] auto BuildRelation(ExpressionIndex left, TokenKind operation, ExpressionIndex right,
                                     SourceSpan span) -> std::optional<ExpressionIndex>;

    [[nodiscard]] auto Builder() const noexcept -> ExpressionBuilder { return ExpressionBuilder{*arena_}; }
    [[nodiscard]] static auto ErrorOperand() -> Operand { return {}; }
    [[nodiscard]] static auto ExpressionOperand(ExpressionIndex index, SourceSpan span) -> Operand;

    SharedSourceText source_;
    std::vector<Token> tokens_;
    std::size_t position_{};
    std::uint32_t previousEnd_{};
    std::uint32_t depth_{};
    bool stopped_{};
    ExpressionArena* arena_{};
    std::vector<Diagnostic> syntaxDiagnostics_;
    std::vector<Diagnostic> conversionDiagnostics_;
};

} // namespace cedar::detail
