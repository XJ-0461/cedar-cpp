// Cedar schema syntax into the fragment model: a recursive-descent reading of
// `cedar-policy-core/src/validator/cedar_schema/grammar.lalrpop` into a small declaration tree,
// then the conversion `to_json_schema.rs` makes from that tree (namespace merging, duplicate and
// reserved-name checks, `appliesTo` checks, shadowing warnings).
//
// Rust's grammar has no error recovery: the first lexical or syntax error, a reserved identifier,
// a bad string escape or a repeated annotation key ends the parse. The conversion runs only on a
// whole parse, as here.

#include "schema/SchemaParser.hpp"

#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"
#include "syntax/Lexer.hpp"
#include "syntax/Unescape.hpp"

#include <cedar/Schema.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

namespace detail
{

namespace
{

// ---- Tokens ----

// The schema grammar's terminals. Words are not split into keywords here: every keyword of the
// schema grammar is also an `AnyIdent`, so the parser compares word text where a keyword is
// expected.
enum class SchemaTokenKind : std::uint8_t
{
    Word,
    String,
    Comma,
    Semicolon,
    Colon,
    DoubleColon,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Less,
    Greater,
    Equal,
    Question,
    At,
    LeftParenthesis,
    RightParenthesis,
    /// A token of the policy grammar that the schema grammar never accepts (a number, `.`, `!`...).
    Unexpected,
    /// A lexical error.
    Invalid,
    EndOfInput,
};

struct SchemaToken
{
    SchemaTokenKind kind;
    SourceSpan span;
};

// The policy lexer's tokens recut to the schema grammar's (`grammar.lalrpop` `match` block). The
// two grammars share whitespace, comments, words and string literals. They differ in `?`: the
// schema grammar has it alone, where the policy lexer makes `?word` a slot and refuses a lone `?`.
// Two-character operators that the schema grammar does not have are split into the two
// one-character tokens it reads instead.
[[nodiscard]] auto TokenizeSchema(const std::string_view text) -> std::vector<SchemaToken>
{
    std::vector<SchemaToken> tokens;
    const auto add = [&](const SchemaTokenKind kind, const std::uint32_t begin, const std::uint32_t end)
    { tokens.push_back(SchemaToken{kind, SourceSpan{begin, end}}); };
    for (const Token& token : Tokenize(text))
    {
        const auto [begin, end] = token.span;
        if (IsAnyIdentifier(token.kind))
        {
            add(SchemaTokenKind::Word, begin, end);
            continue;
        }
        switch (token.kind)
        {
        case TokenKind::String: add(SchemaTokenKind::String, begin, end); break;
        case TokenKind::Comma: add(SchemaTokenKind::Comma, begin, end); break;
        case TokenKind::Semicolon: add(SchemaTokenKind::Semicolon, begin, end); break;
        case TokenKind::Colon: add(SchemaTokenKind::Colon, begin, end); break;
        case TokenKind::DoubleColon: add(SchemaTokenKind::DoubleColon, begin, end); break;
        case TokenKind::LeftBrace: add(SchemaTokenKind::LeftBrace, begin, end); break;
        case TokenKind::RightBrace: add(SchemaTokenKind::RightBrace, begin, end); break;
        case TokenKind::LeftBracket: add(SchemaTokenKind::LeftBracket, begin, end); break;
        case TokenKind::RightBracket: add(SchemaTokenKind::RightBracket, begin, end); break;
        case TokenKind::Less: add(SchemaTokenKind::Less, begin, end); break;
        case TokenKind::Greater: add(SchemaTokenKind::Greater, begin, end); break;
        case TokenKind::Equal: add(SchemaTokenKind::Equal, begin, end); break;
        case TokenKind::At: add(SchemaTokenKind::At, begin, end); break;
        case TokenKind::LeftParenthesis: add(SchemaTokenKind::LeftParenthesis, begin, end); break;
        case TokenKind::RightParenthesis: add(SchemaTokenKind::RightParenthesis, begin, end); break;
        case TokenKind::LessEqual:
            add(SchemaTokenKind::Less, begin, begin + 1);
            add(SchemaTokenKind::Equal, begin + 1, end);
            break;
        case TokenKind::GreaterEqual:
            add(SchemaTokenKind::Greater, begin, begin + 1);
            add(SchemaTokenKind::Equal, begin + 1, end);
            break;
        case TokenKind::EqualEqual:
            add(SchemaTokenKind::Equal, begin, begin + 1);
            add(SchemaTokenKind::Equal, begin + 1, end);
            break;
        case TokenKind::PrincipalSlot:
        case TokenKind::ResourceSlot:
        case TokenKind::OtherSlot:
            add(SchemaTokenKind::Question, begin, begin + 1);
            add(SchemaTokenKind::Word, begin + 1, end);
            break;
        case TokenKind::InvalidCharacter:
            add(text.substr(begin, end - begin) == "?" ? SchemaTokenKind::Question : SchemaTokenKind::Invalid, begin,
                end);
            break;
        case TokenKind::InvalidString:
        case TokenKind::InvalidUtf8: add(SchemaTokenKind::Invalid, begin, end); break;
        case TokenKind::EndOfInput: add(SchemaTokenKind::EndOfInput, begin, end); break;
        default: add(SchemaTokenKind::Unexpected, begin, end); break;
        }
    }
    return tokens;
}

[[nodiscard]] constexpr auto Describe(const SchemaTokenKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case SchemaTokenKind::Word: return "identifier";
    case SchemaTokenKind::String: return "string literal";
    case SchemaTokenKind::Comma: return ",";
    case SchemaTokenKind::Semicolon: return ";";
    case SchemaTokenKind::Colon: return ":";
    case SchemaTokenKind::DoubleColon: return "::";
    case SchemaTokenKind::LeftBrace: return "{";
    case SchemaTokenKind::RightBrace: return "}";
    case SchemaTokenKind::LeftBracket: return "[";
    case SchemaTokenKind::RightBracket: return "]";
    case SchemaTokenKind::Less: return "<";
    case SchemaTokenKind::Greater: return ">";
    case SchemaTokenKind::Equal: return "=";
    case SchemaTokenKind::Question: return "?";
    case SchemaTokenKind::At: return "@";
    case SchemaTokenKind::LeftParenthesis: return "(";
    case SchemaTokenKind::RightParenthesis: return ")";
    case SchemaTokenKind::Unexpected: return "token";
    case SchemaTokenKind::Invalid: return "invalid token";
    case SchemaTokenKind::EndOfInput: return "end of input";
    }
    return "token";
}

/// What a construct that failed to parse holds in place of its name; never reaches a model.
[[nodiscard]] auto Placeholder() -> Identifier
{
    static const Identifier placeholder = *Identifier::Parse("_");
    return placeholder;
}

// ---- The declaration tree (Rust's `cedar_schema::ast`) ----

struct NamedIdentifier
{
    Identifier name;
    SourceSpan span;
};

/// An action name or attribute name: an identifier or a string literal (grammar `Name`).
struct NamedText
{
    std::string text;
    SourceSpan span;
};

struct ParsedPath
{
    std::vector<Identifier> components;
    SourceSpan span;
};

struct ParsedEntity
{
    std::vector<NamedIdentifier> names;
    std::variant<StandardEntityType, EnumEntityType> kind;
    bool external = false;
};

enum class ApplicationKind : std::uint8_t
{
    Principal,
    Resource,
    Context,
};

/// One `principal: ...`, `resource: ...` or `context: ...` inside `appliesTo`.
struct ParsedApplication
{
    ApplicationKind kind;
    std::vector<TypeName> entityTypes{};
    SchemaType context = EmptyRecordType();
    SourceSpan span{};
};

struct ParsedAction
{
    std::vector<NamedText> names;
    std::optional<std::vector<ActionReference>> parents;
    std::optional<std::vector<ParsedApplication>> appliesTo;
    bool external = false;
};

struct ParsedCommonType
{
    NamedIdentifier name;
    SchemaType type;
};

struct ParsedDeclaration
{
    Annotations annotations;
    SourceSpan span;
    std::variant<ParsedEntity, ParsedAction, ParsedCommonType> content;
};

struct ParsedNamespace
{
    std::optional<ParsedPath> name;
    Annotations annotations;
    std::vector<ParsedDeclaration> declarations;
    SourceSpan span;
};

// ---- Parser ----

class SchemaParser final
{
public:
    explicit SchemaParser(SharedSourceText source)
        : source_{std::move(source)}, text_{source_->text}, tokens_{TokenizeSchema(text_)}
    {
    }

