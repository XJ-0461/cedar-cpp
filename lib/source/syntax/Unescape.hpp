#pragma once

#include "syntax/Utf8.hpp"

#include <cedar/Diagnostic.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace cedar::detail
{

/// Why an escape in a string literal was refused. These are the fatal `EscapeError`s that Rust's
/// `rustc_literal_escaper::unescape_str` (0.0.8) can raise for a string, which is what Cedar uses
/// (`cedar-policy-core/src/parser/unescape.rs`), plus `InvalidUtf8` for input that did not come
/// through the lexer. All of them are the one diagnostic code `parse.invalid-escape`.
enum class UnescapeErrorKind : std::uint8_t
{
    LoneSlash,                      ///< `\` at the end of the content.
    InvalidEscape,                  ///< `\` followed by a character with no meaning (`\a`, `\*` outside a pattern).
    BareCarriageReturn,             ///< A raw carriage return; it must be written `\r`.
    EscapeOnlyChar,                 ///< A raw `"`; it must be written `\"`.
    TooShortHexEscape,              ///< `\x` with fewer than two characters after it.
    InvalidCharInHexEscape,         ///< `\x` followed by a non-hex digit.
    OutOfRangeHexEscape,            ///< `\x80` to `\xff`: a hex escape is ASCII only.
    NoBraceInUnicodeEscape,         ///< `\u` not followed by `{`.
    InvalidCharInUnicodeEscape,     ///< A character other than a hex digit, `_` or `}` inside `\u{...}`.
    EmptyUnicodeEscape,             ///< `\u{}`.
    UnclosedUnicodeEscape,          ///< `\u{` with no closing `}`.
    LeadingUnderscoreUnicodeEscape, ///< `\u{_...}`.
    OverlongUnicodeEscape,          ///< More than six hex digits (underscores are not counted).
    LoneSurrogateUnicodeEscape,     ///< A value from D800 to DFFF.
    OutOfRangeUnicodeEscape,        ///< A value above 10FFFF.
    InvalidUtf8,                    ///< Bytes that are not well-formed UTF-8.
};

/// A refused escape: why, and the bytes it covers, from the backslash to the last character the
/// escape consumed (Rust's callback range).
struct UnescapeError
{
    UnescapeErrorKind kind{};
    SourceSpan span{};

    friend constexpr auto operator==(const UnescapeError&, const UnescapeError&) noexcept -> bool = default;
};

/// A short description of `kind` for diagnostics.
[[nodiscard]] constexpr auto Describe(const UnescapeErrorKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case UnescapeErrorKind::LoneSlash: return "a backslash must be followed by an escape";
    case UnescapeErrorKind::InvalidEscape: return "unknown escape";
    case UnescapeErrorKind::BareCarriageReturn: return "a carriage return must be written as \\r";
    case UnescapeErrorKind::EscapeOnlyChar: return "a double quote must be written as \\\"";
    case UnescapeErrorKind::TooShortHexEscape: return "\\x must be followed by two hex digits";
    case UnescapeErrorKind::InvalidCharInHexEscape: return "invalid character in \\x escape";
    case UnescapeErrorKind::OutOfRangeHexEscape: return "\\x escapes must be at most \\x7f";
    case UnescapeErrorKind::NoBraceInUnicodeEscape: return "\\u must be followed by {";
    case UnescapeErrorKind::InvalidCharInUnicodeEscape: return "invalid character in \\u{...} escape";
    case UnescapeErrorKind::EmptyUnicodeEscape: return "empty \\u{} escape";
    case UnescapeErrorKind::UnclosedUnicodeEscape: return "unclosed \\u{ escape";
    case UnescapeErrorKind::LeadingUnderscoreUnicodeEscape: return "\\u{...} must not start with _";
    case UnescapeErrorKind::OverlongUnicodeEscape: return "\\u{...} has more than six hex digits";
    case UnescapeErrorKind::LoneSurrogateUnicodeEscape: return "\\u{...} names a surrogate, which is not a character";
    case UnescapeErrorKind::OutOfRangeUnicodeEscape: return "\\u{...} is above 10FFFF";
    case UnescapeErrorKind::InvalidUtf8: return "invalid UTF-8";
    }
    return "invalid escape";
}

