#pragma once

#include "extensions/Digits.hpp"

#include <cedar/ExtensionValues.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace cedar::detail
{

/// Why `ip(...)` refused its argument (`cedar-policy-core/src/extensions/ipaddr.rs`, `IPAddr::from_str`).
enum class IpAddressParseError : std::uint8_t
{
    TooLong,        ///< More than 43 bytes.
    EmbeddedIpv4,   ///< Two or more colons and two or more dots: IPv4 written inside IPv6.
    InvalidAddress, ///< The address is not one Rust's `std::net::IpAddr` parser accepts.
    InvalidPrefix,  ///< The part after `/` is not a CIDR prefix length for the address's version.
};

[[nodiscard]] constexpr auto Describe(const IpAddressParseError error) noexcept -> std::string_view
{
    switch (error)
    {
    case IpAddressParseError::TooLong: return "is longer than 43 characters";
    case IpAddressParseError::EmbeddedIpv4: return "embeds an IPv4 address in IPv6 notation, which is not accepted";
    case IpAddressParseError::InvalidAddress: return "is not a valid IP address";
    case IpAddressParseError::InvalidPrefix: return "has an invalid prefix length";
    }
    return "is not a valid IP address";
}

/// The textual address grammar of Rust's `std::net` (`core/src/net/parser.rs`), over bytes, with
/// its backtracking: each `Read*` either consumes what it read and succeeds, or consumes nothing.
class StandardIpAddressParser
{
public:
    explicit constexpr StandardIpAddressParser(const std::string_view text) noexcept : text_{text} {}

    /// `IpAddr::from_str`: IPv4 first, then IPv6, and the whole text must be consumed.
    [[nodiscard]] constexpr auto ParseWhole() noexcept -> std::optional<IpAddress>
    {
        std::optional<IpAddress> address;
        if (const auto ipv4 = ReadIpv4()) { address = IpAddress::V4(*ipv4); }
        else if (const auto ipv6 = ReadIpv6())
        {
            std::uint64_t high = 0;
            std::uint64_t low = 0;
            for (std::size_t index = 0; index < 4; ++index)
            {
                high = (high << 16) | (*ipv6)[index];
                low = (low << 16) | (*ipv6)[index + 4];
            }
            address = IpAddress::V6(high, low);
        }
        if (!address || position_ != text_.size()) { return std::nullopt; }
        return address;
    }

private:
    using Groups = std::array<std::uint16_t, 8>;

    [[nodiscard]] constexpr auto ReadGivenCharacter(const char expected) noexcept -> bool
    {
        if (position_ < text_.size() && text_[position_] == expected)
        {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] static constexpr auto DigitValue(const char character, const std::uint32_t radix) noexcept
        -> std::optional<std::uint32_t>
    {
        std::uint32_t value = 0;
        if (character >= '0' && character <= '9') { value = static_cast<std::uint32_t>(character - '0'); }
        else if (character >= 'a' && character <= 'z') { value = static_cast<std::uint32_t>(character - 'a') + 10; }
        else if (character >= 'A' && character <= 'Z') { value = static_cast<std::uint32_t>(character - 'A') + 10; }
        else { return std::nullopt; }
        if (value >= radix) { return std::nullopt; }
        return value;
    }

    /// `read_number` with `max_digits` set: at most `maximumDigits` digits (one more fails the whole
    /// number), at least one, a leading zero only alone unless `allowZeroPrefix`, and a value of at
    /// most `maximumValue` (the `try_into` to u8 or u16).
    [[nodiscard]] constexpr auto ReadNumber(const std::uint32_t radix, const std::size_t maximumDigits,
                                            const bool allowZeroPrefix, const std::uint32_t maximumValue) noexcept
        -> std::optional<std::uint32_t>
    {
        const std::size_t start = position_;
        const bool hasLeadingZero = position_ < text_.size() && text_[position_] == '0';
        std::uint32_t result = 0;
        std::size_t digitCount = 0;
        while (position_ < text_.size())
        {
            const auto digit = DigitValue(text_[position_], radix);
            if (!digit) { break; }
            ++position_;
            result = result * radix + *digit;
            ++digitCount;
            if (digitCount > maximumDigits)
            {
                position_ = start;
                return std::nullopt;
            }
        }
        if (digitCount == 0 || (!allowZeroPrefix && hasLeadingZero && digitCount > 1) || result > maximumValue)
        {
            position_ = start;
            return std::nullopt;
        }
        return result;
    }

    /// `read_ipv4_addr`: four decimal octets separated by dots, no leading zeros.
    [[nodiscard]] constexpr auto ReadIpv4() noexcept -> std::optional<std::uint32_t>
    {
        const std::size_t start = position_;
        std::uint32_t address = 0;
        for (int index = 0; index < 4; ++index)
        {
            const bool separated = index == 0 || ReadGivenCharacter('.');
            const auto octet = separated ? ReadNumber(10, 3, false, 0xFF) : std::nullopt;
            if (!octet)
            {
                position_ = start;
                return std::nullopt;
            }
            address = (address << 8) | *octet;
        }
        return address;
    }

    /// `read_groups`: up to `limit` colon-separated hex groups into `groups`, where the last two
    /// group slots may instead be an embedded IPv4 address. Returns the number of groups read and
    /// whether an IPv4 address ended them.
    constexpr auto ReadGroups(std::uint16_t* groups, const std::size_t limit, bool& endedWithIpv4) noexcept -> std::size_t
    {
        endedWithIpv4 = false;
        for (std::size_t index = 0; index < limit; ++index)
        {
            if (index + 1 < limit)
            {
                const std::size_t start = position_;
                if (index == 0 || ReadGivenCharacter(':'))
                {
                    if (const auto ipv4 = ReadIpv4())
                    {
                        groups[index] = static_cast<std::uint16_t>(*ipv4 >> 16);
                        groups[index + 1] = static_cast<std::uint16_t>(*ipv4 & 0xFFFF);
                        endedWithIpv4 = true;
                        return index + 2;
                    }
                }
                position_ = start;
            }
            const std::size_t start = position_;
            const bool separated = index == 0 || ReadGivenCharacter(':');
            const auto group = separated ? ReadNumber(16, 4, true, 0xFFFF) : std::nullopt;
            if (!group)
            {
                position_ = start;
                return index;
            }
            groups[index] = static_cast<std::uint16_t>(*group);
        }
        return limit;
    }

    /// `read_ipv6_addr`: eight groups, or a head and a tail around one `::` that stands for at
    /// least one zero group. An embedded IPv4 address may end the tail (or a full address) but not
    /// a head.
    [[nodiscard]] constexpr auto ReadIpv6() noexcept -> std::optional<Groups>
    {
        const std::size_t start = position_;
        Groups head{};
        bool headEndedWithIpv4 = false;
        const std::size_t headSize = ReadGroups(head.data(), 8, headEndedWithIpv4);
        if (headSize == 8) { return head; }
        if (headEndedWithIpv4 || !ReadGivenCharacter(':') || !ReadGivenCharacter(':'))
        {
            position_ = start;
            return std::nullopt;
        }
        std::array<std::uint16_t, 7> tail{};
        bool tailEndedWithIpv4 = false;
        const std::size_t tailSize = ReadGroups(tail.data(), 7 - headSize, tailEndedWithIpv4);
        for (std::size_t index = 0; index < tailSize; ++index) { head[8 - tailSize + index] = tail[index]; }
        return head;
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

/// `ip(...)`'s prefix: digits only, no leading zero except `0` itself, at most `maximumLength`
/// characters and at most `maximum` (`ipaddr.rs`, `parse_prefix`).
[[nodiscard]] constexpr auto ParseIpPrefix(const std::string_view text, const std::uint8_t maximum,
                                           const std::size_t maximumLength) noexcept -> std::optional<std::uint8_t>
{
    if (text.size() > maximumLength || !IsAsciiDigits(text)) { return std::nullopt; }
    if (text.front() == '0' && text != "0") { return std::nullopt; }
    const auto value = ParseUnsignedDigits(text);
    if (!value || *value > maximum) { return std::nullopt; }
    return static_cast<std::uint8_t>(*value);
}

/// Parses the argument of `ip(...)`: an IPv4 or IPv6 address as Rust's `std::net::IpAddr` reads
/// it, optionally followed by `/prefix`. A missing prefix is the full length (32 or 128). The
/// address is kept unmasked. Text over 43 bytes, and text with two or more colons and two or more
/// dots anywhere (the check Cedar uses to refuse `::ffff:1.2.3.4`), is refused before parsing.
[[nodiscard]] constexpr auto ParseIpAddress(const std::string_view text) noexcept
    -> std::expected<IpAddress, IpAddressParseError>
{
    if (text.size() > 43) { return std::unexpected(IpAddressParseError::TooLong); }
    std::size_t colons = 0;
    std::size_t dots = 0;
    for (const char character : text)
    {
        if (character == ':') { ++colons; }
        if (character == '.') { ++dots; }
    }
    if (colons >= 2 && dots >= 2) { return std::unexpected(IpAddressParseError::EmbeddedIpv4); }

    const std::size_t slash = text.find('/');
    auto address = StandardIpAddressParser{text.substr(0, slash)}.ParseWhole();
    if (!address) { return std::unexpected(IpAddressParseError::InvalidAddress); }
    if (slash == std::string_view::npos) { return *address; }
    const bool isIpv4 = address->version == IpVersion::V4;
    const auto prefix = ParseIpPrefix(text.substr(slash + 1), isIpv4 ? 32 : 128, isIpv4 ? 2 : 3);
    if (!prefix) { return std::unexpected(IpAddressParseError::InvalidPrefix); }
    address->prefixLength = *prefix;
    return *address;
}

[[nodiscard]] constexpr auto IsIpv4(const IpAddress& address) noexcept -> bool { return address.version == IpVersion::V4; }

[[nodiscard]] constexpr auto IsIpv6(const IpAddress& address) noexcept -> bool { return address.version == IpVersion::V6; }

/// `127.0.0.0/8` (prefix at least 8) or exactly `::1/128`. An IPv4-mapped IPv6 address is IPv6,
/// so `::ffff:7f00:1` is not a loopback address.
[[nodiscard]] constexpr auto IsLoopback(const IpAddress& address) noexcept -> bool
{
    if (IsIpv4(address)) { return ((address.low >> 24) & 0xFF) == 127 && address.prefixLength >= 8; }
    return address.high == 0 && address.low == 1 && address.prefixLength >= 128;
}

/// `224.0.0.0/4` (prefix at least 4) or `ff00::/8` (prefix at least 8).
[[nodiscard]] constexpr auto IsMulticast(const IpAddress& address) noexcept -> bool
{
    if (IsIpv4(address)) { return ((address.low >> 28) & 0xF) == 0xE && address.prefixLength >= 4; }
    return (address.high >> 56) == 0xFF && address.prefixLength >= 8;
}

/// The network mask of a prefix as (high, low) 64-bit halves of a 128-bit value, for an address
/// of `width` bits held in the low end. A prefix of 0 is an empty mask and a prefix of `width` or
/// more is a full one: the shift is never by the full width of the integer (Rust's
/// `checked_shl(...).unwrap_or(0)`).
[[nodiscard]] constexpr auto NetworkMask(const std::uint8_t prefixLength, const unsigned width) noexcept
    -> std::array<std::uint64_t, 2>
{
    constexpr std::uint64_t all = ~std::uint64_t{0};
    const unsigned prefix = prefixLength < width ? prefixLength : width;
    if (width == 32)
    {
        const std::uint64_t mask = prefix == 0 ? 0 : (std::uint64_t{0xFFFF'FFFF} << (32 - prefix)) & 0xFFFF'FFFF;
        return {0, mask};
    }
    const std::uint64_t high = prefix >= 64 ? all : prefix == 0 ? 0 : all << (64 - prefix);
    const std::uint64_t low = prefix <= 64 ? 0 : prefix >= 128 ? all : all << (128 - prefix);
    return {high, low};
}

/// True when `address`'s range lies inside `range`'s: the network of `range` is at or below the
/// network of `address`, and the broadcast address of `address` is at or below that of `range`.
/// Addresses of different versions are never in range of each other.
[[nodiscard]] constexpr auto IsInRange(const IpAddress& address, const IpAddress& range) noexcept -> bool
{
    if (address.version != range.version) { return false; }
    const unsigned width = IsIpv4(address) ? 32 : 128;
    const std::uint64_t lowWidthMask = width == 32 ? 0xFFFF'FFFF : ~std::uint64_t{0};
    const auto addressMask = NetworkMask(address.prefixLength, width);
    const auto rangeMask = NetworkMask(range.prefixLength, width);
    const auto lessOrEqual = [](const std::uint64_t leftHigh, const std::uint64_t leftLow, const std::uint64_t rightHigh,
                                const std::uint64_t rightLow) {
        return leftHigh < rightHigh || (leftHigh == rightHigh && leftLow <= rightLow);
    };
    const std::uint64_t addressNetworkHigh = address.high & addressMask[0];
    const std::uint64_t addressNetworkLow = address.low & addressMask[1];
    const std::uint64_t rangeNetworkHigh = range.high & rangeMask[0];
    const std::uint64_t rangeNetworkLow = range.low & rangeMask[1];
    const std::uint64_t addressBroadcastHigh = address.high | ~addressMask[0];
    const std::uint64_t addressBroadcastLow = (address.low | ~addressMask[1]) & lowWidthMask;
    const std::uint64_t rangeBroadcastHigh = range.high | ~rangeMask[0];
    const std::uint64_t rangeBroadcastLow = (range.low | ~rangeMask[1]) & lowWidthMask;
    return lessOrEqual(rangeNetworkHigh, rangeNetworkLow, addressNetworkHigh, addressNetworkLow) &&
           lessOrEqual(addressBroadcastHigh, addressBroadcastLow, rangeBroadcastHigh, rangeBroadcastLow);
}

} // namespace cedar::detail
