#pragma once

#include "extensions/Digits.hpp"

#include <cedar/ExtensionValues.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace cedar::detail
{

/// Why `decimal(...)` refused its argument (`cedar-policy-core/src/extensions/decimal.rs`, `Error`).
enum class DecimalParseError : std::uint8_t
{
    InvalidSyntax,           ///< Not `-?digits.digits`.
    TooManyFractionalDigits, ///< More than four digits after the point.
    Overflow,                ///< Outside -922337203685477.5808 to 922337203685477.5807.
};

[[nodiscard]] constexpr auto Describe(const DecimalParseError error) noexcept -> std::string_view
{
    switch (error)
    {
    case DecimalParseError::InvalidSyntax: return "is not a well-formed decimal value";
    case DecimalParseError::TooManyFractionalDigits: return "has more than 4 digits after the decimal point";
    case DecimalParseError::Overflow: return "is outside the decimal range";
    }
    return "is not a well-formed decimal value";
}

/// Parses the argument of `decimal(...)`: `^(-?\d+)\.(\d+)$` with one to four fractional digits,
/// scaled by 10^4 with checked arithmetic, as `Decimal::from_str` does. The sign belongs to the
/// integer part and is applied to the fraction too, so `-0.5` is -5000.
///
/// Rust's `\d` also matches non-ASCII decimal digits, after which `i64::from_str` fails with an
/// overflow error; such input is refused here as invalid syntax, which is the same outcome.
[[nodiscard]] constexpr auto ParseDecimal(const std::string_view text) noexcept -> std::expected<Decimal, DecimalParseError>
{
    std::size_t position = 0;
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) { ++position; }

    const std::size_t integerBegin = position;
    while (position < text.size() && IsAsciiDigit(text[position])) { ++position; }
    const std::size_t integerEnd = position;
    if (integerEnd == integerBegin || position == text.size() || text[position] != '.')
    {
        return std::unexpected(DecimalParseError::InvalidSyntax);
    }
    ++position;
    const std::size_t fractionBegin = position;
    while (position < text.size() && IsAsciiDigit(text[position])) { ++position; }
    if (position == fractionBegin || position != text.size()) { return std::unexpected(DecimalParseError::InvalidSyntax); }

    // The integer part, sign included, must fit an i64 before it is scaled.
    const auto magnitude = ParseUnsignedDigits(text.substr(integerBegin, integerEnd - integerBegin));
    if (!magnitude) { return std::unexpected(DecimalParseError::Overflow); }
    constexpr std::uint64_t largestMagnitude = std::uint64_t{1} << 63;
    if (*magnitude > (negative ? largestMagnitude : largestMagnitude - 1))
    {
        return std::unexpected(DecimalParseError::Overflow);
    }
    const std::int64_t integerPart = negative ? static_cast<std::int64_t>(0 - *magnitude) : static_cast<std::int64_t>(*magnitude);
    std::int64_t scaled = 0;
    if (__builtin_mul_overflow(integerPart, Decimal::Scale, &scaled)) { return std::unexpected(DecimalParseError::Overflow); }

    const std::string_view fraction = text.substr(fractionBegin);
    if (fraction.size() > 4) { return std::unexpected(DecimalParseError::TooManyFractionalDigits); }
    std::int64_t fractionValue = 0;
    for (const char digit : fraction) { fractionValue = fractionValue * 10 + (digit - '0'); }
    for (std::size_t place = fraction.size(); place < 4; ++place) { fractionValue *= 10; }

    std::int64_t value = 0;
    const bool overflowed = negative ? __builtin_sub_overflow(scaled, fractionValue, &value)
                                     : __builtin_add_overflow(scaled, fractionValue, &value);
    if (overflowed) { return std::unexpected(DecimalParseError::Overflow); }
    return Decimal{value};
}

} // namespace cedar::detail
