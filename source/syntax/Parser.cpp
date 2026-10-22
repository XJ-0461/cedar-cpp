#include "syntax/Parser.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "model/FuzzyMatch.hpp"
#include "syntax/Folding.hpp"
#include "syntax/Unescape.hpp"

#include <cedar/Errors.hpp>
#include <cedar/Syntax.hpp>
#include <cedar/Value.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cedar::detail
{

namespace
{

// `contains`, `isEmpty` and the other operators written as methods (`cst_to_ast.rs` `to_meth`).
struct BuiltInMethod
{
    std::string_view name;
    std::uint8_t argumentCount; // Not counting the receiver.
};

constexpr std::array BuiltInMethods{
    BuiltInMethod{"contains", 1}, BuiltInMethod{"containsAll", 1}, BuiltInMethod{"containsAny", 1},
    BuiltInMethod{"getTag", 1},   BuiltInMethod{"hasTag", 1},      BuiltInMethod{"isEmpty", 0},
};

[[nodiscard]] constexpr auto FindBuiltInMethod(const std::string_view name) noexcept -> const BuiltInMethod*
{
    const auto* found = std::ranges::find(BuiltInMethods, name, &BuiltInMethod::name);
    return found == BuiltInMethods.end() ? nullptr : found;
}

[[nodiscard]] constexpr auto IsAccessStart(const TokenKind kind) noexcept -> bool
{
    return kind == TokenKind::Dot || kind == TokenKind::LeftParenthesis || kind == TokenKind::LeftBracket;
}

[[nodiscard]] constexpr auto IsRelationOperator(const TokenKind kind) noexcept -> bool
{
    switch (kind)
    {
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::GreaterEqual:
    case TokenKind::Greater:
    case TokenKind::NotEqual:
    case TokenKind::EqualEqual:
    case TokenKind::Equal:
    case TokenKind::In: return true;
    default: return false;
    }
}

[[nodiscard]] constexpr auto IsMultiplyOperator(const TokenKind kind) noexcept -> bool
{
    return kind == TokenKind::Star || kind == TokenKind::Slash || kind == TokenKind::Percent;
}

[[nodiscard]] constexpr auto IsAddOperator(const TokenKind kind) noexcept -> bool
{
    return kind == TokenKind::Plus || kind == TokenKind::Minus;
}

// The request variable a one-word name stands for (Rust `maybe_to_var`).
[[nodiscard]] constexpr auto VariableOf(const TokenKind kind) noexcept -> std::optional<Variable>
{
    switch (kind)
    {
    case TokenKind::Principal: return Variable::Principal;
    case TokenKind::Action: return Variable::Action;
    case TokenKind::Resource: return Variable::Resource;
    case TokenKind::Context: return Variable::Context;
    default: return std::nullopt;
    }
}

// The largest literal magnitude, allowed only directly after a `-` (it is i64's minimum).
constexpr std::uint64_t NegativeLiteralLimit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1;

// Rust's `ExtStyles::suggest_function` / `suggest_method`: the nearest extension function written in
// `style` within three edits, as help text.
[[nodiscard]] auto SuggestExtensionCall(const std::string_view name, const ExtensionCallStyle style)
    -> std::optional<std::string>
{
    constexpr std::size_t MaximumDistance = 3;
    auto names = ExtensionCalls | std::views::filter([style](const ExtensionCall& call) { return call.style == style; }) |
                       std::views::transform(&ExtensionCall::name);
    const std::optional<std::string> nearest = FuzzySearch(name, names, MaximumDistance);
    if (!nearest) { return std::nullopt; }
    return std::format("did you mean `{}`?", *nearest);
}

} // namespace

// ---- Construction and cursor ----

Parser::Parser(SharedSourceText source) : source_{std::move(source)}, tokens_{Tokenize(source_->text)}
{
    if (IsLexicalError(tokens_.front().kind))
    {
        ReportSyntax(DiagnosticCode::InvalidToken, tokens_.front().span,
                     std::format("{}", Describe(tokens_.front().kind)));
    }
}

auto Parser::Peek(const std::size_t offset) const noexcept -> const Token&
{
    if (stopped_) { return tokens_.back(); }
    return tokens_[std::min(position_ + offset, tokens_.size() - 1)];
}

auto Parser::Advance() -> void
{
    if (stopped_ || tokens_[position_].kind == TokenKind::EndOfInput) { return; }
    previousEnd_ = tokens_[position_].span.end;
    ++position_;
    const Token& next = tokens_[position_];
    if (IsLexicalError(next.kind)) { ReportSyntax(DiagnosticCode::InvalidToken, next.span, std::string{Describe(next.kind)}); }
}

auto Parser::Accept(const TokenKind kind) -> bool
{
    if (!At(kind)) { return false; }
    Advance();
    return true;
}

auto Parser::Expect(const TokenKind kind) -> bool
{
    if (Accept(kind)) { return true; }
    const Token& found = Current();
    if (found.kind == TokenKind::EndOfInput)
    {
        ReportSyntax(DiagnosticCode::UnexpectedEndOfInput, found.span,
                     std::format("expected `{}`, found end of input", Describe(kind)));
    }
    else
    {
        ReportSyntax(DiagnosticCode::UnexpectedToken, found.span,
                     std::format("expected `{}`, found `{}`", Describe(kind), TextOf(found)));
    }
    return false;
}

auto Parser::SpanFrom(const std::uint32_t begin) const noexcept -> SourceSpan
{
    return {begin, std::max(begin, previousEnd_)};
}

auto Parser::ExpectEndOfInput() -> bool
{
    if (At(TokenKind::EndOfInput)) { return true; }
    ReportSyntax(DiagnosticCode::ExtraToken, Current().span, std::format("unexpected `{}`", TextOf(Current())));
    return false;
}

// ---- Reporting ----

auto Parser::ReportSyntax(const DiagnosticCode code, const SourceSpan span, std::string message) -> void
{
    if (stopped_) { return; }
    syntaxDiagnostics_.push_back(Diagnostic{code, Severity::Error, std::move(message), std::nullopt, span, {}, source_});
    stopped_ = true;
}

auto Parser::ReportUnexpected() -> void
{
    const Token& found = Current();
    if (found.kind == TokenKind::EndOfInput)
    {
        ReportSyntax(DiagnosticCode::UnexpectedEndOfInput, found.span, "unexpected end of input");
    }
    else
    {
        ReportSyntax(DiagnosticCode::UnexpectedToken, found.span, std::format("unexpected `{}`", TextOf(found)));
    }
}

auto Parser::Report(const DiagnosticCode code, const SourceSpan span, std::string message,
                    std::optional<std::string> help) -> void
{
    if (stopped_) { return; }
    conversionDiagnostics_.push_back(
        Diagnostic{code, Severity::Error, std::move(message), std::move(help), span, {}, source_});
}

auto Parser::TakeDiagnostics() -> std::vector<Diagnostic>
{
    std::vector<Diagnostic> result =
        syntaxDiagnostics_.empty() ? std::move(conversionDiagnostics_) : std::move(syntaxDiagnostics_);
    syntaxDiagnostics_.clear();
    conversionDiagnostics_.clear();
    return result;
}

auto Parser::ExpressionOperand(const ExpressionIndex index, const SourceSpan span) -> Operand
{
    Operand operand;
    operand.kind = OperandKind::Expression;
    operand.expression = index;
    operand.span = span;
    return operand;
}

template <typename Element>
auto Parser::ParseCommaList(const TokenKind closing, Element element) -> void
{
    while (!At(closing) && !At(TokenKind::EndOfInput))
    {
        element();
        if (!Accept(TokenKind::Comma)) { break; }
    }
    Expect(closing);
}

// ---- Entry points ----

auto Parser::ParseExpression() -> std::optional<ExpressionIndex>
{
    return Materialize(ParseExpressionOperand());
}

auto Parser::ParseEntityUidLiteral() -> std::optional<EntityUid>
{
    const std::uint32_t begin = Current().span.begin;
    std::vector<Token> components;
    while (true)
    {
        if (!IsAnyIdentifier(Current().kind))
        {
            ReportUnexpected();
            return std::nullopt;
        }
        components.push_back(Current());
        Advance();
        if (!Expect(TokenKind::DoubleColon)) { return std::nullopt; }
        if (At(TokenKind::String)) { break; }
        if (At(TokenKind::LeftBrace))
        {
            ParseReferenceInitializers(begin);
            return std::nullopt;
        }
    }
    const Token idToken = Current();
    Advance();
    const std::optional<Name> type = ResolveName(components, SpanFrom(begin));
    std::optional<std::string> id = UnescapeToken(idToken);
    if (!type || !id) { return std::nullopt; }
    return EntityUid{EntityType{*type}, Eid{std::move(*id)}};
}

// ---- Policies ----

// Policy := {Annotation} AnyIdent '(' Comma<VariableDef> ')' {Cond} ';', with the checks of Rust's
// `to_policy_template` (`cst_to_ast.rs:257`).
auto Parser::ParsePolicy(PolicyId id, const PolicyText text) -> std::shared_ptr<const Template>
{
    const std::uint32_t begin = Current().span.begin;
    TemplateDefinition definition{.id = std::move(id)};
    bool valid = true;

    while (At(TokenKind::At))
    {
        if (!ParseAnnotation(definition.annotations)) { valid = false; }
    }
    if (!IsAnyIdentifier(Current().kind))
    {
        ReportUnexpected();
        return nullptr;
    }
    const Token effect = Current();
    Advance();
    if (effect.kind == TokenKind::Permit) { definition.effect = Effect::Permit; }
    else if (effect.kind == TokenKind::Forbid) { definition.effect = Effect::Forbid; }
    else
    {
        Report(DiagnosticCode::InvalidEffect, effect.span,
               std::format("`{}` is not an effect; a policy begins with `permit` or `forbid`", TextOf(effect)));
        valid = false;
    }

    if (!Expect(TokenKind::LeftParenthesis)) { return nullptr; }
    Scope scope;
    {
        ExpressionArena scratch;
        UseArena(scratch);
        ParseCommaList(TokenKind::RightParenthesis, [&] { ParseScopeElement(scope, scratch); });
        UseArena(definition.arena);
    }
    if (stopped_) { return nullptr; }
    constexpr std::array scopeVariables{"principal", "action", "resource"};
    for (std::size_t missing = scope.count; missing < scopeVariables.size(); ++missing)
    {
        Report(DiagnosticCode::MissingScopeVariable, SpanFrom(effect.span.begin),
               std::format("the policy scope has no `{}`", scopeVariables[missing]),
               "a scope lists `principal`, `action` and `resource`, in that order");
        valid = false;
    }
    valid = valid && scope.valid;

    std::vector<ExpressionIndex> conditions;
    while (IsAnyIdentifier(Current().kind))
    {
        if (const std::optional<ExpressionIndex> condition = ParseCondition(valid)) { conditions.push_back(*condition); }
    }
    if (!Expect(TokenKind::Semicolon) || !valid) { return nullptr; }
    const SourceSpan span = SpanFrom(begin);

    // A right fold, `c1 && (c2 && c3)` (`cst_to_ast.rs:2283`).
    if (!conditions.empty())
    {
        ExpressionIndex combined = conditions.back();
        for (std::size_t index = conditions.size() - 1; index-- > 0;)
        {
            combined = Builder().And(conditions[index], combined, span);
        }
        definition.nonScopeCondition = combined;
    }
    definition.principal = std::move(scope.principal);
    definition.action = std::move(scope.action);
    definition.resource = std::move(scope.resource);
    definition.source = source_;
    definition.span = text == PolicyText::WholeInput
                          ? SourceSpan{0, static_cast<std::uint32_t>(source_->text.size())}
                          : span;
    return Template::Create(std::move(definition));
}

auto Parser::ParsePolicies() -> std::vector<std::shared_ptr<const Template>>
{
    std::vector<std::shared_ptr<const Template>> bodies;
    std::size_t count = 0;
    while (!At(TokenKind::EndOfInput))
    {
        std::shared_ptr<const Template> body = ParsePolicy(PolicyId{std::format("policy{}", count++)}, PolicyText::Policy);
        if (stopped_)
        {
            if (!Synchronize()) { break; }
            continue;
        }
        if (body) { bodies.push_back(std::move(body)); }
    }
    return bodies;
}

auto Parser::Synchronize() -> bool
{
    if (IsLexicalError(tokens_[position_].kind)) { return false; }
    Resume();
    while (!At(TokenKind::EndOfInput))
    {
        const bool end = At(TokenKind::Semicolon);
        Advance();
        if (stopped_) { return false; }
        if (end) { return true; }
    }
    return true;
}

// Annotation := '@' AnyIdent ['(' Str ')']. Any identifier is a key, keywords included; each key
// appears once (`cst_to_ast.rs` `get_ast_annotations`).
auto Parser::ParseAnnotation(Annotations& annotations) -> bool
{
    const std::uint32_t begin = Current().span.begin;
    Advance();
    if (!IsAnyIdentifier(Current().kind))
    {
        ReportUnexpected();
        return false;
    }
    const Token key = Current();
    Advance();
    std::optional<std::string> value;
    if (Accept(TokenKind::LeftParenthesis))
    {
        if (!At(TokenKind::String))
        {
            ReportUnexpected();
            return false;
        }
        const Token valueToken = Current();
        Advance();
        if (!Expect(TokenKind::RightParenthesis)) { return false; }
        value = UnescapeToken(valueToken);
        if (!value) { return false; }
    }
    if (!annotations.Insert(Symbol::Intern(TextOf(key)), std::move(value)))
    {
        Report(DiagnosticCode::DuplicateAnnotation, SpanFrom(begin),
               std::format("the annotation `@{}` appears more than once", TextOf(key)));
        return false;
    }
    return true;
}

// VariableDef := AnyIdent [':' Name] ['is' Add] [RelOp Expr]. The right-hand side is read as any
// expression and then must be an entity uid, the matching slot or (for `action in`) a list of uids,
// possibly in parentheses (`to_principal_or_resource_constraint`, `to_action_constraint`,
// `cst_to_ast.rs:871`, and `cst_to_ast/to_ref_or_refs.rs`).
auto Parser::ParseScopeElement(Scope& scope, ExpressionArena& scratch) -> void
{
    const std::size_t position = scope.count++;
    const std::uint32_t begin = Current().span.begin;
    if (!IsAnyIdentifier(Current().kind))
    {
        ReportUnexpected();
        return;
    }
    const Token variableToken = Current();
    Advance();

    std::optional<SourceSpan> typeConstraint;
    if (At(TokenKind::Colon))
    {
        const std::uint32_t constraintBegin = Current().span.begin;
        Advance();
        ParseTypeConstraintName();
        typeConstraint = SpanFrom(constraintBegin);
    }
    bool hasIs = false;
    std::optional<EntityType> isType;
    if (Accept(TokenKind::Is))
    {
        hasIs = true;
        isType = EntityTypeOf(ParseAdd());
    }
    std::optional<TokenKind> relation;
    Operand target;
    if (IsRelationOperator(Current().kind))
    {
        relation = Current().kind;
        Advance();
        target = ParseExpressionOperand();
    }
    if (stopped_) { return; }

    const SourceSpan span = SpanFrom(begin);
    const auto fail = [&](const DiagnosticCode code, std::string message, std::optional<std::string> help = std::nullopt) {
        Report(code, span, std::move(message), std::move(help));
        scope.valid = false;
    };
    constexpr std::array expectedVariables{Variable::Principal, Variable::Action, Variable::Resource};
    if (position >= expectedVariables.size())
    {
        fail(DiagnosticCode::ExtraScopeElement, "a policy scope has only `principal`, `action` and `resource`",
             "move this constraint into a `when` clause");
        return;
    }
    const Variable expected = expectedVariables[position];
    const std::optional<Variable> variable = VariableOf(variableToken.kind);
    if (!variable || *variable == Variable::Context)
    {
        fail(DiagnosticCode::InvalidScopeVariable,
             std::format("`{}` is not a scope variable; the scope lists `principal`, `action` and `resource`",
                         TextOf(variableToken)));
        return;
    }
    if (*variable != expected)
    {
        fail(DiagnosticCode::IncorrectVariable,
             std::format("expected `{}` here, found `{}`; the scope lists `principal`, `action` and `resource`, "
                         "in that order",
                         ToString(expected), ToString(*variable)));
        return;
    }
    if (typeConstraint)
    {
        Report(DiagnosticCode::TypeConstraints, *typeConstraint, "type constraints are not supported",
               std::format("write `{} is Type`", ToString(expected)));
        scope.valid = false;
        return;
    }
    if (hasIs && !isType)
    {
        scope.valid = false;
        return;
    }
    // `principal in E is T` reads as `principal in (E is T)`.
    const bool invertedIs = relation == TokenKind::In && target.kind == OperandKind::Expression &&
                            std::holds_alternative<node::Is>(scratch.Node(target.expression));

    if (expected == Variable::Action)
    {
        if (hasIs || invertedIs)
        {
            fail(DiagnosticCode::IsInActionScope, "`is` is not allowed on `action` in the scope");
            return;
        }
        if (!relation) { return; }
        if (*relation == TokenKind::Equal)
        {
            fail(DiagnosticCode::InvalidSingleEquals, "`=` is not an operator", "use `==`");
            return;
        }
        if (*relation != TokenKind::In && *relation != TokenKind::EqualEqual)
        {
            fail(DiagnosticCode::InvalidActionScopeOperator,
                 std::format("`{}` is not allowed on `action` in the scope; use `==` or `in`", Describe(*relation)));
            return;
        }
        const bool in = *relation == TokenKind::In;
        std::optional<Reference> reference =
            ConvertReference(target, scratch, expected, in ? ReferenceKind::EntityOrList : ReferenceKind::Entity);
        if (!reference)
        {
            scope.valid = false;
            return;
        }
        // Every action in the scope has an entity type named `Action`, in any namespace
        // (`ActionConstraint::check_action_types`).
        std::string nonActions;
        for (const EntityUid& uid : reference->uids)
        {
            if (uid.IsAction()) { continue; }
            if (!nonActions.empty()) { nonActions += ", "; }
            nonActions += uid.ToString();
        }
        if (!nonActions.empty())
        {
            fail(DiagnosticCode::InvalidActionType,
                 std::format("an action in the scope must have an entity type named `Action`; {} {} not", nonActions,
                             nonActions.find(", ") == std::string::npos ? "is" : "are"));
            return;
        }
        if (in) { scope.action = ActionIn{std::move(reference->uids), reference->isList}; }
        else { scope.action = ActionEqual{std::move(reference->uids.front())}; }
        return;
    }

    PrincipalOrResourceConstraint constraint{ScopeAny{}};
    if (relation)
    {
        if (invertedIs)
        {
            fail(DiagnosticCode::InvertedIsIn, "`is` comes before `in` in the scope",
                 std::format("write `{} is Type in ...`", ToString(expected)));
            return;
        }
        std::optional<Reference> reference = ConvertReference(target, scratch, expected, ReferenceKind::EntityOrSlot);
        if (!reference)
        {
            scope.valid = false;
            return;
        }
        EntityReference entity = reference->isSlot ? EntityReference{SlotReference{reference->slot}}
                                                   : EntityReference{std::move(reference->uids.front())};
        switch (*relation)
        {
        case TokenKind::EqualEqual:
            if (hasIs)
            {
                fail(DiagnosticCode::IsWithEquals, "`is` cannot be combined with `==` in the scope",
                     "`==` alone already fixes the entity, and with it the type");
                return;
            }
            constraint = ScopeEqual{std::move(entity)};
            break;
        case TokenKind::In:
            if (hasIs) { constraint = ScopeIsIn{*isType, std::move(entity)}; }
            else { constraint = ScopeIn{std::move(entity)}; }
            break;
        case TokenKind::Equal: fail(DiagnosticCode::InvalidSingleEquals, "`=` is not an operator", "use `==`"); return;
        default:
            fail(DiagnosticCode::InvalidScopeOperator,
                 std::format("`{}` is not allowed in the scope; use `==`, `in` or `is`", Describe(*relation)));
            return;
        }
    }
    else if (hasIs) { constraint = ScopeIs{*isType}; }
    if (expected == Variable::Principal) { scope.principal.constraint = std::move(constraint); }
    else { scope.resource.constraint = std::move(constraint); }
}

// Name := CommonIdent | AnyIdent {'::' AnyIdent}, after `principal:`. The type constraint is
// refused whatever it names, so only the syntax is read.
auto Parser::ParseTypeConstraintName() -> void
{
    if (!IsAnyIdentifier(Current().kind))
    {
        ReportUnexpected();
        return;
    }
    const Token first = Current();
    Advance();
    if (!At(TokenKind::DoubleColon))
    {
        if (!IsCommonIdentifier(first.kind))
        {
            ReportSyntax(DiagnosticCode::UnexpectedToken, first.span, std::format("unexpected `{}`", TextOf(first)));
        }
        return;
    }
    while (Accept(TokenKind::DoubleColon))
    {
        if (!IsAnyIdentifier(Current().kind))
        {
            ReportUnexpected();
            return;
        }
        Advance();
    }
}

// Cond := AnyIdent '{' Expr '}' | AnyIdent '{' '}', converted as Rust's `Cond::to_expr`
// (`cst_to_ast.rs:1041`): `unless { e }` is `!e`, and no slot may appear in a condition.
auto Parser::ParseCondition(bool& valid) -> std::optional<ExpressionIndex>
{
    const std::uint32_t begin = Current().span.begin;
    const Token keyword = Current();
    Advance();
    const bool keywordValid = keyword.kind == TokenKind::When || keyword.kind == TokenKind::Unless;
    if (!keywordValid)
    {
        Report(DiagnosticCode::InvalidCondition, keyword.span,
               std::format("`{}` is not a condition; a condition begins with `when` or `unless`", TextOf(keyword)));
        valid = false;
    }
    if (!Expect(TokenKind::LeftBrace)) { return std::nullopt; }
    if (Accept(TokenKind::RightBrace))
    {
        Report(DiagnosticCode::EmptyClause, SpanFrom(begin), std::format("the `{}` clause is empty", TextOf(keyword)));
        valid = false;
        return std::nullopt;
    }
    const std::size_t firstNode = arena_->Size();
    const std::optional<ExpressionIndex> condition = ParseExpression();
    if (!Expect(TokenKind::RightBrace) || !condition)
    {
        valid = false;
        return std::nullopt;
    }
    const SourceSpan span = SpanFrom(begin);
    // The condition's nodes are the ones built while it was read.
    for (std::size_t index = firstNode; index < arena_->Size(); ++index)
    {
        const auto position = static_cast<ExpressionIndex>(index);
        if (const auto* slot = std::get_if<node::Slot>(&arena_->Node(position)))
        {
            Report(DiagnosticCode::SlotsInConditionClause, arena_->Span(position).value_or(span),
                   std::format("`{}` appears in a `{}` clause; slots may appear only in the scope",
                               ToString(slot->slot), TextOf(keyword)));
            valid = false;
        }
    }
    if (!keywordValid) { return std::nullopt; }
    if (keyword.kind == TokenKind::Unless) { return Builder().Not(*condition, span); }
    return condition;
}

auto Parser::EntityTypeOf(const Operand& operand) -> std::optional<EntityType>
{
    switch (operand.kind)
    {
    case OperandKind::Error: return std::nullopt;
    case OperandKind::Name: return EntityType{*operand.name};
    case OperandKind::Variable:
        if (auto name = Name::Parse(ToString(operand.variable))) { return EntityType{*name}; }
        return std::nullopt;
    default:
        Report(DiagnosticCode::InvalidIsType, operand.span, "the right side of `is` must be an entity type name");
        return std::nullopt;
    }
}

auto Parser::ConvertReference(const Operand& operand, const ExpressionArena& arena, const Variable variable,
                              const ReferenceKind kind) -> std::optional<Reference>
{
    const std::string_view written = std::string_view{source_->text}.substr(operand.span.begin, operand.span.Length());
    switch (operand.kind)
    {
    case OperandKind::Error: return std::nullopt;
    case OperandKind::Expression: return ConvertEntityNode(arena, operand.expression, variable, kind, operand.span);
    case OperandKind::Variable:
    case OperandKind::Name:
        Report(DiagnosticCode::UnexpectedConstruct, operand.span,
               std::format("expected {}, found the name `{}`", ExpectationOf(kind), written),
               "an entity uid is written `Type::\"id\"`");
        return std::nullopt;
    case OperandKind::String:
    case OperandKind::Bool:
        Report(DiagnosticCode::UnexpectedConstruct, operand.span,
               std::format("expected {}, found the literal `{}`", ExpectationOf(kind), written),
               "an entity uid is written `Type::\"id\"`");
        return std::nullopt;
    }
    return std::nullopt;
}

auto Parser::ConvertEntityNode(const ExpressionArena& arena, const ExpressionIndex index, const Variable variable,
                               const ReferenceKind kind, const SourceSpan span) -> std::optional<Reference>
{
    const ExpressionNode& expression = arena.Node(index);
    if (const auto* folded = std::get_if<node::Folded>(&expression))
    {
        return ConvertEntityNode(arena, folded->written, variable, kind, span);
    }
    if (const auto* literal = std::get_if<node::Literal>(&expression); literal && literal->value.Kind() == ValueKind::Entity)
    {
        return Reference{{literal->value.AsEntity()}, std::nullopt, false, false};
    }
    if (const auto* slot = std::get_if<node::Slot>(&expression))
    {
        if (kind != ReferenceKind::EntityOrSlot)
        {
            Report(DiagnosticCode::WrongEntityArgument, span,
                   std::format("expected {}, found the slot `{}`", ExpectationOf(kind), ToString(slot->slot)));
            return std::nullopt;
        }
        const bool matches = (slot->slot == SlotId::Principal) == (variable == Variable::Principal);
        if (!matches)
        {
            Report(DiagnosticCode::UnexpectedConstruct, span,
                   std::format("expected {}, found `{}` instead of `?{}`", ExpectationOf(kind), ToString(slot->slot),
                               ToString(variable)));
            return std::nullopt;
        }
        return Reference{{}, arena.Span(index), true, false};
    }
    if (const auto* set = std::get_if<node::SetLiteral>(&expression))
    {
        if (kind != ReferenceKind::EntityOrList)
        {
            Report(DiagnosticCode::WrongEntityArgument, span, std::format("expected {}, found a set", ExpectationOf(kind)));
            return std::nullopt;
        }
        Reference reference{{}, std::nullopt, false, true};
        bool valid = true;
        for (const ExpressionIndex element : arena.Elements(set->elements))
        {
            const std::optional<Reference> converted =
                ConvertEntityNode(arena, element, variable, ReferenceKind::Entity, arena.Span(element).value_or(span));
            if (converted) { reference.uids.push_back(converted->uids.front()); }
            else { valid = false; }
        }
        if (!valid) { return std::nullopt; }
        return reference;
    }
    Report(DiagnosticCode::UnexpectedConstruct, span,
           std::format("expected {}, found an expression", ExpectationOf(kind)),
           "the scope compares with an entity uid; move other conditions into a `when` clause");
    return std::nullopt;
}

auto Parser::ExpectationOf(const ReferenceKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case ReferenceKind::EntityOrSlot: return "an entity uid or the matching slot";
    case ReferenceKind::Entity: return "an entity uid";
    case ReferenceKind::EntityOrList: return "an entity uid or a list of entity uids";
    }
    return "an entity uid";
}

