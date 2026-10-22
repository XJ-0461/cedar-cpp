#pragma once

#include "syntax/Utf8.hpp"

#include <cedar/Diagnostic.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace cedar::detail
{

/// The terminals of the policy grammar (`cedar-policy-core/src/parser/grammar.lalrpop`, the `match`
/// block), plus end of input and the three ways a lexical error is reported.
///
/// Every identifier-shaped word is one token. A word that the grammar names as a terminal gets its
/// own kind (`permit`, `if`, `principal`, ...); every other word is `Identifier`. Whether a keyword
/// may stand where an identifier is expected is the parser's call: see IsAnyIdentifier and
/// IsCommonIdentifier.
enum class TokenKind : std::uint8_t
{
    EndOfInput,

    Identifier, ///< `[_a-zA-Z][_a-zA-Z0-9]*` that is none of the words below.
    True,
    False,
    If,
    Permit,
    Forbid,
    When,
    Unless,
    In,
    Has,
    Like,
    Is,
    Then,
    Else,
    Principal,
    Action,
    Resource,
    Context,

    PrincipalSlot, ///< `?principal`
    ResourceSlot,  ///< `?resource`
    OtherSlot,     ///< `?` followed by any other identifier; the parser rejects it.

    Number, ///< `[0-9]+`; ParseNumberLiteral gives the value.
    String, ///< `"` ... `"` with escapes still in place; StringContent gives the text inside the quotes.

    At,
    Dot,
    Comma,
    Semicolon,
    Colon,
    DoubleColon,
    LeftParenthesis,
    RightParenthesis,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    EqualEqual,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    OrOr,
    AndAnd,
    Plus,
    Minus,
    Star,
    Slash,   ///< Lexed so the parser can report that division is unsupported.
    Percent, ///< Lexed so the parser can report that remainder is unsupported.
    Bang,
    Equal, ///< A single `=`, lexed so the parser can suggest `==`.

    InvalidCharacter,   ///< A character that starts no token (`$`, `&`, `?` alone, a non-ASCII letter).
    InvalidString,      ///< A `"` with no well-formed string after it: unterminated, or `\` before a line feed.
    InvalidUtf8,        ///< Bytes that are not well-formed UTF-8.
};

/// One token: its kind and the bytes it covers in the lexed text.
struct Token
{
    TokenKind kind{TokenKind::EndOfInput};
    SourceSpan span{};

    /// The token's text within `source`, the text it was lexed from.
    [[nodiscard]] constexpr auto Text(const std::string_view source) const noexcept -> std::string_view
    {
        return source.substr(span.begin, span.Length());
    }

    friend constexpr auto operator==(const Token&, const Token&) noexcept -> bool = default;
};

/// True for the error kinds. Every lexical error is fatal to the parse, as in Rust, where the
/// generated lexer stops at the first invalid token.
[[nodiscard]] constexpr auto IsLexicalError(const TokenKind kind) noexcept -> bool
{
    return kind == TokenKind::InvalidCharacter || kind == TokenKind::InvalidString || kind == TokenKind::InvalidUtf8;
}

/// True for the grammar's `CommonIdent`: `Identifier` and every keyword kind except `true`,
/// `false` and `if`. Unqualified names (`Primary`'s `Name`, the last component of `Name` when it has
/// no path) must be one of these.
[[nodiscard]] constexpr auto IsCommonIdentifier(const TokenKind kind) noexcept -> bool
{
    return kind == TokenKind::Identifier || (kind >= TokenKind::Permit && kind <= TokenKind::Context);
}

/// True for the grammar's `AnyIdent`: every identifier-shaped word, keywords included. Annotation
/// keys, attribute names after `.`, `RefInit` keys, path components before `::`, the effect, the
/// scope variables and the condition keyword are all `AnyIdent` in the grammar; the conversion to
/// the AST then checks which word it is (Rust `cst_to_ast.rs`).
[[nodiscard]] constexpr auto IsAnyIdentifier(const TokenKind kind) noexcept -> bool
{
    return kind >= TokenKind::Identifier && kind <= TokenKind::Context;
}

/// The fixed text of a punctuation or keyword kind, or a short description of the others, for
/// diagnostics ("expected `)`, found identifier").
[[nodiscard]] constexpr auto Describe(const TokenKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case TokenKind::EndOfInput: return "end of input";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::True: return "true";
    case TokenKind::False: return "false";
    case TokenKind::If: return "if";
    case TokenKind::Permit: return "permit";
    case TokenKind::Forbid: return "forbid";
    case TokenKind::When: return "when";
    case TokenKind::Unless: return "unless";
    case TokenKind::In: return "in";
    case TokenKind::Has: return "has";
    case TokenKind::Like: return "like";
    case TokenKind::Is: return "is";
    case TokenKind::Then: return "then";
    case TokenKind::Else: return "else";
    case TokenKind::Principal: return "principal";
    case TokenKind::Action: return "action";
    case TokenKind::Resource: return "resource";
    case TokenKind::Context: return "context";
    case TokenKind::PrincipalSlot: return "?principal";
    case TokenKind::ResourceSlot: return "?resource";
    case TokenKind::OtherSlot: return "slot";
    case TokenKind::Number: return "number";
    case TokenKind::String: return "string literal";
    case TokenKind::At: return "@";
    case TokenKind::Dot: return ".";
    case TokenKind::Comma: return ",";
    case TokenKind::Semicolon: return ";";
    case TokenKind::Colon: return ":";
    case TokenKind::DoubleColon: return "::";
    case TokenKind::LeftParenthesis: return "(";
    case TokenKind::RightParenthesis: return ")";
    case TokenKind::LeftBrace: return "{";
    case TokenKind::RightBrace: return "}";
    case TokenKind::LeftBracket: return "[";
    case TokenKind::RightBracket: return "]";
    case TokenKind::EqualEqual: return "==";
    case TokenKind::NotEqual: return "!=";
    case TokenKind::Less: return "<";
    case TokenKind::LessEqual: return "<=";
    case TokenKind::Greater: return ">";
    case TokenKind::GreaterEqual: return ">=";
    case TokenKind::OrOr: return "||";
    case TokenKind::AndAnd: return "&&";
    case TokenKind::Plus: return "+";
    case TokenKind::Minus: return "-";
    case TokenKind::Star: return "*";
    case TokenKind::Slash: return "/";
    case TokenKind::Percent: return "%";
    case TokenKind::Bang: return "!";
    case TokenKind::Equal: return "=";
    case TokenKind::InvalidCharacter: return "invalid character";
    case TokenKind::InvalidString: return "invalid string literal";
    case TokenKind::InvalidUtf8: return "invalid UTF-8";
    }
    return "token";
}