    /// Grammar `Schema`: annotated namespaces and annotated top-level declarations, in order.
    [[nodiscard]] auto ParseSchema() -> std::vector<ParsedNamespace>
    {
        std::vector<ParsedNamespace> namespaces;
        while (!At(SchemaTokenKind::EndOfInput))
        {
            const std::uint32_t begin = Current().span.begin;
            Annotations annotations = ParseAnnotations();
            if (AtWord("namespace"))
            {
                namespaces.push_back(ParseNamespace(std::move(annotations), begin));
                continue;
            }
            ParsedNamespace unnamed;
            unnamed.declarations.push_back(ParseDeclaration(std::move(annotations)));
            unnamed.span = SpanFrom(begin);
            namespaces.push_back(std::move(unnamed));
        }
        return namespaces;
    }

    [[nodiscard]] auto TakeError() -> std::optional<Diagnostic> { return std::move(error_); }

private:
    // ---- Cursor ----

    [[nodiscard]] auto Current() const noexcept -> const SchemaToken&
    {
        return error_ ? tokens_.back() : tokens_[position_];
    }

    [[nodiscard]] auto PeekKind(const std::size_t offset) const noexcept -> SchemaTokenKind
    {
        if (error_) { return SchemaTokenKind::EndOfInput; }
        return tokens_[std::min(position_ + offset, tokens_.size() - 1)].kind;
    }

    [[nodiscard]] auto TextOf(const SchemaToken& token) const noexcept -> std::string_view
    {
        return std::string_view{text_}.substr(token.span.begin, token.span.Length());
    }

    [[nodiscard]] auto At(const SchemaTokenKind kind) const noexcept -> bool { return Current().kind == kind; }

    [[nodiscard]] auto AtWord(const std::string_view word) const noexcept -> bool
    {
        return At(SchemaTokenKind::Word) && TextOf(Current()) == word;
    }

    void Advance() noexcept
    {
        if (error_ || tokens_[position_].kind == SchemaTokenKind::EndOfInput) { return; }
        previousEnd_ = tokens_[position_].span.end;
        ++position_;
    }

    auto Accept(const SchemaTokenKind kind) noexcept -> bool
    {
        if (!At(kind)) { return false; }
        Advance();
        return true;
    }

    auto AcceptWord(const std::string_view word) noexcept -> bool
    {
        if (!AtWord(word)) { return false; }
        Advance();
        return true;
    }