// ---- Expressions ----

// Expr := Or | 'if' Expr 'then' Expr 'else' Expr
auto Parser::ParseExpressionOperand() -> Operand
{
    if (depth_ >= MaximumNestingDepth) { return ReportNestingTooDeep(); }
    ++depth_;
    // `if::...` is a path, not a conditional; it is refused later, as a reserved word in a name.
    Operand result = At(TokenKind::If) && Peek(1).kind != TokenKind::DoubleColon ? ParseIf() : ParseOr();
    --depth_;
    return result;
}

auto Parser::ReportNestingTooDeep() -> Operand
{
    ReportSyntax(DiagnosticCode::NestingTooDeep, Current().span,
                 std::format("expressions are nested more than {} deep", MaximumNestingDepth));
    return ErrorOperand();
}

auto Parser::ParseIf() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Advance();
    const std::optional<ExpressionIndex> condition = Materialize(ParseExpressionOperand());
    if (!Expect(TokenKind::Then)) { return ErrorOperand(); }
    const std::optional<ExpressionIndex> thenBranch = Materialize(ParseExpressionOperand());
    if (!Expect(TokenKind::Else)) { return ErrorOperand(); }
    const std::optional<ExpressionIndex> elseBranch = Materialize(ParseExpressionOperand());
    if (!condition || !thenBranch || !elseBranch) { return ErrorOperand(); }
    const SourceSpan span = SpanFrom(begin);
    return ExpressionOperand(Builder().If(*condition, *thenBranch, *elseBranch, span), span);
}

