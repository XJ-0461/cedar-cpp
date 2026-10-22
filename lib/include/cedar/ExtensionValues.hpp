#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace cedar
{

// The four extension value types. Each is a small trivially copyable struct stored inline in a
// Value. Equality and ordering are the members' (Rust derives `Eq` and `Ord` on the same fields), so
// equal values are equal however they were written: `decimal("1.0") == decimal("1.0000")`.

/// A `decimal`: a fixed-point number with four fractional digits, held as the value times 10^4
/// (`cedar-policy-core/src/extensions/decimal.rs`, `Decimal`). Ordered by value.
struct Decimal
{
    /// The power of ten the stored integer is scaled by.
    static constexpr std::int64_t Scale = 10'000;

    /// The value times Scale.
    std::int64_t scaledValue = 0;

    friend constexpr auto operator==(Decimal, Decimal) noexcept -> bool = default;
    friend constexpr auto operator<=>(Decimal, Decimal) noexcept -> std::strong_ordering = default;
};

/// A `datetime`: milliseconds since the Unix epoch, not counting leap seconds
/// (`cedar-policy-core/src/extensions/datetime.rs`, `DateTime`). Ordered by instant.
struct Datetime
{
    std::int64_t millisecondsSinceEpoch = 0;

    friend constexpr auto operator==(Datetime, Datetime) noexcept -> bool = default;
    friend constexpr auto operator<=>(Datetime, Datetime) noexcept -> std::strong_ordering = default;
};

/// A `duration`: a signed number of milliseconds (`datetime.rs`, `Duration`). Ordered by length.
struct Duration
{
    std::int64_t milliseconds = 0;

    friend constexpr auto operator==(Duration, Duration) noexcept -> bool = default;
    friend constexpr auto operator<=>(Duration, Duration) noexcept -> std::strong_ordering = default;
};

/// Which IP version an IpAddress is. V4 orders before V6, as `std::net::IpAddr` does.
enum class IpVersion : std::uint8_t
{
    V4,
    V6,
};

/// An `ipaddr`: an address and a CIDR prefix length (`cedar-policy-core/src/extensions/ipaddr.rs`,
/// `IPAddr`).
///
/// The address is stored as given, not masked by the prefix, so `ip("10.0.0.1/24")`,
/// `ip("10.0.0.0/24")` and `ip("10.0.0.1")` (prefix 32) are three different values. The order is
/// Rust's derived one: version, then address as an unsigned number, then prefix length; every IPv4
/// value orders before every IPv6 value.
///
/// The 128 address bits are `high` (bits 127..64) and `low` (bits 63..0). An IPv4 address is held in
/// the low 32 bits of `low` with `high` zero, so the member order is also the numeric order.
struct IpAddress
{
    IpVersion version = IpVersion::V4;
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    /// At most 32 for IPv4 and 128 for IPv6. A single address has the maximum.
    std::uint8_t prefixLength = 32;

    /// An IPv4 address; `address` is the dotted quad as a big-endian 32-bit number.
    [[nodiscard]] static constexpr auto V4(const std::uint32_t address, const std::uint8_t prefixLength = 32) noexcept
        -> IpAddress
    {
        return IpAddress{IpVersion::V4, 0, address, prefixLength};
    }

    /// An IPv6 address from its high and low 64 bits.
    [[nodiscard]] static constexpr auto V6(const std::uint64_t high, const std::uint64_t low,
                                           const std::uint8_t prefixLength = 128) noexcept -> IpAddress
    {
        return IpAddress{IpVersion::V6, high, low, prefixLength};
    }

    friend constexpr auto operator==(const IpAddress&, const IpAddress&) noexcept -> bool = default;
    friend constexpr auto operator<=>(const IpAddress&, const IpAddress&) noexcept -> std::strong_ordering = default;
};

// ---- Canonical printing ----
//
// Each value has a canonical Cedar expression that evaluates back to it: Rust's `canonical_repr`.
// Rust prints that form only for values computed by an operation (`offset`, `durationSince`,
// `toDate`, `toTime`); a value made by a constructor call prints the constructor's argument as it
// was written. cedar-cpp keeps no source text in a value, so it always prints the canonical form.

namespace detail
{

constexpr void AppendUnsignedDecimal(std::string& output, std::uint64_t value)
{
    char digits[20]{};
    int count = 0;
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) { output.push_back(digits[--count]); }
}

constexpr void AppendSignedDecimal(std::string& output, const std::int64_t value)
{
    if (value < 0)
    {
        output.push_back('-');
        AppendUnsignedDecimal(output, 0 - static_cast<std::uint64_t>(value));
    }
    else
    {
        AppendUnsignedDecimal(output, static_cast<std::uint64_t>(value));
    }
}

constexpr void AppendLowercaseHex(std::string& output, const std::uint16_t value)
{
    constexpr std::string_view hexDigits = "0123456789abcdef";
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4)
    {
        const auto digit = static_cast<std::size_t>((value >> shift) & 0xF);
        if (digit != 0 || started || shift == 0)
        {
            output.push_back(hexDigits[digit]);
            started = true;
        }
    }
}