/// The kind of an identifier-shaped word: its keyword kind, or `Identifier`.
[[nodiscard]] constexpr auto ClassifyWord(const std::string_view word) noexcept -> TokenKind
{
    constexpr TokenKind first = TokenKind::True;
    constexpr TokenKind last = TokenKind::Context;
    for (auto kind = static_cast<std::uint8_t>(first); kind <= static_cast<std::uint8_t>(last); ++kind)
    {
        if (word == Describe(static_cast<TokenKind>(kind))) { return static_cast<TokenKind>(kind); }
    }
    return TokenKind::Identifier;
}

/// The value of a `Number` token's text, or nullopt when it does not fit in 64 unsigned bits. Rust
/// parses the token as `u64` and rejects the literal on overflow (`grammar.lalrpop`, `Literal`).
/// The i64 range check, with `-9223372036854775808` allowed only as a negation applied directly to
/// the literal, is the parser's (`cst_to_ast.rs`, `NegOp::Dash`).
[[nodiscard]] constexpr auto ParseNumberLiteral(const std::string_view digits) noexcept -> std::optional<std::uint64_t>
{
    constexpr std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t value = 0;
    for (const char digit : digits)
    {
        const auto add = static_cast<std::uint64_t>(digit - '0');
        if (value > (maximum - add) / 10) { return std::nullopt; }
        value = value * 10 + add;
    }
    return value;
}