// Or := And {'||' And}, folded to the left.
auto Parser::ParseOr() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Operand first = ParseAnd();
    if (!At(TokenKind::OrOr)) { return first; }
    return ParseOrAfter(first, begin);
}

auto Parser::ParseOrAfter(const Operand& first, const std::uint32_t begin) -> Operand
{
    std::optional<ExpressionIndex> accumulated = Materialize(first);
    while (Accept(TokenKind::OrOr))
    {
        const std::optional<ExpressionIndex> right = Materialize(ParseAnd());
        accumulated = accumulated && right ? std::optional{Builder().Or(*accumulated, *right, SpanFrom(begin))}
                                           : std::nullopt;
    }
    if (!accumulated) { return ErrorOperand(); }
    return ExpressionOperand(*accumulated, SpanFrom(begin));
}

// And := Relation {'&&' Relation}, folded to the left.
auto Parser::ParseAnd() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Operand first = ParseRelation();
    if (!At(TokenKind::AndAnd)) { return first; }
    return ParseAndAfter(first, begin);
}

auto Parser::ParseAndAfter(const Operand& first, const std::uint32_t begin) -> Operand
{
    std::optional<ExpressionIndex> accumulated = Materialize(first);
    while (Accept(TokenKind::AndAnd))
    {
        const std::optional<ExpressionIndex> right = Materialize(ParseRelation());
        accumulated = accumulated && right ? std::optional{Builder().And(*accumulated, *right, SpanFrom(begin))}
                                           : std::nullopt;
    }
    if (!accumulated) { return ErrorOperand(); }
    return ExpressionOperand(*accumulated, SpanFrom(begin));
}