    auto Expect(const SchemaTokenKind kind) -> bool
    {
        if (Accept(kind)) { return true; }
        Unexpected(std::format("`{}`", Describe(kind)));
        return false;
    }

    /// Reports the current token as not what the grammar allows here.
    void Unexpected(const std::string_view expected)
    {
        if (error_) { return; }
        const SchemaToken& found = Current();
        switch (found.kind)
        {
        case SchemaTokenKind::EndOfInput:
            Fail(DiagnosticCode::UnexpectedEndOfInput, found.span, std::format("expected {}, found end of input", expected));
            return;
        case SchemaTokenKind::Invalid:
            Fail(DiagnosticCode::InvalidToken, found.span, std::format("invalid token `{}`", TextOf(found)));
            return;
        default:
            Fail(DiagnosticCode::UnexpectedToken, found.span,
                 std::format("expected {}, found `{}`", expected, TextOf(found)));
            return;
        }
    }

    void Fail(const DiagnosticCode code, const SourceSpan span, std::string message)
    {
        if (error_) { return; }
        error_ = Diagnostic{.code = code, .message = std::move(message), .primary = span, .source = source_};
    }

    [[nodiscard]] auto SpanFrom(const std::uint32_t begin) const noexcept -> SourceSpan
    {
        return {begin, std::max(begin, previousEnd_)};
    }

    // ---- Words and literals ----

    /// Grammar `AnyIdent`: any word, keywords included.
    [[nodiscard]] auto ParseAnyIdentifier() -> std::string_view
    {
        if (!At(SchemaTokenKind::Word))
        {
            Unexpected("an identifier");
            return {};
        }
        const std::string_view word = TextOf(Current());
        Advance();
        return word;
    }

    /// Grammar `Ident`: a word that is not a reserved keyword (Rust's `Id::from_str`). `__cedar`
    /// passes here; the declarations that may not use it check later.
    [[nodiscard]] auto ParseIdentifier() -> NamedIdentifier
    {
        const SourceSpan span = Current().span;
        const std::string_view word = ParseAnyIdentifier();
        if (error_) { return {Placeholder(), span}; }
        auto identifier = Identifier::Parse(word);
        if (!identifier)
        {
            Fail(DiagnosticCode::ReservedIdentifier, span, std::format("`{}` is a reserved identifier", word));
            return {Placeholder(), span};
        }
        return {*identifier, span};
    }

    /// Grammar `STR`: a string literal, unescaped.
    [[nodiscard]] auto ParseString() -> std::string
    {
        if (!At(SchemaTokenKind::String))
        {
            Unexpected("a string literal");
            return {};
        }
        const SourceSpan span = Current().span;
        Advance();
        const std::string_view content = std::string_view{text_}.substr(span.begin + 1, span.Length() - 2);
        auto unescaped = Unescape(content, span.begin + 1);
        if (!unescaped)
        {
            Fail(DiagnosticCode::InvalidEscape, unescaped.error().span,
                 std::format("invalid escape in string literal: {}", Describe(unescaped.error().kind)));
            return {};
        }
        return *std::move(unescaped);
    }

    /// Grammar `Name`: an identifier or a string literal.
    [[nodiscard]] auto ParseName() -> NamedText
    {
        const SourceSpan span = Current().span;
        if (At(SchemaTokenKind::String)) { return {ParseString(), span}; }
        if (!At(SchemaTokenKind::Word))
        {
            Unexpected("a name (an identifier or a string literal)");
            return {{}, span};
        }
        const NamedIdentifier identifier = ParseIdentifier();
        return {std::string{identifier.name.Text()}, span};
    }

    /// Grammar `Path`: identifiers joined by `::`.
    [[nodiscard]] auto ParsePath() -> ParsedPath
    {
        const std::uint32_t begin = Current().span.begin;
        ParsedPath path;
        path.components.push_back(ParseIdentifier().name);
        while (!error_ && Accept(SchemaTokenKind::DoubleColon)) { path.components.push_back(ParseIdentifier().name); }
        path.span = SpanFrom(begin);
        return path;
    }

    [[nodiscard]] static auto ToTypeName(const ParsedPath& path) -> TypeName
    {
        const std::span<const Identifier> components{path.components};
        return TypeName{.name = Name::Qualified(components.first(components.size() - 1), components.back()),
                        .span = ModelSpan{path.span}};
    }

    [[nodiscard]] auto ParseTypeName() -> TypeName
    {
        const ParsedPath path = ParsePath();
        if (error_) { return TypeName{.name = Name::Unqualified(Placeholder())}; }
        return ToTypeName(path);
    }

    /// Grammar `Annotation*`. A key given twice is an error (Rust's `deduplicate_annotations`).
    [[nodiscard]] auto ParseAnnotations() -> Annotations
    {
        Annotations annotations;
        while (!error_ && At(SchemaTokenKind::At))
        {
            const std::uint32_t begin = Current().span.begin;
            Advance();
            const std::string_view key = ParseAnyIdentifier();
            std::string value;
            if (Accept(SchemaTokenKind::LeftParenthesis))
            {
                value = ParseString();
                Expect(SchemaTokenKind::RightParenthesis);
            }
            if (error_) { break; }
            // A value left out is the empty string (Rust's `Annotation::with_optional_value`).
            if (!annotations.Insert(Symbol::Intern(key), std::move(value)))
            {
                Fail(DiagnosticCode::DuplicateAnnotation, SpanFrom(begin), std::format("duplicate annotation `@{}`", key));
            }
        }
        return annotations;
    }

