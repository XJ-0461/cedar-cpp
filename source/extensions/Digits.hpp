#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cedar::detail
{

// ASCII digit helpers shared by the extension parsers. Every Cedar extension grammar uses `[0-9]`
// (or Rust's `\d`, see Decimal.hpp), never a locale's digits.

[[nodiscard]] constexpr auto IsAsciiDigit(const char character) noexcept -> bool
{
    return character >= '0' && character <= '9';
}

/// True when `text` is non-empty and all ASCII digits.
[[nodiscard]] constexpr auto IsAsciiDigits(const std::string_view text) noexcept -> bool
{
    if (text.empty()) { return false; }
    for (const char character : text)
    {
        if (!IsAsciiDigit(character)) { return false; }
    }
    return true;
}

/// The value of a run of ASCII digits (Rust's `u64::from_str` on digits only), or nothing when it
/// does not fit in 64 bits.
[[nodiscard]] constexpr auto ParseUnsignedDigits(const std::string_view digits) noexcept -> std::optional<std::uint64_t>
{
    std::uint64_t value = 0;
    for (const char digit : digits)
    {
        if (__builtin_mul_overflow(value, std::uint64_t{10}, &value) ||
            __builtin_add_overflow(value, static_cast<std::uint64_t>(digit - '0'), &value))
        {
            return std::nullopt;
        }
    }
    return value;
}

} // namespace cedar::detail