// Relation := Add {RelOp Add} | Add 'has' Add | Add 'has' 'if' {MemAccess} | Add 'like' Add
//           | Add 'is' Add ['in' Add]
// The grammar accepts a chain of relational operators; the conversion refuses more than one
// (`a < b < c` is AmbiguousOperators). After `has`, `like` and `is` the relation ends.
auto Parser::ParseRelation() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Operand first = ParseAdd();
    switch (Current().kind)
    {
    case TokenKind::Has: return ParseHas(first, begin);
    case TokenKind::Like: return ParseLike(first, begin);
    case TokenKind::Is: return ParseIs(first, begin);
    default: return IsRelationOperator(Current().kind) ? ParseComparison(first, begin) : first;
    }
}

auto Parser::ParseComparison(const Operand& first, const std::uint32_t begin) -> Operand
{
    const TokenKind operation = Current().kind;
    Advance();
    std::optional<ExpressionIndex> left = Materialize(first);
    std::optional<ExpressionIndex> right = Materialize(ParseAdd());
    bool ambiguous = false;
    while (IsRelationOperator(Current().kind))
    {
        ambiguous = true;
        Advance();
        static_cast<void>(Materialize(ParseAdd()));
    }
    const SourceSpan span = SpanFrom(begin);
    if (ambiguous)
    {
        Report(DiagnosticCode::AmbiguousOperators, span, "relational operators cannot be chained",
               "add parentheses to say which comparison comes first");
        return ErrorOperand();
    }
    if (!left || !right) { return ErrorOperand(); }
    const std::optional<ExpressionIndex> relation = BuildRelation(*left, operation, *right, span);
    if (!relation) { return ErrorOperand(); }
    return ExpressionOperand(*relation, span);
}

auto Parser::ParseLike(const Operand& first, const std::uint32_t begin) -> Operand
{
    Advance();
    const std::optional<ExpressionIndex> target = Materialize(first);
    const Operand pattern = ParseAdd();
    std::optional<std::vector<std::string>> literals;
    if (pattern.kind == OperandKind::String)
    {
        const StringContent content = ContentOf(pattern.token, source_->text);
        auto unescaped = UnescapePattern(content.text, content.offset);
        if (unescaped) { literals = std::move(*unescaped); }
        else
        {
            Report(DiagnosticCode::InvalidEscape, unescaped.error().span,
                   std::string{Describe(unescaped.error().kind)});
        }
    }
    else if (pattern.kind != OperandKind::Error)
    {
        Report(DiagnosticCode::InvalidPattern, pattern.span, "the pattern after `like` must be a string literal");
    }
    if (!target || !literals) { return ErrorOperand(); }
    const std::vector<std::string_view> views(literals->begin(), literals->end());
    const SourceSpan span = SpanFrom(begin);
    return ExpressionOperand(Builder().Like(*target, views, span), span);
}

auto Parser::ParseIs(const Operand& first, const std::uint32_t begin) -> Operand
{
    Advance();
    const std::optional<ExpressionIndex> target = Materialize(first);
    const std::optional<EntityType> type = EntityTypeOf(ParseAdd());
    if (Accept(TokenKind::In))
    {
        const std::optional<ExpressionIndex> container = Materialize(ParseAdd());
        if (!target || !type || !container) { return ErrorOperand(); }
        const SourceSpan span = SpanFrom(begin);
        return ExpressionOperand(Builder().IsIn(*target, *type, *container, span), span);
    }
    if (!target || !type) { return ErrorOperand(); }
    const SourceSpan span = SpanFrom(begin);
    return ExpressionOperand(Builder().Is(*target, *type, span), span);
}