    // ---- Types ----

    /// Grammar `Type`: a path, `Set<Type>` or a record type.
    [[nodiscard]] auto ParseType(const std::size_t depth) -> SchemaType
    {
        if (depth > MaximumSchemaTypeDepth)
        {
            Fail(DiagnosticCode::NestingTooDeep, Current().span,
                 std::format("types nest more than {} levels deep", MaximumSchemaTypeDepth));
            return EmptyRecordType();
        }
        const std::uint32_t begin = Current().span.begin;
        if (AtWord("Set") && PeekKind(1) == SchemaTokenKind::Less) { return ParseSetType(depth, begin); }
        if (At(SchemaTokenKind::LeftBrace)) { return ParseRecordType(depth); }
        if (At(SchemaTokenKind::Word))
        {
            TypeName name = ParseTypeName();
            const ModelSpan span = name.span;
            return SchemaType{.kind = schema_type::EntityOrCommon{std::move(name)}, .span = span};
        }
        Unexpected("a type");
        return EmptyRecordType();
    }

    [[gnu::noinline]] auto ParseSetType(const std::size_t depth, const std::uint32_t begin) -> SchemaType
    {
        Advance();
        Advance();
        SchemaType element = ParseType(depth + 1);
        Expect(SchemaTokenKind::Greater);
        return SchemaType{.kind = schema_type::Set{std::indirect<SchemaType>{std::move(element)}},
                          .span = ModelSpan{SpanFrom(begin)}};
    }

    /// Grammar `'{' AttrDecls? '}'`. A name given twice keeps the last declaration, as Rust's
    /// collection into a `BTreeMap` does.
    [[gnu::noinline]] auto ParseRecordType(const std::size_t depth) -> SchemaType
    {
        const std::uint32_t begin = Current().span.begin;
        Expect(SchemaTokenKind::LeftBrace);
        schema_type::Record record;
        if (!At(SchemaTokenKind::RightBrace))
        {
            while (!error_)
            {
                ParseAttribute(depth, record);
                if (!Accept(SchemaTokenKind::Comma) || At(SchemaTokenKind::RightBrace)) { break; }
            }
        }
        Expect(SchemaTokenKind::RightBrace);
        return SchemaType{.kind = std::move(record), .span = ModelSpan{SpanFrom(begin)}};
    }

    /// Grammar `Annotation* Name '?'? ':' Type`.
    [[gnu::noinline]] void ParseAttribute(const std::size_t depth, schema_type::Record& record)
    {
        const std::uint32_t begin = Current().span.begin;
        Annotations annotations = ParseAnnotations();
        NamedText name = ParseName();
        const bool required = !Accept(SchemaTokenKind::Question);
        Expect(SchemaTokenKind::Colon);
        SchemaType type = ParseType(depth + 1);
        if (error_) { return; }
        RecordAttribute attribute{.name = std::move(name.text),
                                  .type = std::indirect<SchemaType>{std::move(type)},
                                  .required = required,
                                  .annotations = std::move(annotations),
                                  .span = ModelSpan{SpanFrom(begin)}};
        const auto position =
            std::ranges::lower_bound(record.attributes, attribute.name, std::less<>{}, &RecordAttribute::name);
        if (position != record.attributes.end() && position->name == attribute.name)
        {
            *position = std::move(attribute);
            return;
        }
        record.attributes.insert(position, std::move(attribute));
    }

    /// Grammar `EntTypes`: one path, or a bracketed list of paths (possibly empty, no trailing comma).
    [[nodiscard]] auto ParseEntityTypes() -> std::vector<TypeName>
    {
        std::vector<TypeName> types;
        if (!Accept(SchemaTokenKind::LeftBracket))
        {
            types.push_back(ParseTypeName());
            return types;
        }
        if (Accept(SchemaTokenKind::RightBracket)) { return types; }
        types.push_back(ParseTypeName());
        while (!error_ && Accept(SchemaTokenKind::Comma)) { types.push_back(ParseTypeName()); }
        Expect(SchemaTokenKind::RightBracket);
        return types;
    }

    // ---- Declarations ----

    [[nodiscard]] auto ParseNamespace(Annotations annotations, const std::uint32_t begin) -> ParsedNamespace
    {
        Advance(); // `namespace`
        ParsedNamespace parsed;
        parsed.annotations = std::move(annotations);
        parsed.name = ParsePath();
        Expect(SchemaTokenKind::LeftBrace);
        while (!error_ && !At(SchemaTokenKind::RightBrace) && !At(SchemaTokenKind::EndOfInput))
        {
            Annotations declarationAnnotations = ParseAnnotations();
            parsed.declarations.push_back(ParseDeclaration(std::move(declarationAnnotations)));
        }
        Expect(SchemaTokenKind::RightBrace);
        parsed.span = SpanFrom(begin);
        return parsed;
    }