namespace unescape
{

/// What the walk hands to its sink: a character, or (patterns only) a `\*` escape.
enum class Element : std::uint8_t
{
    Character,
    EscapedStar,
};

[[nodiscard]] constexpr auto HexDigit(const char32_t c) noexcept -> int
{
    if (c >= '0' && c <= '9') { return static_cast<int>(c - '0'); }
    if (c >= 'a' && c <= 'f') { return static_cast<int>(c - 'a') + 10; }
    if (c >= 'A' && c <= 'F') { return static_cast<int>(c - 'A') + 10; }
    return -1;
}

// Rust's `Unescape::unescape` for `str`, stopping at the first fatal error. `sink(element, scalar)`
// receives each character in order. `patternMode` turns `\*` from InvalidEscape into an
// EscapedStar element, as `to_pattern` does by matching the error's range against `\*`.
template <typename Sink>
[[nodiscard]] constexpr auto Walk(const std::string_view text, const std::uint32_t contentOffset, const bool patternMode,
                                  Sink&& sink) -> std::expected<void, UnescapeError>
{
    std::size_t position = 0;
    std::size_t start = 0;
    const auto fail = [&](const UnescapeErrorKind kind) -> std::unexpected<UnescapeError> {
        return std::unexpected{UnescapeError{
            kind, {contentOffset + static_cast<std::uint32_t>(start), contentOffset + static_cast<std::uint32_t>(position)}}};
    };
    // The next character, advancing past it; `ok` is false at the end of the text, and an
    // ill-formed byte is an error of its own.
    struct Next
    {
        char32_t scalar{};
        bool ok{};
    };
    bool invalidUtf8 = false;
    const auto next = [&]() -> Next {
        if (position == text.size()) { return {}; }
        const DecodedScalar decoded = DecodeUtf8(text, position);
        if (decoded.length == 0)
        {
            ++position;
            invalidUtf8 = true;
            return {};
        }
        position += decoded.length;
        return {decoded.scalar, true};
    };

    while (position < text.size())
    {
        start = position;
        const Next first = next();
        if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
        if (first.scalar == '"') { return fail(UnescapeErrorKind::EscapeOnlyChar); }
        if (first.scalar == '\r') { return fail(UnescapeErrorKind::BareCarriageReturn); }
        if (first.scalar != '\\')
        {
            sink(Element::Character, first.scalar);
            continue;
        }

        // A backslash before a line feed skips the line feed and the ASCII whitespace after it
        // (space, tab, line feed, carriage return). The lexer never produces this: its string
        // token refuses `\` before a line feed.
        if (position < text.size() && text[position] == '\n')
        {
            ++position;
            while (position < text.size() &&
                   (text[position] == ' ' || text[position] == '\t' || text[position] == '\n' || text[position] == '\r'))
            {
                ++position;
            }
            continue;
        }

        const Next escaped = next();
        if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
        if (!escaped.ok) { return fail(UnescapeErrorKind::LoneSlash); }
        switch (escaped.scalar)
        {
        case '0': sink(Element::Character, U'\0'); continue;
        case '"': sink(Element::Character, U'"'); continue;
        case 'n': sink(Element::Character, U'\n'); continue;
        case 'r': sink(Element::Character, U'\r'); continue;
        case 't': sink(Element::Character, U'\t'); continue;
        case '\\': sink(Element::Character, U'\\'); continue;
        case '\'': sink(Element::Character, U'\''); continue;
        case 'x':
        {
            int value = 0;
            for (int index = 0; index < 2; ++index)
            {
                const Next digit = next();
                if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
                if (!digit.ok) { return fail(UnescapeErrorKind::TooShortHexEscape); }
                const int digitValue = HexDigit(digit.scalar);
                if (digitValue < 0) { return fail(UnescapeErrorKind::InvalidCharInHexEscape); }
                value = value * 16 + digitValue;
            }
            if (value > 0x7F) { return fail(UnescapeErrorKind::OutOfRangeHexEscape); }
            sink(Element::Character, static_cast<char32_t>(value));
            continue;
        }
        case 'u':
        {
            const Next brace = next();
            if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
            if (!brace.ok || brace.scalar != '{') { return fail(UnescapeErrorKind::NoBraceInUnicodeEscape); }
            const Next head = next();
            if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
            if (!head.ok) { return fail(UnescapeErrorKind::UnclosedUnicodeEscape); }
            if (head.scalar == '_') { return fail(UnescapeErrorKind::LeadingUnderscoreUnicodeEscape); }
            if (head.scalar == '}') { return fail(UnescapeErrorKind::EmptyUnicodeEscape); }
            const int headValue = HexDigit(head.scalar);
            if (headValue < 0) { return fail(UnescapeErrorKind::InvalidCharInUnicodeEscape); }
            auto value = static_cast<std::uint32_t>(headValue);
            int digits = 1;
            while (true)
            {
                const Next c = next();
                if (invalidUtf8) { return fail(UnescapeErrorKind::InvalidUtf8); }
                if (!c.ok) { return fail(UnescapeErrorKind::UnclosedUnicodeEscape); }
                if (c.scalar == '_') { continue; }
                if (c.scalar == '}') { break; }
                const int digitValue = HexDigit(c.scalar);
                if (digitValue < 0) { return fail(UnescapeErrorKind::InvalidCharInUnicodeEscape); }
                ++digits;
                if (digits <= 6) { value = value * 16 + static_cast<std::uint32_t>(digitValue); }
            }
            if (digits > 6) { return fail(UnescapeErrorKind::OverlongUnicodeEscape); }
            if (value > 0x10FFFF) { return fail(UnescapeErrorKind::OutOfRangeUnicodeEscape); }
            if (value >= 0xD800 && value <= 0xDFFF) { return fail(UnescapeErrorKind::LoneSurrogateUnicodeEscape); }
            sink(Element::Character, static_cast<char32_t>(value));
            continue;
        }
        case '*':
            if (patternMode)
            {
                sink(Element::EscapedStar, U'*');
                continue;
            }
            return fail(UnescapeErrorKind::InvalidEscape);
        default: return fail(UnescapeErrorKind::InvalidEscape);
        }
    }
    return {};
}

} // namespace unescape

