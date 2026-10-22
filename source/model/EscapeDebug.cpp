#include "model/EscapeDebug.hpp"

#include "model/EscapeDebugTables.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>

namespace cedar::detail
{

namespace
{

constexpr char32_t ReplacementCharacter = U'�';

struct Decoded
{
    char32_t codePoint;
    std::size_t length;
};

// Strict UTF-8 decoding: rejects overlong forms, surrogates and code points above U+10FFFF.
auto DecodeOne(const std::string_view text) noexcept -> Decoded
{
    const auto byteAt = [&](const std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    const std::uint8_t lead = byteAt(0);
    if (lead < 0x80) { return {lead, 1}; }

    std::size_t length = 0;
    char32_t codePoint = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) { length = 2; codePoint = lead & 0x1FU; minimum = 0x80; }
    else if ((lead & 0xF0) == 0xE0) { length = 3; codePoint = lead & 0x0FU; minimum = 0x800; }
    else if ((lead & 0xF8) == 0xF0) { length = 4; codePoint = lead & 0x07U; minimum = 0x10000; }
    else { return {ReplacementCharacter, 1}; }

    if (text.size() < length) { return {ReplacementCharacter, 1}; }
    for (std::size_t i = 1; i < length; ++i)
    {
        const std::uint8_t continuation = byteAt(i);
        if ((continuation & 0xC0) != 0x80) { return {ReplacementCharacter, 1}; }
        codePoint = (codePoint << 6) | (continuation & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        return {ReplacementCharacter, 1};
    }
    return {codePoint, length};
}

auto InRanges(const std::span<const CodePointRange> ranges, const char32_t codePoint) noexcept -> bool
{
    const auto value = static_cast<std::uint32_t>(codePoint);
    const auto after = std::ranges::upper_bound(ranges, value, {}, &CodePointRange::first);
    return after != ranges.begin() && value <= std::prev(after)->last;
}

void AppendUtf8(std::string& output, const char32_t codePoint)
{
    const auto value = static_cast<std::uint32_t>(codePoint);
    if (value < 0x80) { output.push_back(static_cast<char>(value)); }
    else if (value < 0x800)
    {
        output.push_back(static_cast<char>(0xC0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
    else if (value < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

void AppendEscaped(std::string& output, std::string_view text, const bool everyGraphemeExtender)
{
    const auto always = EscapeDebugAlwaysEscapedRanges();
    const auto leading = EscapeDebugLeadingEscapedRanges();
    bool first = true;
    while (!text.empty())
    {
        const auto [codePoint, length] = DecodeOne(text);
        text.remove_prefix(length);
        switch (codePoint)
        {
        case U'\0': output += "\\0"; break;
        case U'\t': output += "\\t"; break;
        case U'\r': output += "\\r"; break;
        case U'\n': output += "\\n"; break;
        case U'\\': output += "\\\\"; break;
        case U'"': output += "\\\""; break;
        case U'\'': output += "\\'"; break;
        default:
            if (InRanges(always, codePoint) || ((first || everyGraphemeExtender) && InRanges(leading, codePoint)))
            {
                std::format_to(std::back_inserter(output), "\\u{{{:x}}}", static_cast<std::uint32_t>(codePoint));
            }
            else
            {
                AppendUtf8(output, codePoint);
            }
            break;
        }
        first = false;
    }
}

} // namespace

void AppendEscapeDebug(std::string& output, const std::string_view text)
{
    AppendEscaped(output, text, false);
}

void AppendCharacterEscapeDebug(std::string& output, const std::string_view text)
{
    AppendEscaped(output, text, true);
}

} // namespace cedar::detail