    /// Grammar `Decl`: an entity, action or common type declaration.
    [[nodiscard]] auto ParseDeclaration(Annotations annotations) -> ParsedDeclaration
    {
        const std::uint32_t begin = Current().span.begin;
        ParsedDeclaration declaration{.annotations = std::move(annotations), .span = {}, .content = ParsedEntity{}};
        if (AcceptWord("entity")) { declaration.content = ParseEntity(false); }
        else if (AcceptWord("action")) { declaration.content = ParseAction(false); }
        else if (AcceptWord("type")) { declaration.content = ParseCommonType(); }
        else if (AcceptWord("external"))
        {
            if (AcceptWord("entity")) { declaration.content = ParseEntity(true); }
            else if (AcceptWord("action")) { declaration.content = ParseAction(true); }
            else { Unexpected("`entity` or `action`"); }
        }
        else { Unexpected("a declaration (`entity`, `action`, `type` or `namespace`)"); }
        declaration.span = SpanFrom(begin);
        return declaration;
    }

    /// Grammar `Entity` after `entity`: `Idents ['in' EntTypes] [['='] RecType] ['tags' Type] ';'`,
    /// `Idents 'enum' '[' Eids ']' ';'`, or for an external entity `Idents ['in' EntTypes] ';'`.
    [[nodiscard]] auto ParseEntity(const bool external) -> ParsedEntity
    {
        ParsedEntity entity;
        entity.external = external;
        entity.names.push_back(ParseIdentifier());
        while (!error_ && Accept(SchemaTokenKind::Comma)) { entity.names.push_back(ParseIdentifier()); }

        if (!external && AcceptWord("enum"))
        {
            EnumEntityType enumerated;
            Expect(SchemaTokenKind::LeftBracket);
            enumerated.choices.emplace_back(ParseString());
            while (!error_ && Accept(SchemaTokenKind::Comma)) { enumerated.choices.emplace_back(ParseString()); }
            Expect(SchemaTokenKind::RightBracket);
            Expect(SchemaTokenKind::Semicolon);
            entity.kind = std::move(enumerated);
            return entity;
        }

        StandardEntityType standard;
        if (AcceptWord("in")) { standard.memberOfTypes = ParseEntityTypes(); }
        if (!external)
        {
            if (Accept(SchemaTokenKind::Equal))
            {
                if (!At(SchemaTokenKind::LeftBrace)) { Unexpected("`{`"); }
                standard.shape = ParseRecordType(1);
            }
            else if (At(SchemaTokenKind::LeftBrace)) { standard.shape = ParseRecordType(1); }
            if (AcceptWord("tags")) { standard.tags = ParseType(1); }
        }
        Expect(SchemaTokenKind::Semicolon);
        entity.kind = std::move(standard);
        return entity;
    }

    /// Grammar `QualName`: a name (no type), or `Path '::' STR`.
    [[nodiscard]] auto ParseActionReference() -> ActionReference
    {
        const std::uint32_t begin = Current().span.begin;
        if (At(SchemaTokenKind::String))
        {
            std::string id = ParseString();
            return ActionReference{.id = std::move(id), .type = std::nullopt, .span = ModelSpan{SpanFrom(begin)}};
        }
        ParsedPath path;
        const NamedIdentifier first = ParseIdentifier();
        path.components.push_back(first.name);
        while (!error_ && Accept(SchemaTokenKind::DoubleColon))
        {
            if (At(SchemaTokenKind::String))
            {
                path.span = SourceSpan{begin, previousEnd_ - 2};
                std::string id = ParseString();
                return ActionReference{.id = std::move(id), .type = ToTypeName(path), .span = ModelSpan{SpanFrom(begin)}};
            }
            path.components.push_back(ParseIdentifier().name);
        }
        if (path.components.size() > 1) { Unexpected("`::` and a string literal"); }
        return ActionReference{
            .id = std::string{first.name.Text()}, .type = std::nullopt, .span = ModelSpan{SpanFrom(begin)}};
    }

    /// Grammar `Action` after `action`: `Names ['in' QualNameOrQualNames] ['appliesTo' '{' AppDecls '}']
    /// ['attributes' '{' '}'] ';'`; an external action stops after the parents.
    [[nodiscard]] auto ParseAction(const bool external) -> ParsedAction
    {
        ParsedAction action;
        action.external = external;
        action.names.push_back(ParseName());
        while (!error_ && Accept(SchemaTokenKind::Comma)) { action.names.push_back(ParseName()); }

        if (AcceptWord("in"))
        {
            std::vector<ActionReference> parents;
            if (Accept(SchemaTokenKind::LeftBracket))
            {
                parents.push_back(ParseActionReference());
                while (!error_ && Accept(SchemaTokenKind::Comma)) { parents.push_back(ParseActionReference()); }
                Expect(SchemaTokenKind::RightBracket);
            }
            else { parents.push_back(ParseActionReference()); }
            action.parents = std::move(parents);
        }
        if (!external)
        {
            if (AcceptWord("appliesTo"))
            {
                Expect(SchemaTokenKind::LeftBrace);
                action.appliesTo = ParseApplications();
                Expect(SchemaTokenKind::RightBrace);
            }
            if (AcceptWord("attributes"))
            {
                Expect(SchemaTokenKind::LeftBrace);
                Expect(SchemaTokenKind::RightBrace);
            }
        }
        Expect(SchemaTokenKind::Semicolon);
        return action;
    }