/// The value of a string literal's content (the text between its quotes, as ContentOf gives it),
/// following Rust's string escapes: `\n \r \t \\ \0 \' \"`, `\x00`-`\x7f`, `\u{...}` with one to
/// six hex digits and `_` separators after the first, and backslash-line-feed continuation. A raw
/// carriage return is refused. Error spans are `contentOffset` plus the byte offsets in `content`.
///
/// Only the first error is returned; Rust collects every error, but parity is accept or reject.
[[nodiscard]] constexpr auto Unescape(const std::string_view content, const std::uint32_t contentOffset = 0)
    -> std::expected<std::string, UnescapeError>
{
    std::string result;
    result.reserve(content.size());
    const auto walked = unescape::Walk(content, contentOffset, false,
                                       [&](unescape::Element, const char32_t scalar) { AppendUtf8(result, scalar); });
    if (!walked) { return std::unexpected{walked.error()}; }
    return result;
}

/// The literal runs of a `like` pattern's content, in the form Pattern stores: n wildcards give
/// n + 1 literals. Escapes are those of Unescape plus `\*`, a literal star. Every other `*`
/// is a wildcard, including one written as `\u{2a}` or `\x2a` (`unescape.rs`, `to_pattern`).
[[nodiscard]] constexpr auto UnescapePattern(const std::string_view content, const std::uint32_t contentOffset = 0)
    -> std::expected<std::vector<std::string>, UnescapeError>
{
    std::vector<std::string> literals(1);
    const auto walked =
        unescape::Walk(content, contentOffset, true, [&](const unescape::Element element, const char32_t scalar) {
            if (element == unescape::Element::Character && scalar == U'*')
            {
                literals.emplace_back();
                return;
            }
            AppendUtf8(literals.back(), scalar);
        });
    if (!walked) { return std::unexpected{walked.error()}; }
    return literals;
}

} // namespace cedar::detail
