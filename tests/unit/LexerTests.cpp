// The lexer and string unescape tables. Each table is checked twice: by static_assert at compile
// time, and by a gtest at run time (so the same code also runs under the sanitizers). Expected
// behaviour comes from `cedar-policy-core/src/parser/grammar.lalrpop`, the tests in
// `cedar-policy-core/src/parser/unescape.rs`, and the reference CLI's `check-parse` (whitespace set,
// strings with a backslash before a line feed).

#include "syntax/Lexer.hpp"
#include "syntax/Unescape.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace
{

using namespace std::string_view_literals;
using cedar::detail::ContentOf;
using cedar::detail::IsAnyIdentifier;
using cedar::detail::IsCommonIdentifier;
using cedar::detail::IsLexicalError;
using cedar::detail::ParseNumberLiteral;
using cedar::detail::Tokenize;
using cedar::detail::TokenKind;
using cedar::detail::Unescape;
using cedar::detail::UnescapeErrorKind;
using cedar::detail::UnescapePattern;
using enum cedar::detail::TokenKind;

// ---- Token kinds ----

// Expected kinds in order; the unused tail is EndOfInput, which also ends the comparison.
struct KindCase
{
    std::string_view text;
    std::array<TokenKind, 32> kinds{};
};

constexpr std::array KindCases{
    KindCase{"", {}},
    KindCase{"permit(principal, action, resource);",
             {Permit, LeftParenthesis, Principal, Comma, Action, Comma, Resource, RightParenthesis, Semicolon}},
    KindCase{"true false if permit forbid when unless in has like is then else principal action resource context",
             {True, False, If, Permit, Forbid, When, Unless, In, Has, Like, Is, Then, Else, Principal, Action, Resource,
              Context}},
    KindCase{"trueX _if Permit IF __cedar a_9Z", {Identifier, Identifier, Identifier, Identifier, Identifier, Identifier}},
    KindCase{"?principal ?resource ?principalx ?foo ?_ ? x ?1",
             {PrincipalSlot, ResourceSlot, OtherSlot, OtherSlot, OtherSlot, InvalidCharacter, Identifier, InvalidCharacter,
              Number}},
    KindCase{"== != < <= > >= && || ! + - * / % = . , ; : :: ( ) [ ] { } @",
             {EqualEqual, NotEqual,     Less,      LessEqual,       Greater,          GreaterEqual, AndAnd,
              OrOr,       Bang,         Plus,      Minus,           Star,             Slash,     Percent,
              Equal,      Dot,          Comma,     Semicolon,       Colon,            DoubleColon,
              LeftParenthesis, RightParenthesis, LeftBracket, RightBracket, LeftBrace, RightBrace, At}},
    // Longest match, token by token, with no separator needed.
    KindCase{"a<=b==c!=d", {Identifier, LessEqual, Identifier, EqualEqual, Identifier, NotEqual, Identifier}},
    KindCase{":::: ::: === !== !!", {DoubleColon, DoubleColon, DoubleColon, Colon, EqualEqual, Equal, NotEqual, Equal, Bang,
                                     Bang}},
    KindCase{"1is_x 007 1.5", {Number, Identifier, Number, Number, Dot, Number}},
    KindCase{"-9223372036854775808", {Minus, Number}},
    KindCase{"User::\"alice\"", {Identifier, DoubleColon, String}},
    KindCase{"x.if.principal", {Identifier, Dot, If, Dot, Principal}},
    // Comments run to `\n` or `\r` only; U+2028 does not end one.
    KindCase{"a//b\rc", {Identifier, Identifier}},
    KindCase{"a // b \xE2\x80\xA8 ;", {Identifier}},
    KindCase{"a/b //", {Identifier, Slash, Identifier}},
    // Unicode White_Space is skipped: NBSP, LS, NEL, VT, FF, ideographic space, ogham space, NNBSP.
    KindCase{"\xC2\xA0\xE2\x80\xA8\xC2\x85\v\f\xE3\x80\x80\xE1\x9A\x80\xE2\x80\xAF\t\r\na", {Identifier}},
    // Not White_Space: zero width space, BOM, Mongolian vowel separator, information separator.
    KindCase{"\xE2\x80\x8B", {InvalidCharacter}},
    KindCase{"\xEF\xBB\xBF" "a", {InvalidCharacter, Identifier}},
    KindCase{"\xE1\xA0\x8E", {InvalidCharacter}},
    KindCase{"\x1C", {InvalidCharacter}},
    KindCase{"$ & | \xC3\xA9", {InvalidCharacter, InvalidCharacter, InvalidCharacter, InvalidCharacter}},
    KindCase{"\"a\\\"b\" \"\\\r\" \"\\\\\"", {String, String, String}},
    KindCase{"\"abc", {InvalidString}},
    KindCase{"\"a\\\nb\"", {InvalidString, Identifier, InvalidString}},
    KindCase{"\"a\\", {InvalidString}},
    KindCase{"\xFF" "a", {InvalidUtf8, Identifier}},
    KindCase{"\"a\xFF\"", {InvalidUtf8, InvalidString}},
};

constexpr auto KindsMatch(const KindCase& testCase) -> bool
{
    const auto tokens = Tokenize(testCase.text);
    for (std::size_t index = 0; index < testCase.kinds.size(); ++index)
    {
        if (index >= tokens.size() || tokens[index].kind != testCase.kinds[index]) { return false; }
        if (testCase.kinds[index] == EndOfInput) { return index + 1 == tokens.size(); }
    }
    return false;
}

constexpr auto AllKindCasesMatch() -> bool
{
    for (const KindCase& testCase : KindCases)
    {
        if (!KindsMatch(testCase)) { return false; }
    }
    return true;
}

static_assert(AllKindCasesMatch());

// ---- Token spans ----

struct ExpectedToken
{
    TokenKind kind{};
    std::uint32_t begin{};
    std::uint32_t end{};
};

// Expected tokens in order, ending with the explicit EndOfInput token.
struct SpanCase
{
    std::string_view text;
    std::array<ExpectedToken, 8> tokens{};
};

constexpr std::array SpanCases{
    SpanCase{"", {{{EndOfInput, 0, 0}}}},
    SpanCase{"  ", {{{EndOfInput, 2, 2}}}},
    SpanCase{"a == \"b\\\"c\" // x\n 12",
             {{{Identifier, 0, 1}, {EqualEqual, 2, 4}, {String, 5, 11}, {Number, 18, 20}, {EndOfInput, 20, 20}}}},
    SpanCase{"?principal::?x", {{{PrincipalSlot, 0, 10}, {DoubleColon, 10, 12}, {OtherSlot, 12, 14}, {EndOfInput, 14, 14}}}},
    SpanCase{"\xC3\xA9x", {{{InvalidCharacter, 0, 2}, {Identifier, 2, 3}, {EndOfInput, 3, 3}}}},
    SpanCase{"\"abc", {{{InvalidString, 0, 4}, {EndOfInput, 4, 4}}}},
    SpanCase{"\"a\\\n\"", {{{InvalidString, 0, 3}, {InvalidString, 4, 5}, {EndOfInput, 5, 5}}}},
    SpanCase{"\"a\xFF\"", {{{InvalidUtf8, 0, 3}, {InvalidString, 3, 4}, {EndOfInput, 4, 4}}}},
    SpanCase{"// \xFF\n", {{{InvalidUtf8, 3, 4}, {EndOfInput, 5, 5}}}},
    SpanCase{"\xE2\x80\x8B;", {{{InvalidCharacter, 0, 3}, {Semicolon, 3, 4}, {EndOfInput, 4, 4}}}},
};

constexpr auto SpansMatch(const SpanCase& testCase) -> bool
{
    const auto tokens = Tokenize(testCase.text);
    for (std::size_t index = 0; index < testCase.tokens.size(); ++index)
    {
        const ExpectedToken& expected = testCase.tokens[index];
        if (index >= tokens.size()) { return false; }
        const auto& actual = tokens[index];
        if (actual.kind != expected.kind || actual.span.begin != expected.begin || actual.span.end != expected.end)
        {
            return false;
        }
        if (expected.kind == EndOfInput) { return index + 1 == tokens.size(); }
    }
    return false;
}

constexpr auto AllSpanCasesMatch() -> bool
{
    for (const SpanCase& testCase : SpanCases)
    {
        if (!SpansMatch(testCase)) { return false; }
    }
    return true;
}

static_assert(AllSpanCasesMatch());

// ---- Token helpers ----

static_assert(IsAnyIdentifier(Identifier) && IsAnyIdentifier(True) && IsAnyIdentifier(If) && IsAnyIdentifier(Context));
static_assert(!IsAnyIdentifier(PrincipalSlot) && !IsAnyIdentifier(String) && !IsAnyIdentifier(EndOfInput));
static_assert(IsCommonIdentifier(Identifier) && IsCommonIdentifier(Permit) && IsCommonIdentifier(Has) &&
              IsCommonIdentifier(Context));
static_assert(!IsCommonIdentifier(True) && !IsCommonIdentifier(False) && !IsCommonIdentifier(If));
static_assert(IsLexicalError(InvalidCharacter) && IsLexicalError(InvalidString) && IsLexicalError(InvalidUtf8));
static_assert(!IsLexicalError(EndOfInput) && !IsLexicalError(Equal));

static_assert(ParseNumberLiteral("0") == 0);
static_assert(ParseNumberLiteral("007") == 7);
static_assert(ParseNumberLiteral("9223372036854775808") == 9223372036854775808ULL);
static_assert(ParseNumberLiteral("18446744073709551615") == 18446744073709551615ULL);
static_assert(ParseNumberLiteral("18446744073709551616") == std::nullopt);
static_assert(ParseNumberLiteral("99999999999999999999") == std::nullopt);

constexpr auto ContentOfWorks() -> bool
{
    constexpr std::string_view text = "x == \"a\\tb\"";
    const auto tokens = Tokenize(text);
    const auto content = ContentOf(tokens[2], text);
    return content.text == "a\\tb" && content.offset == 6;
}

static_assert(ContentOfWorks());

// ---- Unescape ----

struct UnescapeCase
{
    std::string_view content;
    std::optional<std::string_view> value;  // set when unescaping succeeds
    UnescapeErrorKind error{};               // otherwise the first error ...
    std::uint32_t begin{};                   // ... and its span
    std::uint32_t end{};
};

constexpr auto Ok(const std::string_view content, const std::string_view value) -> UnescapeCase
{
    return {content, value};
}

constexpr auto Error(const std::string_view content, const UnescapeErrorKind error, const std::uint32_t begin,
                     const std::uint32_t end) -> UnescapeCase
{
    return {content, std::nullopt, error, begin, end};
}

constexpr std::array UnescapeCases{
    // unescape.rs test_string_escape
    Ok(R"(\t\r\n\\\0\x42)", "\t\r\n\\\0B"sv),
    Error(R"(abc\xFFdef)", UnescapeErrorKind::OutOfRangeHexEscape, 3, 7),
    Ok(R"(\u{0}\u{1}\u{1234}\u{12345}\u{054321}\u{123}\u{42})",
       "\0\x01\xE1\x88\xB4\xF0\x92\x8D\x85\xF1\x94\x8C\xA1\xC4\xA3" "B"sv),
    Error(R"(abc\u{1111111}\u{222222222}FFdef)", UnescapeErrorKind::OverlongUnicodeEscape, 3, 14),
    Error(R"(abc\*\bdef)", UnescapeErrorKind::InvalidEscape, 3, 5),
    // unescape.rs multiple_skipped_warning and unskipped_whitespace_warning (warnings only)
    Ok("lines\\\n\n\nskipped", "linesskipped"),
    Ok("unicode space\\\n\xE2\x80\x81\nnot skipped", "unicode space\xE2\x80\x81\nnot skipped"),
    Ok("a\\\n \t\r\n b", "ab"),
    // The rest of the escape set
    Ok("", ""),
    Ok(R"(\'\")", "'\""),
    Ok(R"(\x7f\x7F\x00)", "\x7F\x7F\0"sv),
    Ok(R"(\u{4_1}\u{000041}\u{10FFFF}\u{10ffff})", "AA\xF4\x8F\xBF\xBF\xF4\x8F\xBF\xBF"),
    Ok("raw\nline feed and \xC3\xA9", "raw\nline feed and \xC3\xA9"),
    Ok("\\u{2a}*", "**"),
    Error("\\", UnescapeErrorKind::LoneSlash, 0, 1),
    Error("a\rb", UnescapeErrorKind::BareCarriageReturn, 1, 2),
    Error("a\"", UnescapeErrorKind::EscapeOnlyChar, 1, 2),
    Error(R"(\x4)", UnescapeErrorKind::TooShortHexEscape, 0, 3),
    Error(R"(\xg1)", UnescapeErrorKind::InvalidCharInHexEscape, 0, 3),
    Error(R"(\x80)", UnescapeErrorKind::OutOfRangeHexEscape, 0, 4),
    Error(R"(\u41)", UnescapeErrorKind::NoBraceInUnicodeEscape, 0, 3),
    Error(R"(\u)", UnescapeErrorKind::NoBraceInUnicodeEscape, 0, 2),
    Error(R"(\u{4g})", UnescapeErrorKind::InvalidCharInUnicodeEscape, 0, 5),
    Error(R"(\u{})", UnescapeErrorKind::EmptyUnicodeEscape, 0, 4),
    Error(R"(\u{41)", UnescapeErrorKind::UnclosedUnicodeEscape, 0, 5),
    Error(R"(\u{)", UnescapeErrorKind::UnclosedUnicodeEscape, 0, 3),
    Error(R"(\u{_41})", UnescapeErrorKind::LeadingUnderscoreUnicodeEscape, 0, 4),
    Error(R"(\u{0000041})", UnescapeErrorKind::OverlongUnicodeEscape, 0, 11),
    Error(R"(\u{d800})", UnescapeErrorKind::LoneSurrogateUnicodeEscape, 0, 8),
    Error(R"(\u{DFFF})", UnescapeErrorKind::LoneSurrogateUnicodeEscape, 0, 8),
    Error(R"(\u{110000})", UnescapeErrorKind::OutOfRangeUnicodeEscape, 0, 10),
    Error("\\\xC3\xA9", UnescapeErrorKind::InvalidEscape, 0, 3),
    Error(R"(\b)", UnescapeErrorKind::InvalidEscape, 0, 2),
    Error("\xFF", UnescapeErrorKind::InvalidUtf8, 0, 1),
    Error("ab\xC3", UnescapeErrorKind::InvalidUtf8, 2, 3),
};

constexpr auto UnescapeMatches(const UnescapeCase& testCase, const std::uint32_t contentOffset) -> bool
{
    const auto result = Unescape(testCase.content, contentOffset);
    if (testCase.value) { return result.has_value() && *result == *testCase.value; }
    return !result.has_value() && result.error().kind == testCase.error &&
           result.error().span.begin == testCase.begin + contentOffset &&
           result.error().span.end == testCase.end + contentOffset;
}

constexpr auto AllUnescapeCasesMatch() -> bool
{
    for (const UnescapeCase& testCase : UnescapeCases)
    {
        if (!UnescapeMatches(testCase, 0) || !UnescapeMatches(testCase, 100)) { return false; }
    }
    return true;
}

static_assert(AllUnescapeCasesMatch());

// ---- Pattern unescape ----

struct PatternCase
{
    std::string_view content;
    std::size_t literalCount{};  // zero when unescaping fails
    std::array<std::string_view, 4> literals{};
    UnescapeErrorKind error{};
    std::uint32_t begin{};
    std::uint32_t end{};
};

constexpr std::array PatternCases{
    // unescape.rs test_pattern_escape
    PatternCase{R"(\t\r\n\\\0\x42\*)", 1, {"\t\r\n\\\0B*"sv}},
    PatternCase{R"(abc\xFF\xFEdef)", 0, {}, UnescapeErrorKind::OutOfRangeHexEscape, 3, 7},
    PatternCase{"\xF0\x9F\x91\x80\xF0\x9F\x91\x80\\*\xF0\x9F\xA4\x9E\xF0\x9F\xA4\x9E\\*\xF0\x9F\xA4\x9D",
                1,
                {"\xF0\x9F\x91\x80\xF0\x9F\x91\x80*\xF0\x9F\xA4\x9E\xF0\x9F\xA4\x9E*\xF0\x9F\xA4\x9D"}},
    PatternCase{R"(abc\d\bdef)", 0, {}, UnescapeErrorKind::InvalidEscape, 3, 5},
    // The shapes Pattern documents
    PatternCase{"", 1, {""}},
    PatternCase{"ab", 1, {"ab"}},
    PatternCase{"a*b", 2, {"a", "b"}},
    PatternCase{"*", 2, {"", ""}},
    PatternCase{"a**", 3, {"a", "", ""}},
    // Only `\*` is a literal star; a star from any other escape is a wildcard.
    PatternCase{R"(\u{2a})", 2, {"", ""}},
    PatternCase{R"(\x2a)", 2, {"", ""}},
    PatternCase{R"(a\**\*)", 2, {"a*", "*"}},
    PatternCase{"\\", 0, {}, UnescapeErrorKind::LoneSlash, 0, 1},
};

constexpr auto PatternMatches(const PatternCase& testCase) -> bool
{
    const auto result = UnescapePattern(testCase.content);
    if (testCase.literalCount == 0)
    {
        return !result.has_value() && result.error().kind == testCase.error && result.error().span.begin == testCase.begin &&
               result.error().span.end == testCase.end;
    }
    if (!result.has_value() || result->size() != testCase.literalCount) { return false; }
    for (std::size_t index = 0; index < testCase.literalCount; ++index)
    {
        if ((*result)[index] != testCase.literals[index]) { return false; }
    }
    return true;
}

constexpr auto AllPatternCasesMatch() -> bool
{
    for (const PatternCase& testCase : PatternCases)
    {
        if (!PatternMatches(testCase)) { return false; }
    }
    return true;
}

static_assert(AllPatternCasesMatch());

// ---- The same tables at run time ----

TEST(LexerTests, TokenKinds)
{
    for (const KindCase& testCase : KindCases)
    {
        EXPECT_TRUE(KindsMatch(testCase)) << "text: " << testCase.text;
    }
}

TEST(LexerTests, TokenSpans)
{
    for (const SpanCase& testCase : SpanCases)
    {
        EXPECT_TRUE(SpansMatch(testCase)) << "text: " << testCase.text;
    }
}

TEST(LexerTests, StringContent) { EXPECT_TRUE(ContentOfWorks()); }

TEST(LexerTests, Unescape)
{
    for (const UnescapeCase& testCase : UnescapeCases)
    {
        EXPECT_TRUE(UnescapeMatches(testCase, 0)) << "content: " << testCase.content;
        EXPECT_TRUE(UnescapeMatches(testCase, 100)) << "content: " << testCase.content;
    }
}

TEST(LexerTests, UnescapePattern)
{
    for (const PatternCase& testCase : PatternCases)
    {
        EXPECT_TRUE(PatternMatches(testCase)) << "content: " << testCase.content;
    }
}

TEST(LexerTests, NumberLiterals)
{
    EXPECT_EQ(ParseNumberLiteral("18446744073709551615"), 18446744073709551615ULL);
    EXPECT_EQ(ParseNumberLiteral("18446744073709551616"), std::nullopt);
}

} // namespace