// The right side of `has` is an Add in the grammar; the conversion takes only a string literal,
// or an identifier or variable followed by `.identifier` fields (`cst_to_ast.rs` `to_has_rhs`).
// Anything else is read in full for its syntax and refused.
auto Parser::ParseHas(const Operand& target, const std::uint32_t begin) -> Operand
{
    Advance();
    const std::optional<ExpressionIndex> object = Materialize(target);
    const std::uint32_t rightBegin = Current().span.begin;

    if (At(TokenKind::If))
    {
        const SourceSpan ifSpan = Current().span;
        Advance();
        std::vector<Access> accesses;
        ParseAccesses(accesses);
        Report(DiagnosticCode::ReservedIdentifier, ifSpan, "`if` is a reserved word and cannot be an attribute name");
        return ErrorOperand();
    }

    std::vector<std::string> path;
    bool valid = true;
    bool shaped = false; // The tokens read so far are a string or a word with fields.

    if (At(TokenKind::String) && !IsAccessStart(Peek(1).kind))
    {
        const Token token = Current();
        Advance();
        if (auto text = UnescapeToken(token)) { path.push_back(std::move(*text)); }
        else { valid = false; }
        shaped = true;
    }
    else if (IsAnyIdentifier(Current().kind) && Peek(1).kind != TokenKind::DoubleColon)
    {
        const Token word = Current();
        Advance();
        if (word.kind == TokenKind::True || word.kind == TokenKind::False)
        {
            Report(DiagnosticCode::ReservedIdentifier, word.span,
                   std::format("`{}` is a reserved word and cannot be an attribute name", TextOf(word)));
            valid = false;
        }
        else if (VariableOf(word.kind)) { path.emplace_back(TextOf(word)); }
        else if (ResolveName(std::span{&word, 1}, word.span)) { path.emplace_back(TextOf(word)); }
        else { valid = false; }

        while (IsAccessStart(Current().kind))
        {
            if (At(TokenKind::Dot) && IsAnyIdentifier(Peek(1).kind))
            {
                Advance();
                const Token field = Current();
                Advance();
                if (CheckFieldName(field)) { path.emplace_back(TextOf(field)); }
                else { valid = false; }
                continue;
            }
            Access access;
            const std::uint32_t accessBegin = Current().span.begin;
            ParseAccess(access);
            if (valid)
            {
                Report(DiagnosticCode::InvalidHasRightHandSide, SpanFrom(accessBegin),
                       "the right side of `has` may only use `.` to name nested attributes");
            }
            valid = false;
        }
        shaped = true;
    }

    if (!shaped || IsMultiplyOperator(Current().kind) || IsAddOperator(Current().kind))
    {
        const Operand rest = shaped ? ParseAddAfter(ParseMultiplyAfter(ErrorOperand(), rightBegin), rightBegin) : ParseAdd();
        static_cast<void>(rest);
        Report(DiagnosticCode::InvalidHasRightHandSide, SpanFrom(rightBegin),
               "the right side of `has` must be an attribute name, a string literal, or a path like `a.b.c`");
        return ErrorOperand();
    }

    if (!object || !valid) { return ErrorOperand(); }
    const std::vector<std::string_view> views(path.begin(), path.end());
    const SourceSpan span = SpanFrom(begin);
    return ExpressionOperand(Builder().HasAttribute(*object, views, span), span);
}

// Add := Mult {('+' | '-') Mult}, folded to the left.
auto Parser::ParseAdd() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    return ParseAddAfter(ParseMultiply(), begin);
}

auto Parser::ParseAddAfter(Operand first, const std::uint32_t begin) -> Operand
{
    if (!IsAddOperator(Current().kind)) { return first; }
    std::optional<ExpressionIndex> accumulated = Materialize(first);
    while (IsAddOperator(Current().kind))
    {
        const BinaryOperator operation = At(TokenKind::Plus) ? BinaryOperator::Add : BinaryOperator::Subtract;
        Advance();
        const std::optional<ExpressionIndex> right = Materialize(ParseMultiply());
        accumulated = accumulated && right
                          ? std::optional{Builder().Binary(operation, *accumulated, *right, SpanFrom(begin))}
                          : std::nullopt;
    }
    if (!accumulated) { return ErrorOperand(); }
    return ExpressionOperand(*accumulated, SpanFrom(begin));
}

// Mult := Unary {('*' | '/' | '%') Unary}, folded to the left. `/` and `%` are in the grammar
// only to be refused.
auto Parser::ParseMultiply() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    return ParseMultiplyAfter(ParseUnary(), begin);
}

auto Parser::ParseMultiplyAfter(Operand first, const std::uint32_t begin) -> Operand
{
    if (!IsMultiplyOperator(Current().kind)) { return first; }
    std::optional<ExpressionIndex> accumulated = Materialize(first);
    while (IsMultiplyOperator(Current().kind))
    {
        const Token operation = Current();
        Advance();
        std::optional<ExpressionIndex> right = Materialize(ParseUnary());
        if (operation.kind == TokenKind::Slash)
        {
            Report(DiagnosticCode::UnsupportedDivision, operation.span, "division is not supported");
            right.reset();
        }
        else if (operation.kind == TokenKind::Percent)
        {
            Report(DiagnosticCode::UnsupportedModulo, operation.span, "remainder is not supported");
            right.reset();
        }
        accumulated = accumulated && right
                          ? std::optional{Builder().Binary(BinaryOperator::Multiply, *accumulated, *right,
                                                           SpanFrom(begin))}
                          : std::nullopt;
    }
    if (!accumulated) { return ErrorOperand(); }
    return ExpressionOperand(*accumulated, SpanFrom(begin));
}

// Unary := ['!' {'!'} | '-' {'-'}] Member, at most four operators (`cst_to_ast.rs`, `NegOp`).
// A `-` directly before a number literal with no access after it makes a negative literal, which
// is how `-9223372036854775808` is written; any further `-` negates that literal.
auto Parser::ParseUnary() -> Operand
{
    if (!At(TokenKind::Bang) && !At(TokenKind::Minus)) { return ParseMember(); }
    return ParseUnaryOperators();
}

auto Parser::ParseUnaryOperators() -> Operand
{
    constexpr std::size_t operatorLimit = 4;
    const std::uint32_t begin = Current().span.begin;
    const TokenKind operation = Current().kind;

    std::size_t count = 0;
    while (At(operation))
    {
        Advance();
        ++count;
    }
    const auto tooMany = [&] {
        Report(DiagnosticCode::UnaryOperatorLimit, SpanFrom(begin),
               std::format("at most {} `{}` operators may be applied in a row", operatorLimit, Describe(operation)));
    };

    if (operation == TokenKind::Bang)
    {
        const Operand operand = ParseMember();
        if (count > operatorLimit)
        {
            tooMany();
            return ErrorOperand();
        }
        std::optional<ExpressionIndex> result = Materialize(operand);
        if (!result) { return ErrorOperand(); }
        for (std::size_t index = 0; index < count; ++index) { result = Builder().Not(*result, SpanFrom(begin)); }
        return ExpressionOperand(*result, SpanFrom(begin));
    }

    std::optional<ExpressionIndex> result;
    std::size_t negations = count;
    if (At(TokenKind::Number) && !IsAccessStart(Peek(1).kind))
    {
        const Token number = Current();
        Advance();
        if (count > operatorLimit)
        {
            tooMany();
            return ErrorOperand();
        }
        const std::optional<std::uint64_t> magnitude = ParseNumberLiteral(TextOf(number));
        if (!magnitude)
        {
            ReportSyntax(DiagnosticCode::IntegerLiteralTooLarge, number.span, "integer literal does not fit in 64 bits");
            return ErrorOperand();
        }
        if (*magnitude > NegativeLiteralLimit)
        {
            Report(DiagnosticCode::IntegerLiteralTooLarge, SpanFrom(begin),
                   std::format("integer literal -{} is out of range", *magnitude));
            return ErrorOperand();
        }
        const std::int64_t value = *magnitude == NegativeLiteralLimit ? std::numeric_limits<std::int64_t>::min()
                                                                      : -static_cast<std::int64_t>(*magnitude);
        result = Builder().Long(value, number.span);
        --negations;
    }
    else
    {
        const Operand operand = ParseMember();
        if (count > operatorLimit)
        {
            tooMany();
            return ErrorOperand();
        }
        result = Materialize(operand);
    }
    if (!result) { return ErrorOperand(); }
    for (std::size_t index = 0; index < negations; ++index)
    {
        result = Builder().Unary(UnaryOperator::Negate, *result, SpanFrom(begin));
    }
    return ExpressionOperand(*result, SpanFrom(begin));
}