    /// Grammar `AppDecls`: at least one `principal`, `resource` or `context` entry, comma separated,
    /// with an optional trailing comma. Repeats are refused when converting.
    [[nodiscard]] auto ParseApplications() -> std::vector<ParsedApplication>
    {
        std::vector<ParsedApplication> applications;
        while (!error_)
        {
            const std::uint32_t begin = Current().span.begin;
            ParsedApplication application{.kind = ApplicationKind::Principal};
            if (AtWord("principal") || AtWord("resource"))
            {
                application.kind = AtWord("principal") ? ApplicationKind::Principal : ApplicationKind::Resource;
                Advance();
                Expect(SchemaTokenKind::Colon);
                application.entityTypes = ParseEntityTypes();
            }
            else if (AcceptWord("context"))
            {
                application.kind = ApplicationKind::Context;
                Expect(SchemaTokenKind::Colon);
                if (At(SchemaTokenKind::LeftBrace)) { application.context = ParseRecordType(1); }
                else
                {
                    // A named context must be a common type (Rust's `Type::CommonTypeRef`).
                    TypeName name = ParseTypeName();
                    const ModelSpan span = name.span;
                    application.context =
                        SchemaType{.kind = schema_type::CommonTypeReference{std::move(name)}, .span = span};
                }
            }
            else
            {
                Unexpected("`principal`, `resource` or `context`");
                break;
            }
            application.span = SpanFrom(begin);
            applications.push_back(std::move(application));
            if (!Accept(SchemaTokenKind::Comma) || At(SchemaTokenKind::RightBrace)) { break; }
        }
        return applications;
    }

    /// Grammar `TypeDecl` after `type`: `Ident '=' Type ';'`.
    [[nodiscard]] auto ParseCommonType() -> ParsedCommonType
    {
        ParsedCommonType common{.name = ParseIdentifier(), .type = EmptyRecordType()};
        Expect(SchemaTokenKind::Equal);
        common.type = ParseType(1);
        Expect(SchemaTokenKind::Semicolon);
        return common;
    }

    SharedSourceText source_;
    std::string_view text_;
    std::vector<SchemaToken> tokens_;
    std::size_t position_ = 0;
    std::uint32_t previousEnd_ = 0;
    std::optional<Diagnostic> error_;
};

// ---- Conversion (to_json_schema.rs) ----

/// Rust's `BUILTIN_TYPES` and the extension type names (`Extensions::all_available().ext_types()`):
/// an entity or common type with one of these names shadows the built-in.
constexpr std::array<std::string_view, 7> BuiltinTypeNames{"Bool",    "Long",     "String", "datetime",
                                                           "decimal", "duration", "ipaddr"};

class Converter final
{
public:
    explicit Converter(SharedSourceText source) : source_{std::move(source)} {}

    [[nodiscard]] auto Convert(std::vector<ParsedNamespace> parsed)
        -> std::expected<WithWarnings<std::shared_ptr<const SchemaFragmentModel>>, SchemaError>
    {
        const std::vector<ParsedNamespace> namespaces = MergeUnqualified(std::move(parsed));

        // build_namespace_bindings: names, duplicate declarations, then duplicate namespaces.
        std::vector<std::optional<Name>> names;
        for (const ParsedNamespace& definition : namespaces)
        {
            names.push_back(NamespaceName(definition));
            CheckDuplicateDeclarations(definition);
        }
        if (diagnostics_.empty()) { CheckDuplicateNamespaces(namespaces, names); }
        if (auto error = TakeErrors()) { return std::unexpected{*std::move(error)}; }
        std::vector<Diagnostic> warnings = Warnings(namespaces);

        auto model = std::make_shared<SchemaFragmentModel>();
        model->source = source_;
        for (std::size_t index = 0; index < namespaces.size(); ++index)
        {
            NamespaceDefinition definition = ConvertNamespace(namespaces[index]);
            definition.name = names[index];
            [[maybe_unused]] const bool inserted = InsertSorted(model->namespaces, std::move(definition));
        }
        if (auto error = TakeErrors()) { return std::unexpected{*std::move(error)}; }
        if (auto restrictions = CheckFragmentRestrictions(*model); !restrictions)
        {
            return std::unexpected{std::move(restrictions).error()};
        }
        return WithWarnings<std::shared_ptr<const SchemaFragmentModel>>{std::move(model), std::move(warnings)};
    }

private:
    /// Rust's `split_unqualified_namespace`: the named namespaces in order, then one unnamed namespace
    /// holding every top-level declaration, if there are any.
    [[nodiscard]] static auto MergeUnqualified(std::vector<ParsedNamespace> parsed) -> std::vector<ParsedNamespace>
    {
        std::vector<ParsedNamespace> namespaces;
        ParsedNamespace unqualified;
        for (ParsedNamespace& definition : parsed)
        {
            if (definition.name)
            {
                namespaces.push_back(std::move(definition));
                continue;
            }
            for (ParsedDeclaration& declaration : definition.declarations)
            {
                unqualified.declarations.push_back(std::move(declaration));
            }
        }
        if (!unqualified.declarations.empty()) { namespaces.push_back(std::move(unqualified)); }
        return namespaces;
    }

    [[nodiscard]] auto TakeErrors() -> std::optional<SchemaError>
    {
        std::vector<Diagnostic> diagnostics = std::exchange(diagnostics_, {});
        return SchemaError::FromDiagnostics(std::move(diagnostics));
    }

    void Error(const DiagnosticCode code, const SourceSpan span, std::string message)
    {
        diagnostics_.push_back(Diagnostic{.code = code, .message = std::move(message), .primary = span, .source = source_});
    }

    /// The namespace's name; `__cedar` may not appear in it.
    [[nodiscard]] auto NamespaceName(const ParsedNamespace& definition) -> std::optional<Name>
    {
        if (!definition.name) { return std::nullopt; }
        const ParsedPath& path = *definition.name;
        const std::span<const Identifier> components{path.components};
        const Name name = Name::Qualified(components.first(components.size() - 1), components.back());
        if (name.IsReserved())
        {
            Error(DiagnosticCode::ReservedNamespace, path.span,
                  std::format("namespace `{}` uses the reserved namespace `{}`", name.QualifiedText(), ReservedNamespaceName));
        }
        return name;
    }