constexpr void AppendDottedQuad(std::string& output, const std::uint32_t address)
{
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        AppendUnsignedDecimal(output, (address >> shift) & 0xFF);
        if (shift != 0) { output.push_back('.'); }
    }
}

} // namespace detail

/// Appends the `decimal` constructor argument, always with four fractional digits: `-1.2300`
/// (Rust's `Display for Decimal`).
constexpr void AppendCanonicalString(std::string& output, const Decimal value)
{
    const std::uint64_t magnitude = value.scaledValue < 0 ? 0 - static_cast<std::uint64_t>(value.scaledValue)
                                                          : static_cast<std::uint64_t>(value.scaledValue);
    if (value.scaledValue < 0) { output.push_back('-'); }
    constexpr auto scale = static_cast<std::uint64_t>(Decimal::Scale);
    detail::AppendUnsignedDecimal(output, magnitude / scale);
    output.push_back('.');
    const std::uint64_t fraction = magnitude % scale;
    for (std::uint64_t place = scale / 10; place > 0; place /= 10) { output.push_back(static_cast<char>('0' + fraction / place % 10)); }
}

/// Appends the `duration` constructor argument in milliseconds: `-5ms` (Rust's `Display for Duration`).
constexpr void AppendCanonicalString(std::string& output, const Duration value)
{
    detail::AppendSignedDecimal(output, value.milliseconds);
    output.append("ms");
}

/// Appends the `ip` constructor argument as `address/prefix`, with the address printed as Rust's
/// `Display for IpAddr` prints it: IPv4 as a dotted quad; IPv6 in lowercase hex with the first
/// longest run of two or more zero groups shortened to `::`, except that an IPv4-mapped address
/// prints as `::ffff:a.b.c.d`. (That form mixes colons and dots, so `ip()` refuses to parse it back;
/// Rust shares the quirk.)
constexpr void AppendCanonicalString(std::string& output, const IpAddress& value)
{
    if (value.version == IpVersion::V4)
    {
        detail::AppendDottedQuad(output, static_cast<std::uint32_t>(value.low));
    }
    else
    {
        std::uint16_t groups[8]{};
        for (int index = 0; index < 4; ++index)
        {
            groups[index] = static_cast<std::uint16_t>(value.high >> (48 - 16 * index));
            groups[index + 4] = static_cast<std::uint16_t>(value.low >> (48 - 16 * index));
        }
        if (value.high == 0 && (value.low >> 32) == 0xFFFF)
        {
            output.append("::ffff:");
            detail::AppendDottedQuad(output, static_cast<std::uint32_t>(value.low));
        }
        else
        {
            int longestStart = 0;
            int longestLength = 0;
            int currentStart = 0;
            int currentLength = 0;
            for (int index = 0; index < 8; ++index)
            {
                if (groups[index] != 0)
                {
                    currentLength = 0;
                    continue;
                }
                if (currentLength == 0) { currentStart = index; }
                ++currentLength;
                if (currentLength > longestLength)
                {
                    longestStart = currentStart;
                    longestLength = currentLength;
                }
            }
            const auto appendGroups = [&](const int begin, const int end) {
                for (int index = begin; index < end; ++index)
                {
                    if (index != begin) { output.push_back(':'); }
                    detail::AppendLowercaseHex(output, groups[index]);
                }
            };
            if (longestLength > 1)
            {
                appendGroups(0, longestStart);
                output.append("::");
                appendGroups(longestStart + longestLength, 8);
            }
            else
            {
                appendGroups(0, 8);
            }
        }
    }
    output.push_back('/');
    detail::AppendUnsignedDecimal(output, value.prefixLength);
}

/// Appends `decimal("<canonical>")`.
constexpr void AppendCanonicalCall(std::string& output, const Decimal value)
{
    output.append("decimal(\"");
    AppendCanonicalString(output, value);
    output.append("\")");
}

/// Appends `duration("<n>ms")`.
constexpr void AppendCanonicalCall(std::string& output, const Duration value)
{
    output.append("duration(\"");
    AppendCanonicalString(output, value);
    output.append("\")");
}

/// Appends `ip("<address>/<prefix>")`.
constexpr void AppendCanonicalCall(std::string& output, const IpAddress& value)
{
    output.append("ip(\"");
    AppendCanonicalString(output, value);
    output.append("\")");
}

/// Appends `datetime("1970-01-01").offset(duration("<n>ms"))`, the epoch offset by the value's
/// milliseconds (Rust's `DateTime::as_ext_func_call`, printed with `offset` in method style).
constexpr void AppendCanonicalCall(std::string& output, const Datetime value)
{
    output.append("datetime(\"1970-01-01\").offset(");
    AppendCanonicalCall(output, Duration{value.millisecondsSinceEpoch});
    output.push_back(')');
}

} // namespace cedar