// Member := Primary {MemAccess}. What each access means depends on what comes before it
// (`cst_to_ast.rs`, `Member::to_expr_or_special`): after an extension function's name, a call;
// after a variable or any expression, a field, an index or a method call; anything else is
// refused.
auto Parser::ParseMember() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Operand primary = ParsePrimary();
    if (!IsAccessStart(Current().kind)) { return primary; }
    return ParseAccessChain(primary, begin);
}

auto Parser::ParseAccessChain(const Operand& primary, const std::uint32_t begin) -> Operand
{
    std::vector<Access> accesses;
    const bool accessesValid = ParseAccesses(accesses);
    if (primary.kind == OperandKind::Error || !accessesValid) { return ErrorOperand(); }

    const auto spanThrough = [&](const Access& access) { return SourceSpan{begin, access.span.end}; };
    std::size_t next = 0;
    std::optional<ExpressionIndex> head;
    if (primary.kind == OperandKind::Name)
    {
        const Access& first = accesses.front();
        const std::string name{primary.name->QualifiedText()};
        if (first.kind == AccessKind::Call)
        {
            head = BuildFunctionCall(*primary.name, first.arguments, spanThrough(first));
            next = 1;
        }
        else if (first.kind == AccessKind::Field && accesses.size() > 1 && accesses[1].kind == AccessKind::Call)
        {
            Report(DiagnosticCode::NoMethods, spanThrough(accesses[1]),
                   std::format("`{}` is not a value and has no methods; `{}.{}(...)` is not a call", name, name,
                               first.text));
            return ErrorOperand();
        }
        else if (first.kind == AccessKind::Field)
        {
            Report(DiagnosticCode::InvalidAccess, spanThrough(first),
                   std::format("`{}` is not a value and has no field `{}`", name, first.text));
            return ErrorOperand();
        }
        else
        {
            Report(DiagnosticCode::InvalidIndex, spanThrough(first),
                   std::format("`{}` is not a value and cannot be indexed", name));
            return ErrorOperand();
        }
    }
    else if (primary.kind == OperandKind::Variable && accesses.front().kind == AccessKind::Call)
    {
        Report(DiagnosticCode::VariableCall, spanThrough(accesses.front()),
               std::format("`{}` is a variable, not a function", ToString(primary.variable)));
        return ErrorOperand();
    }
    else { head = Materialize(primary); }

    while (head && next < accesses.size())
    {
        const Access& access = accesses[next];
        switch (access.kind)
        {
        case AccessKind::Call:
            Report(DiagnosticCode::ExpressionCall, spanThrough(access), "only extension functions can be called");
            return ErrorOperand();
        case AccessKind::Field:
            if (next + 1 < accesses.size() && accesses[next + 1].kind == AccessKind::Call)
            {
                head = BuildMethodCall(*head, access, accesses[next + 1].arguments, spanThrough(accesses[next + 1]));
                next += 2;
                continue;
            }
            [[fallthrough]];
        case AccessKind::Index:
            head = Builder().GetAttribute(*head, access.text, spanThrough(access));
            ++next;
            continue;
        }
    }
    if (!head) { return ErrorOperand(); }
    return ExpressionOperand(*head, SpanFrom(begin));
}

auto Parser::ParseAccesses(std::vector<Access>& accesses) -> bool
{
    bool valid = true;
    while (IsAccessStart(Current().kind))
    {
        Access access;
        if (!ParseAccess(access)) { valid = false; }
        accesses.push_back(std::move(access));
    }
    return valid;
}

// MemAccess := '.' AnyIdent | '(' Comma<Expr> ')' | '[' Expr ']'
auto Parser::ParseAccess(Access& access) -> bool
{
    const std::uint32_t begin = Current().span.begin;
    bool valid = true;
    if (Accept(TokenKind::Dot))
    {
        access.kind = AccessKind::Field;
        if (!IsAnyIdentifier(Current().kind))
        {
            ReportUnexpected();
            return false;
        }
        const Token field = Current();
        Advance();
        access.text = TextOf(field);
        valid = CheckFieldName(field);
    }
    else if (Accept(TokenKind::LeftParenthesis))
    {
        access.kind = AccessKind::Call;
        ParseCommaList(TokenKind::RightParenthesis, [&] {
            if (auto argument = Materialize(ParseExpressionOperand())) { access.arguments.push_back(*argument); }
            else { valid = false; }
        });
    }
    else
    {
        Advance();
        access.kind = AccessKind::Index;
        const Operand index = ParseExpressionOperand();
        Expect(TokenKind::RightBracket);
        if (index.kind == OperandKind::String)
        {
            if (auto text = UnescapeToken(index.token)) { access.text = std::move(*text); }
            else { valid = false; }
        }
        else
        {
            if (index.kind != OperandKind::Error)
            {
                Report(DiagnosticCode::NonStringIndex, index.span, "an index must be a string literal");
            }
            valid = false;
        }
    }
    access.span = SpanFrom(begin);
    return valid && !stopped_;
}

// Primary := Literal | Ref | Name | Slot | '(' Expr ')' | '[' Comma<Expr> ']' | '{' Comma<RecInit> '}'
auto Parser::ParsePrimary() -> Operand
{
    const Token token = Current();
    switch (token.kind)
    {
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::If:
        if (Peek(1).kind == TokenKind::DoubleColon) { return ParseNameOrReference(); }
        if (token.kind == TokenKind::If) { break; }
        {
            Advance();
            Operand operand;
            operand.kind = OperandKind::Bool;
            operand.boolean = token.kind == TokenKind::True;
            operand.span = token.span;
            return operand;
        }
    case TokenKind::Number: return ParseNumber();
    case TokenKind::String:
    {
        Advance();
        Operand operand;
        operand.kind = OperandKind::String;
        operand.token = token;
        operand.span = token.span;
        return operand;
    }
    case TokenKind::PrincipalSlot:
    case TokenKind::ResourceSlot:
        Advance();
        return ExpressionOperand(
            Builder().Slot(token.kind == TokenKind::PrincipalSlot ? SlotId::Principal : SlotId::Resource, token.span),
            token.span);
    case TokenKind::OtherSlot: return ParseOtherSlot();
    case TokenKind::LeftParenthesis: return ParseParenthesized();
    case TokenKind::LeftBracket: return ParseSetLiteral();
    case TokenKind::LeftBrace: return ParseRecordLiteral();
    default:
        if (IsAnyIdentifier(token.kind)) { return ParseNameOrReference(); }
        break;
    }
    ReportUnexpected();
    return ErrorOperand();
}

auto Parser::ParseNumber() -> Operand
{
    const Token token = Current();
    Advance();
    const std::optional<std::uint64_t> value = ParseNumberLiteral(TextOf(token));
    if (!value)
    {
        ReportSyntax(DiagnosticCode::IntegerLiteralTooLarge, token.span, "integer literal does not fit in 64 bits");
        return ErrorOperand();
    }
    if (*value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        Report(DiagnosticCode::IntegerLiteralTooLarge, token.span,
               std::format("integer literal {} is out of range", *value));
        return ErrorOperand();
    }
    return ExpressionOperand(Builder().Long(static_cast<std::int64_t>(*value), token.span), token.span);
}

auto Parser::ParseOtherSlot() -> Operand
{
    const Token token = Current();
    Advance();
    Report(DiagnosticCode::InvalidSlot, token.span,
           std::format("`{}` is not a slot; the slots are `?principal` and `?resource`", TextOf(token)));
    return ErrorOperand();
}

auto Parser::ParseParenthesized() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Advance();
    const Operand inner = ParseExpressionOperand();
    if (!Expect(TokenKind::RightParenthesis)) { return ErrorOperand(); }
    const std::optional<ExpressionIndex> index = Materialize(inner);
    if (!index) { return ErrorOperand(); }
    return ExpressionOperand(*index, SpanFrom(begin));
}