/// The text between the quotes of a `String` token, with the escapes still in place, and the
/// offset of its first byte in the lexed text (what Unescape takes as `contentOffset`).
struct StringContent
{
    std::string_view text;
    std::uint32_t offset{};
};

/// The content of `token`, which must be a `String` token lexed from `source`.
[[nodiscard]] constexpr auto ContentOf(const Token& token, const std::string_view source) noexcept -> StringContent
{
    return {source.substr(token.span.begin + 1, token.span.Length() - 2), token.span.begin + 1};
}

/// A lexer over policy text. Whitespace (Unicode `White_Space`, as Rust regex `\s`) and `//`
/// comments are skipped. Each token is the longest match; a keyword wins over the identifier of
/// the same text, and `?principal` / `?resource` over the other-slot form, as in LALRPOP.
///
/// After a lexical error the lexer resumes after the error token, so it never stops early; the
/// parser stops at the first error. Precondition: `text.size()` is below 4 GiB (spans are 32-bit).
class Lexer final
{
public:
    explicit constexpr Lexer(const std::string_view text) noexcept : text_{text} {}

    /// The next token. At the end it returns `EndOfInput` (an empty span at the end) every time.
    [[nodiscard]] constexpr auto Next() noexcept -> Token
    {
        SkipTrivia();
        const std::size_t begin = position_;
        if (begin == text_.size()) { return Make(TokenKind::EndOfInput, begin); }

        const char c = text_[begin];
        if (IsWordHead(c))
        {
            ScanWordTail(begin + 1);
            return Make(ClassifyWord(text_.substr(begin, position_ - begin)), begin);
        }
        if (c >= '0' && c <= '9')
        {
            position_ = begin + 1;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') { ++position_; }
            return Make(TokenKind::Number, begin);
        }
        if (c == '"') { return ScanString(begin); }
        if (c == '?' && begin + 1 < text_.size() && IsWordHead(text_[begin + 1]))
        {
            ScanWordTail(begin + 2);
            const std::string_view slot = text_.substr(begin, position_ - begin);
            if (slot == "?principal") { return Make(TokenKind::PrincipalSlot, begin); }
            if (slot == "?resource") { return Make(TokenKind::ResourceSlot, begin); }
            return Make(TokenKind::OtherSlot, begin);
        }

        const char following = begin + 1 < text_.size() ? text_[begin + 1] : '\0';
        const auto one = [&](const TokenKind kind) {
            position_ = begin + 1;
            return Make(kind, begin);
        };
        const auto two = [&](const TokenKind kind) {
            position_ = begin + 2;
            return Make(kind, begin);
        };
        switch (c)
        {
        case '@': return one(TokenKind::At);
        case '.': return one(TokenKind::Dot);
        case ',': return one(TokenKind::Comma);
        case ';': return one(TokenKind::Semicolon);
        case ':': return following == ':' ? two(TokenKind::DoubleColon) : one(TokenKind::Colon);
        case '(': return one(TokenKind::LeftParenthesis);
        case ')': return one(TokenKind::RightParenthesis);
        case '{': return one(TokenKind::LeftBrace);
        case '}': return one(TokenKind::RightBrace);
        case '[': return one(TokenKind::LeftBracket);
        case ']': return one(TokenKind::RightBracket);
        case '=': return following == '=' ? two(TokenKind::EqualEqual) : one(TokenKind::Equal);
        case '!': return following == '=' ? two(TokenKind::NotEqual) : one(TokenKind::Bang);
        case '<': return following == '=' ? two(TokenKind::LessEqual) : one(TokenKind::Less);
        case '>': return following == '=' ? two(TokenKind::GreaterEqual) : one(TokenKind::Greater);
        case '|':
            if (following == '|') { return two(TokenKind::OrOr); }
            break;
        case '&':
            if (following == '&') { return two(TokenKind::AndAnd); }
            break;
        case '+': return one(TokenKind::Plus);
        case '-': return one(TokenKind::Minus);
        case '*': return one(TokenKind::Star);
        case '/': return one(TokenKind::Slash); // `//` was consumed by SkipTrivia.
        case '%': return one(TokenKind::Percent);
        default: break;
        }

        const DecodedScalar decoded = DecodeUtf8(text_, begin);
        if (decoded.length == 0)
        {
            position_ = begin + 1;
            return Make(TokenKind::InvalidUtf8, begin);
        }
        position_ = begin + decoded.length;
        return Make(TokenKind::InvalidCharacter, begin);
    }

private:
    [[nodiscard]] static constexpr auto IsWordHead(const char c) noexcept -> bool
    {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    [[nodiscard]] static constexpr auto IsWordTail(const char c) noexcept -> bool
    {
        return IsWordHead(c) || (c >= '0' && c <= '9');
    }

    constexpr void ScanWordTail(const std::size_t from) noexcept
    {
        position_ = from;
        while (position_ < text_.size() && IsWordTail(text_[position_])) { ++position_; }
    }

    [[nodiscard]] constexpr auto Make(const TokenKind kind, const std::size_t begin) const noexcept -> Token
    {
        return {kind, {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(position_)}};
    }

    // Skips whitespace and `//` comments. A comment runs to the next `\n` or `\r` (grammar
    // `//[^\n\r]*`); U+2028 and other Unicode line breaks do not end it. Stops at bytes that are not
    // well-formed UTF-8, inside a comment too, so that Next reports them.
    constexpr void SkipTrivia() noexcept
    {
        while (position_ < text_.size())
        {
            const char c = text_[position_];
            if (c == '/' && position_ + 1 < text_.size() && text_[position_ + 1] == '/')
            {
                std::size_t cursor = position_ + 2;
                while (cursor < text_.size() && text_[cursor] != '\n' && text_[cursor] != '\r')
                {
                    const DecodedScalar decoded = DecodeUtf8(text_, cursor);
                    if (decoded.length == 0)
                    {
                        position_ = cursor;
                        return;
                    }
                    cursor += decoded.length;
                }
                position_ = cursor;
                continue;
            }
            const DecodedScalar decoded = DecodeUtf8(text_, position_);
            if (decoded.length == 0 || !IsUnicodeWhiteSpace(decoded.scalar)) { return; }
            position_ += decoded.length;
        }
    }

    // A string literal is `"(\\.|[^"\\])*"`, where `.` is any character but a line feed. A
    // backslash takes the next character whatever it is, so `\"` does not end the string; the
    // escape itself is checked later by Unescape. A backslash before a line feed or the end of input,
    // or a missing closing quote, makes the whole attempt one InvalidString token.
    [[nodiscard]] constexpr auto ScanString(const std::size_t begin) noexcept -> Token
    {
        std::size_t cursor = begin + 1;
        while (cursor < text_.size())
        {
            const char c = text_[cursor];
            if (c == '"')
            {
                position_ = cursor + 1;
                return Make(TokenKind::String, begin);
            }
            if (c == '\\')
            {
                if (cursor + 1 == text_.size() || text_[cursor + 1] == '\n')
                {
                    position_ = cursor + 1;
                    return Make(TokenKind::InvalidString, begin);
                }
                ++cursor;
            }
            const DecodedScalar decoded = DecodeUtf8(text_, cursor);
            if (decoded.length == 0)
            {
                position_ = cursor + 1;
                return Make(TokenKind::InvalidUtf8, begin);
            }
            cursor += decoded.length;
        }
        position_ = cursor;
        return Make(TokenKind::InvalidString, begin);
    }

    std::string_view text_;
    std::size_t position_{};
};

/// Every token of `text`, ending with exactly one `EndOfInput`. Lexical errors are included as
/// tokens; lexing continues after them.
[[nodiscard]] constexpr auto Tokenize(const std::string_view text) -> std::vector<Token>
{
    std::vector<Token> tokens;
    Lexer lexer{text};
    while (true)
    {
        tokens.push_back(lexer.Next());
        if (tokens.back().kind == TokenKind::EndOfInput) { return tokens; }
    }
}

} // namespace cedar::detail