    /// Rust's `NamespaceRecord::new`: each entity type, action and common type name once per namespace.
    void CheckDuplicateDeclarations(const ParsedNamespace& definition)
    {
        std::map<std::string, SourceSpan, std::less<>> entities;
        std::map<std::string, SourceSpan, std::less<>> actions;
        std::map<std::string, SourceSpan, std::less<>> commonTypes;
        const auto add = [&](std::map<std::string, SourceSpan, std::less<>>& seen, const std::string_view what,
                             const std::string_view name, const SourceSpan span)
        {
            if (seen.emplace(std::string{name}, span).second) { return; }
            Error(DiagnosticCode::DuplicateDeclarations, span, std::format("{} `{}` declared twice", what, name));
        };
        for (const ParsedDeclaration& declaration : definition.declarations)
        {
            if (const auto* entity = std::get_if<ParsedEntity>(&declaration.content))
            {
                for (const NamedIdentifier& name : entity->names) { add(entities, "entity type", name.name.Text(), name.span); }
            }
            else if (const auto* action = std::get_if<ParsedAction>(&declaration.content))
            {
                for (const NamedText& name : action->names) { add(actions, "action", name.text, name.span); }
            }
            else
            {
                const auto& common = std::get<ParsedCommonType>(declaration.content);
                add(commonTypes, "common type", common.name.name.Text(), common.name.span);
            }
        }
    }

    void CheckDuplicateNamespaces(const std::vector<ParsedNamespace>& namespaces,
                                  const std::vector<std::optional<Name>>& names)
    {
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            for (std::size_t earlier = 0; earlier < index; ++earlier)
            {
                if (names[earlier] != names[index]) { continue; }
                const SourceSpan span = namespaces[index].name ? namespaces[index].name->span : SourceSpan{};
                Error(DiagnosticCode::DuplicateNamespaces, span,
                      std::format("namespace `{}` declared twice", names[index] ? names[index]->QualifiedText() : ""));
                break;
            }
        }
    }

    /// Rust's `make_warning_for_shadowing`, per namespace: a common type with an entity type's name,
    /// and an entity or common type with a built-in type's name.
    [[nodiscard]] auto Warnings(const std::vector<ParsedNamespace>& namespaces) const -> std::vector<Diagnostic>
    {
        std::vector<Diagnostic> warnings;
        const auto warn = [&](const DiagnosticCode code, const SourceSpan span, std::string message, std::string help)
        {
            warnings.push_back(Diagnostic{.code = code,
                                          .severity = Severity::Warning,
                                          .message = std::move(message),
                                          .help = std::move(help),
                                          .primary = span,
                                          .source = source_});
        };
        const auto shadowsBuiltin = [](const std::string_view name)
        { return std::ranges::find(BuiltinTypeNames, name) != BuiltinTypeNames.end(); };
        for (const ParsedNamespace& definition : namespaces)
        {
            std::map<std::string_view, SourceSpan> entities;
            std::map<std::string_view, SourceSpan> commonTypes;
            for (const ParsedDeclaration& declaration : definition.declarations)
            {
                if (const auto* entity = std::get_if<ParsedEntity>(&declaration.content))
                {
                    for (const NamedIdentifier& name : entity->names) { entities.emplace(name.name.Text(), name.span); }
                }
                else if (const auto* common = std::get_if<ParsedCommonType>(&declaration.content))
                {
                    commonTypes.emplace(common->name.name.Text(), common->name.span);
                }
            }
            for (const auto& [name, span] : commonTypes)
            {
                if (entities.contains(name))
                {
                    warn(DiagnosticCode::ShadowsEntity, span, std::format("the common type `{}` shadows an entity type", name),
                         std::format("in this namespace, `{}` refers to the common type", name));
                }
                if (shadowsBuiltin(name))
                {
                    warn(DiagnosticCode::ShadowsBuiltin, span, std::format("the name `{}` shadows a built-in type", name),
                         std::format("refer to the built-in type as `__cedar::{}`", name));
                }
            }
            for (const auto& [name, span] : entities)
            {
                if (!shadowsBuiltin(name)) { continue; }
                warn(DiagnosticCode::ShadowsBuiltin, span, std::format("the name `{}` shadows a built-in type", name),
                     std::format("refer to the built-in type as `__cedar::{}`", name));
            }
        }
        return warnings;
    }

    [[nodiscard]] auto ConvertNamespace(const ParsedNamespace& parsed) -> NamespaceDefinition
    {
        NamespaceDefinition definition;
        definition.annotations = parsed.annotations;
        definition.span = ModelSpan{parsed.span};
        for (const ParsedDeclaration& declaration : parsed.declarations)
        {
            std::visit([&](const auto& content) { ConvertDeclaration(definition, declaration, content); },
                       declaration.content);
        }
        return definition;
    }