// Name := CommonIdent | AnyIdent {'::' AnyIdent}
// Ref  := Name '::' Str | Name '::' '{' Comma<RefInit> '}'
auto Parser::ParseNameOrReference() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    std::vector<Token> components{Current()};
    Advance();
    while (Accept(TokenKind::DoubleColon))
    {
        if (At(TokenKind::String))
        {
            const Token idToken = Current();
            Advance();
            const SourceSpan span = SpanFrom(begin);
            const std::optional<Name> type = ResolveName(components, {begin, components.back().span.end});
            std::optional<std::string> id = UnescapeToken(idToken);
            if (!type || !id) { return ErrorOperand(); }
            return ExpressionOperand(Builder().Entity(EntityUid{EntityType{*type}, Eid{std::move(*id)}}, span), span);
        }
        if (At(TokenKind::LeftBrace))
        {
            ParseReferenceInitializers(begin);
            return ErrorOperand();
        }
        if (!IsAnyIdentifier(Current().kind))
        {
            ReportUnexpected();
            return ErrorOperand();
        }
        components.push_back(Current());
        Advance();
    }

    const SourceSpan span = SpanFrom(begin);
    if (components.size() == 1)
    {
        if (const std::optional<Variable> variable = VariableOf(components.front().kind))
        {
            Operand operand;
            operand.kind = OperandKind::Variable;
            operand.variable = *variable;
            operand.span = span;
            return operand;
        }
    }
    std::optional<Name> name = ResolveName(components, span);
    if (!name) { return ErrorOperand(); }
    Operand operand;
    operand.kind = OperandKind::Name;
    operand.name = std::move(name);
    operand.span = span;
    return operand;
}

auto Parser::ParseReferenceInitializers(const std::uint32_t begin) -> void
{
    Expect(TokenKind::LeftBrace);
    ParseCommaList(TokenKind::RightBrace, [&] {
        if (!IsAnyIdentifier(Current().kind))
        {
            ReportUnexpected();
            return;
        }
        Advance();
        if (!Expect(TokenKind::Colon)) { return; }
        switch (Current().kind)
        {
        case TokenKind::True:
        case TokenKind::False:
        case TokenKind::Number:
        case TokenKind::String: Advance(); return;
        default: ReportUnexpected(); return;
        }
    });
    Report(DiagnosticCode::InvalidEntityLiteral, SpanFrom(begin),
           "an entity literal is written `Type::\"id\"`; the record form is not supported");
}

auto Parser::ParseSetLiteral() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Advance();
    std::vector<ExpressionIndex> elements;
    bool valid = true;
    ParseCommaList(TokenKind::RightBracket, [&] {
        if (auto element = Materialize(ParseExpressionOperand())) { elements.push_back(*element); }
        else { valid = false; }
    });
    if (!valid || stopped_) { return ErrorOperand(); }
    const SourceSpan span = SpanFrom(begin);
    const ExpressionIndex set = Builder().SetLiteral(elements, span);
    return ExpressionOperand(FoldConstantSet(*arena_, Builder(), set, elements, span), span);
}

// RecInit := 'if' ':' Expr | Expr ':' Expr. The key must be an identifier, a variable or a string
// literal; `if` has its own rule only so that it can be refused as a reserved word.
auto Parser::ParseRecordLiteral() -> Operand
{
    const std::uint32_t begin = Current().span.begin;
    Advance();
    std::vector<std::pair<std::string, ExpressionIndex>> entries;
    bool valid = true;
    ParseCommaList(TokenKind::RightBrace, [&] {
        std::optional<std::string> key;
        if (At(TokenKind::If) && Peek(1).kind == TokenKind::Colon)
        {
            Report(DiagnosticCode::ReservedIdentifier, Current().span, "`if` is a reserved word and cannot be a record key");
            Advance();
        }
        else { key = AttributeOf(ParseExpressionOperand()); }
        if (!Expect(TokenKind::Colon))
        {
            valid = false;
            return;
        }
        const std::optional<ExpressionIndex> value = Materialize(ParseExpressionOperand());
        if (key && value) { entries.emplace_back(std::move(*key), *value); }
        else { valid = false; }
    });
    if (!valid || stopped_) { return ErrorOperand(); }

    std::vector<RecordLiteralField> fields;
    fields.reserve(entries.size());
    for (const auto& [key, value] : entries) { fields.push_back({key, value}); }
    const SourceSpan span = SpanFrom(begin);
    auto record = Builder().RecordLiteral(fields, span);
    if (!record)
    {
        Report(DiagnosticCode::DuplicateRecordKey, span,
               std::format("the key \"{}\" appears more than once in this record", record.error().key));
        return ErrorOperand();
    }
    return ExpressionOperand(*record, span);
}

// ---- Conversions ----

auto Parser::Materialize(const Operand& operand) -> std::optional<ExpressionIndex>
{
    switch (operand.kind)
    {
    case OperandKind::Error: return std::nullopt;
    case OperandKind::Expression: return operand.expression;
    case OperandKind::Variable: return Builder().Variable(operand.variable, operand.span);
    case OperandKind::Bool: return Builder().Bool(operand.boolean, operand.span);
    case OperandKind::String:
    {
        const std::optional<std::string> text = UnescapeToken(operand.token);
        if (!text) { return std::nullopt; }
        return Builder().String(*text, operand.span);
    }
    case OperandKind::Name:
        Report(DiagnosticCode::ArbitraryVariable, operand.span,
               std::format("`{}` is not a variable; the variables are `principal`, `action`, `resource` and "
                           "`context`",
                           operand.name->QualifiedText()));
        return std::nullopt;
    }
    return std::nullopt;
}

auto Parser::AttributeOf(const Operand& operand) -> std::optional<std::string>
{
    switch (operand.kind)
    {
    case OperandKind::Error: return std::nullopt;
    case OperandKind::Variable: return std::string{ToString(operand.variable)};
    case OperandKind::String: return UnescapeToken(operand.token);
    case OperandKind::Name:
        if (!operand.name->IsUnqualified())
        {
            Report(DiagnosticCode::PathAsAttribute, operand.span,
                   std::format("`{}` is a path; a record key must be one identifier or a string literal",
                               operand.name->QualifiedText()));
            return std::nullopt;
        }
        return std::string{operand.name->QualifiedText()};
    case OperandKind::Bool:
        Report(DiagnosticCode::ReservedIdentifier, operand.span,
               std::format("`{}` is a reserved word and cannot be a record key", operand.boolean ? "true" : "false"));
        return std::nullopt;
    case OperandKind::Expression:
        Report(DiagnosticCode::InvalidAttribute, operand.span,
               "a record key must be an identifier or a string literal");
        return std::nullopt;
    }
    return std::nullopt;
}

auto Parser::UnescapeToken(const Token& token) -> std::optional<std::string>
{
    const StringContent content = ContentOf(token, source_->text);
    auto text = Unescape(content.text, content.offset);
    if (!text)
    {
        Report(DiagnosticCode::InvalidEscape, text.error().span, std::string{Describe(text.error().kind)});
        return std::nullopt;
    }
    return std::move(*text);
}

auto Parser::ResolveName(const std::span<const Token> components, const SourceSpan span) -> std::optional<Name>
{
    bool valid = true;
    std::string qualified;
    for (const Token& component : components)
    {
        const std::string_view text = TextOf(component);
        if (!CheckIdentifier(text))
        {
            Report(DiagnosticCode::ReservedIdentifier, component.span,
                   std::format("`{}` is a reserved word and cannot be used in a name", text));
            valid = false;
        }
        if (!qualified.empty()) { qualified += "::"; }
        qualified += text;
    }
    if (!valid) { return std::nullopt; }
    auto name = Name::Parse(qualified);
    if (!name)
    {
        Report(CodeFor(name.error()), span,
               std::format("`{}` is in the reserved namespace `{}`", qualified, ReservedNamespaceName));
        return std::nullopt;
    }
    return *name;
}

auto Parser::CheckFieldName(const Token& token) -> bool
{
    const std::string_view text = TextOf(token);
    if (!CheckIdentifier(text))
    {
        Report(DiagnosticCode::ReservedIdentifier, token.span,
               std::format("`{}` is a reserved word and cannot be an attribute name", text),
               std::format("write the attribute as [\"{}\"]", text));
        return false;
    }
    if (text == ReservedNamespaceName)
    {
        Report(DiagnosticCode::ReservedNamespace, token.span,
               std::format("`{}` is reserved and cannot be an attribute name", text));
        return false;
    }
    return true;
}

