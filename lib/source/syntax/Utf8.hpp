#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cedar::detail
{

/// One Unicode scalar value decoded from UTF-8, and how many bytes it took. `length == 0` means the
/// bytes at that position are not a well-formed sequence (truncated, overlong, a surrogate, above
/// U+10FFFF, or a stray continuation byte).
struct DecodedScalar
{
    char32_t scalar{};
    std::uint8_t length{};
};

/// Decodes the scalar value that starts at `text[offset]`. Precondition: `offset < text.size()`.
[[nodiscard]] constexpr auto DecodeUtf8(const std::string_view text, const std::size_t offset) noexcept -> DecodedScalar
{
    const auto byte = [&](const std::size_t index) { return static_cast<std::uint8_t>(text[index]); };
    const std::uint8_t lead = byte(offset);
    if (lead < 0x80) { return {lead, 1}; }

    std::uint8_t length = 0;
    char32_t scalar = 0;
    char32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0)
    {
        length = 2;
        scalar = static_cast<char32_t>(lead & 0x1Fu);
        minimum = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0)
    {
        length = 3;
        scalar = static_cast<char32_t>(lead & 0x0Fu);
        minimum = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0)
    {
        length = 4;
        scalar = static_cast<char32_t>(lead & 0x07u);
        minimum = 0x10000;
    }
    else
    {
        return {};
    }
    if (text.size() - offset < length) { return {}; }
    for (std::size_t index = 1; index < length; ++index)
    {
        const std::uint8_t continuation = byte(offset + index);
        if ((continuation & 0xC0) != 0x80) { return {}; }
        scalar = (scalar << 6) | static_cast<char32_t>(continuation & 0x3Fu);
    }
    if (scalar < minimum || scalar > 0x10FFFF || (scalar >= 0xD800 && scalar <= 0xDFFF)) { return {}; }
    return {scalar, length};
}

/// Appends the UTF-8 encoding of `scalar`, which must be a Unicode scalar value.
constexpr void AppendUtf8(std::string& output, const char32_t scalar)
{
    if (scalar < 0x80)
    {
        output.push_back(static_cast<char>(scalar));
    }
    else if (scalar < 0x800)
    {
        output.push_back(static_cast<char>(0xC0 | (scalar >> 6)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
    else if (scalar < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0 | (scalar >> 12)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (scalar >> 18)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
}

/// True for the characters with the Unicode `White_Space` property, which is what Rust's regex
/// `\s` matches and so what the policy grammar skips between tokens.
[[nodiscard]] constexpr auto IsUnicodeWhiteSpace(const char32_t scalar) noexcept -> bool
{
    return (scalar >= 0x09 && scalar <= 0x0D) || scalar == 0x20 || scalar == 0x85 || scalar == 0xA0 ||
           scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x2028 || scalar == 0x2029 ||
           scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

} // namespace cedar::detail