    void ConvertDeclaration(NamespaceDefinition& definition, const ParsedDeclaration& declaration,
                            const ParsedEntity& entity)
    {
        for (const NamedIdentifier& name : entity.names)
        {
            if (name.name.IsReserved())
            {
                Error(DiagnosticCode::ReservedNamespace, name.span,
                      std::format("`{}` is reserved and cannot name an entity type", ReservedNamespaceName));
                continue;
            }
            [[maybe_unused]] const bool inserted = InsertSorted(definition.entityTypes,
                                                                EntityTypeDeclaration{.name = name.name,
                                                                                      .kind = entity.kind,
                                                                                      .external = entity.external,
                                                                                      .annotations = declaration.annotations,
                                                                                      .span = ModelSpan{declaration.span}});
        }
    }

    void ConvertDeclaration(NamespaceDefinition& definition, const ParsedDeclaration& declaration,
                            const ParsedAction& action)
    {
        std::optional<AppliesTo> appliesTo;
        if (!action.external)
        {
            if (action.appliesTo)
            {
                appliesTo = ConvertApplications(action.names.front(), *action.appliesTo);
                if (!appliesTo) { return; }
            }
            else { appliesTo = AppliesTo{}; }
        }
        for (const NamedText& name : action.names)
        {
            [[maybe_unused]] const bool inserted = InsertSorted(definition.actions,
                                                                ActionDeclaration{.name = name.text,
                                                                                  .appliesTo = appliesTo,
                                                                                  .memberOf = action.parents,
                                                                                  .external = action.external,
                                                                                  .annotations = declaration.annotations,
                                                                                  .span = ModelSpan{declaration.span}});
        }
    }

    void ConvertDeclaration(NamespaceDefinition& definition, const ParsedDeclaration& declaration,
                            const ParsedCommonType& common)
    {
        const std::string_view name = common.name.name.Text();
        if (common.name.name.IsReserved())
        {
            Error(DiagnosticCode::ReservedNamespace, common.name.span,
                  std::format("`{}` is reserved and cannot name a common type", ReservedNamespaceName));
            return;
        }
        if (IsReservedCommonTypeName(name))
        {
            Error(DiagnosticCode::ReservedSchemaKeyword, common.name.span,
                  std::format("`{}` is a built-in type name and cannot name a common type", name));
            return;
        }
        [[maybe_unused]] const bool inserted = InsertSorted(definition.commonTypes,
                                                            CommonTypeDeclaration{.name = common.name.name,
                                                                                  .type = common.type,
                                                                                  .annotations = declaration.annotations,
                                                                                  .span = ModelSpan{declaration.span}});
    }

    /// Rust's `convert_app_decls`: each of principal, resource and context at most once, principal
    /// and resource present and not `[]`.
    [[nodiscard]] auto ConvertApplications(const NamedText& action, const std::vector<ParsedApplication>& applications)
        -> std::optional<AppliesTo>
    {
        std::optional<std::vector<TypeName>> principals;
        std::optional<std::vector<TypeName>> resources;
        std::optional<SchemaType> context;
        for (const ParsedApplication& application : applications)
        {
            if (application.kind == ApplicationKind::Context)
            {
                if (context)
                {
                    Error(DiagnosticCode::DuplicateContext, application.span,
                          std::format("action `{}` declares `context` twice", action.text));
                    return std::nullopt;
                }
                context.emplace(application.context);
                continue;
            }
            const bool principal = application.kind == ApplicationKind::Principal;
            std::optional<std::vector<TypeName>>& slot = principal ? principals : resources;
            const std::string_view word = principal ? "principal" : "resource";
            if (slot)
            {
                Error(DiagnosticCode::DuplicatePrincipalOrResource, application.span,
                      std::format("action `{}` declares `{}` twice", action.text, word));
                return std::nullopt;
            }
            if (application.entityTypes.empty())
            {
                Error(DiagnosticCode::EmptyList, application.span,
                      std::format("action `{}` declares an empty `{}` list", action.text, word));
                return std::nullopt;
            }
            slot = application.entityTypes;
        }
        if (!resources || !principals)
        {
            Error(DiagnosticCode::NoPrincipalOrResource, action.span,
                  std::format("action `{}` has `appliesTo` without `{}`", action.text, resources ? "principal" : "resource"));
            return std::nullopt;
        }
        return AppliesTo{.principalTypes = *std::move(principals),
                         .resourceTypes = *std::move(resources),
                         .context = context ? *std::move(context) : EmptyRecordType()};
    }

    SharedSourceText source_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

auto ParseCedarSchema(SharedSourceText source)
    -> std::expected<WithWarnings<std::shared_ptr<const SchemaFragmentModel>>, SchemaError>
{
    if (source->text.size() >= std::numeric_limits<std::uint32_t>::max())
    {
        return std::unexpected{SchemaError{Diagnostic{.code = DiagnosticCode::InvalidToken,
                                                      .message = "the schema text is 4 GiB or longer",
                                                      .source = source}}};
    }
    SchemaParser parser{source};
    std::vector<ParsedNamespace> namespaces = parser.ParseSchema();
    if (auto error = parser.TakeError()) { return std::unexpected{SchemaError{*std::move(error)}}; }
    return Converter{std::move(source)}.Convert(std::move(namespaces));
}

} // namespace detail

auto ParseSchemaFragment(const std::string_view text, const std::string_view sourceName)
    -> std::expected<WithWarnings<SchemaFragment>, SchemaError>
{
    auto parsed = detail::ParseCedarSchema(MakeSourceText(std::string{sourceName}, std::string{text}));
    if (!parsed) { return std::unexpected{std::move(parsed).error()}; }
    return WithWarnings<SchemaFragment>{detail::SchemaAccess::MakeFragment(std::move(parsed->value)),
                                        std::move(parsed->warnings)};
}

} // namespace cedar