auto Parser::BuildMethodCall(const ExpressionIndex receiver, const Access& method,
                             const std::span<const ExpressionIndex> arguments, const SourceSpan span)
    -> std::optional<ExpressionIndex>
{
    const std::string_view name = method.text;
    if (const BuiltInMethod* builtIn = FindBuiltInMethod(name))
    {
        if (arguments.size() != builtIn->argumentCount)
        {
            Report(DiagnosticCode::WrongArity, span,
                   std::format("`{}` takes {} argument{}, but {} {} given", name, builtIn->argumentCount,
                               builtIn->argumentCount == 1 ? "" : "s", arguments.size(),
                               arguments.size() == 1 ? "was" : "were"));
            return std::nullopt;
        }
        ExpressionBuilder builder = Builder();
        if (name == "isEmpty") { return builder.Unary(UnaryOperator::IsEmpty, receiver, span); }
        const BinaryOperator operation = name == "contains"      ? BinaryOperator::Contains
                                         : name == "containsAll" ? BinaryOperator::ContainsAll
                                         : name == "containsAny" ? BinaryOperator::ContainsAny
                                         : name == "getTag"      ? BinaryOperator::GetTag
                                                                 : BinaryOperator::HasTag;
        return builder.Binary(operation, receiver, arguments.front(), span);
    }
    if (IsExtensionFunction(name, ExtensionCallStyle::Method))
    {
        std::vector<ExpressionIndex> all{receiver};
        all.insert(all.end(), arguments.begin(), arguments.end());
        const auto function = Name::Parse(name);
        assert(function);
        return Builder().ExtensionCall(*function, all, span);
    }
    if (IsExtensionFunction(name, ExtensionCallStyle::Function))
    {
        Report(DiagnosticCode::MethodCallOnFunction, span, std::format("`{}` is a function, not a method", name),
               std::format("write it as `{}(...)`", name));
        return std::nullopt;
    }
    Report(DiagnosticCode::UnknownMethod, span, std::format("`{}` is not a known method", name),
           SuggestExtensionCall(name, ExtensionCallStyle::Method));
    return std::nullopt;
}

auto Parser::BuildFunctionCall(const Name& function, const std::span<const ExpressionIndex> arguments,
                               const SourceSpan span) -> std::optional<ExpressionIndex>
{
    const std::string_view name = function.QualifiedText();
    if (function.IsUnqualified() &&
        (FindBuiltInMethod(name) != nullptr || IsExtensionFunction(name, ExtensionCallStyle::Method)))
    {
        Report(DiagnosticCode::FunctionCallOnMethod, span, std::format("`{}` is a method, not a function", name),
               std::format("write it as `receiver.{}(...)`", name));
        return std::nullopt;
    }
    if (IsExtensionFunction(name, ExtensionCallStyle::Function))
    {
        const ExpressionIndex call = Builder().ExtensionCall(function, arguments, span);
        return FoldConstructorCall(*arena_, Builder(), *FindExtensionCall(name), call, arguments, span);
    }
    Report(DiagnosticCode::UnknownFunction, span, std::format("`{}` is not a known function", name),
           SuggestExtensionCall(name, ExtensionCallStyle::Function));
    return std::nullopt;
}

// `cst_to_ast.rs` `construct_expr_rel`, with `!=`, `>` and `>=` built as Rust builds them.
auto Parser::BuildRelation(const ExpressionIndex left, const TokenKind operation, const ExpressionIndex right,
                           const SourceSpan span) -> std::optional<ExpressionIndex>
{
    ExpressionBuilder builder = Builder();
    switch (operation)
    {
    case TokenKind::Less: return builder.Binary(BinaryOperator::Less, left, right, span);
    case TokenKind::LessEqual: return builder.Binary(BinaryOperator::LessEqual, left, right, span);
    case TokenKind::GreaterEqual: return builder.GreaterEqual(left, right, span);
    case TokenKind::Greater: return builder.Greater(left, right, span);
    case TokenKind::NotEqual: return builder.NotEqual(left, right, span);
    case TokenKind::EqualEqual: return builder.Equal(left, right, span);
    case TokenKind::In: return builder.In(left, right, span);
    default:
        Report(DiagnosticCode::InvalidSingleEquals, span, "`=` is not an operator", "use `==` to compare values");
        return std::nullopt;
    }
}

} // namespace cedar::detail

namespace cedar
{

namespace
{

auto InputTooLarge(const std::string_view sourceName) -> ParseError
{
    return ParseError{Diagnostic{DiagnosticCode::InvalidToken, Severity::Error,
                                 std::format("{} is 4 GiB or larger, which is more than Cedar text may be", sourceName),
                                 std::nullopt, std::nullopt, {}, {}}};
}

auto ErrorsOf(detail::Parser& parser) -> ParseError
{
    std::optional<ParseError> error = ParseError::FromDiagnostics(parser.TakeDiagnostics());
    assert(error);
    return std::move(*error);
}

constexpr std::size_t MaximumTextSize = std::numeric_limits<std::uint32_t>::max();

} // namespace

auto ParseExpression(const std::string_view text, const std::string_view sourceName)
    -> std::expected<Expression, ParseError>
{
    if (text.size() >= MaximumTextSize) { return std::unexpected{InputTooLarge(sourceName)}; }
    SharedSourceText source = MakeSourceText(std::string{sourceName}, std::string{text});
    detail::Parser parser{source};
    ExpressionArena arena;
    parser.UseArena(arena);
    const std::optional<ExpressionIndex> root = parser.ParseExpression();
    parser.ExpectEndOfInput();
    if (parser.HasErrors() || !root) { return std::unexpected{ErrorsOf(parser)}; }
    return Expression{std::move(arena), *root, std::move(source)};
}

auto ParseEntityUid(const std::string_view text, const std::string_view sourceName)
    -> std::expected<EntityUid, ParseError>
{
    if (text.size() >= MaximumTextSize) { return std::unexpected{InputTooLarge(sourceName)}; }
    SharedSourceText source = MakeSourceText(std::string{sourceName}, std::string{text});
    detail::Parser parser{source};
    std::optional<EntityUid> uid = parser.ParseEntityUidLiteral();
    parser.ExpectEndOfInput();
    if (parser.HasErrors() || !uid) { return std::unexpected{ErrorsOf(parser)}; }
    return std::move(*uid);
}

namespace
{

// One policy or template read on its own, keeping the whole text (Rust `parse_policy_or_template`).
auto ParseOneBody(const std::string_view text, std::optional<PolicyId> id, const std::string_view sourceName)
    -> std::expected<std::shared_ptr<const Template>, ParseError>
{
    if (text.size() >= MaximumTextSize) { return std::unexpected{InputTooLarge(sourceName)}; }
    detail::Parser parser{MakeSourceText(std::string{sourceName}, std::string{text})};
    std::shared_ptr<const Template> body =
        parser.ParsePolicy(id ? std::move(*id) : PolicyId{"policy0"}, detail::Parser::PolicyText::WholeInput);
    parser.ExpectEndOfInput();
    if (parser.HasErrors() || !body) { return std::unexpected{ErrorsOf(parser)}; }
    return body;
}

auto WholeTextError(const std::shared_ptr<const Template>& body, const DiagnosticCode code, std::string message)
    -> ParseError
{
    return ParseError{
        Diagnostic{code, Severity::Error, std::move(message), std::nullopt, body->SourceExtent(), {}, body->Source()}};
}

} // namespace

auto ParsePolicy(const std::string_view text, std::optional<PolicyId> id, const std::string_view sourceName)
    -> std::expected<Policy, ParseError>
{
    auto body = ParseOneBody(text, std::move(id), sourceName);
    if (!body) { return std::unexpected{std::move(body.error())}; }
    if (!(*body)->IsStatic())
    {
        return std::unexpected{WholeTextError(*body, DiagnosticCode::ExpectedStaticPolicy,
                                              "expected a static policy, found a template (it has a slot)")};
    }
    auto policy = Policy::Static(*body);
    assert(policy);
    return std::move(*policy);
}

auto ParseTemplate(const std::string_view text, std::optional<PolicyId> id, const std::string_view sourceName)
    -> std::expected<std::shared_ptr<const Template>, ParseError>
{
    auto body = ParseOneBody(text, std::move(id), sourceName);
    if (!body) { return body; }
    if ((*body)->IsStatic())
    {
        return std::unexpected{WholeTextError(*body, DiagnosticCode::ExpectedTemplate,
                                              "expected a template, found a static policy (it has no slot)")};
    }
    return body;
}

auto ParsePolicySet(const std::string_view text, const std::string_view sourceName)
    -> std::expected<PolicySet, ParseError>
{
    if (text.size() >= MaximumTextSize) { return std::unexpected{InputTooLarge(sourceName)}; }
    detail::Parser parser{MakeSourceText(std::string{sourceName}, std::string{text})};
    std::vector<std::shared_ptr<const Template>> bodies = parser.ParsePolicies();
    if (parser.HasErrors()) { return std::unexpected{ErrorsOf(parser)}; }
    PolicySet policies;
    for (std::shared_ptr<const Template>& body : bodies)
    {
        // Ids are positional, so neither add can find its id taken.
        if (body->IsStatic())
        {
            auto policy = Policy::Static(std::move(body));
            assert(policy);
            [[maybe_unused]] const auto added = policies.Add(std::move(*policy));
            assert(added);
        }
        else
        {
            [[maybe_unused]] const auto added = policies.AddTemplate(std::move(body));
            assert(added);
        }
    }
    return policies;
}

} // namespace cedar
