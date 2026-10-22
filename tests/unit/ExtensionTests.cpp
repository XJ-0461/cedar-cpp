// The extension types: decimal, ipaddr, datetime and duration. Each table is checked twice: by
// static_assert at compile time, and by a gtest at run time (so the same code also runs under the
// sanitizers). The cases come from the `#[cfg(test)]` modules of
// `cedar-policy-core/src/extensions/{decimal,ipaddr,datetime}.rs`, plus edge cases answered by the
// reference CLI (`cedar evaluate`), marked "oracle".

#include "extensions/Datetime.hpp"
#include "extensions/Decimal.hpp"
#include "extensions/ExtensionCalls.hpp"
#include "extensions/IpAddress.hpp"

#include <cedar/Errors.hpp>
#include <cedar/Value.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using cedar::Datetime;
using cedar::Decimal;
using cedar::Duration;
using cedar::EvaluationErrorKind;
using cedar::IpAddress;
using cedar::Value;
using cedar::ValueKind;
using cedar::detail::DatetimeParseError;
using cedar::detail::DecimalParseError;
using cedar::detail::DurationParseError;
using cedar::detail::IpAddressParseError;

constexpr std::int64_t Minimum = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t Maximum = std::numeric_limits<std::int64_t>::max();

template <typename Case, std::size_t Count, typename Check>
constexpr auto AllPass(const std::array<Case, Count>& cases, Check check) -> bool
{
    for (const Case& testCase : cases)
    {
        if (!check(testCase)) { return false; }
    }
    return true;
}

/// Runs a table at run time, reporting each failing case by its text.
template <typename Case, std::size_t Count, typename Check>
void ExpectAllPass(const std::array<Case, Count>& cases, Check check)
{
    for (const Case& testCase : cases) { EXPECT_TRUE(check(testCase)) << testCase.text; }
}

// ---- decimal ----

struct DecimalCase
{
    std::string_view text;
    std::expected<Decimal, DecimalParseError> expected;
};

constexpr auto Accept(const std::int64_t scaledValue) -> std::expected<Decimal, DecimalParseError>
{
    return Decimal{scaledValue};
}

constexpr auto Reject(const DecimalParseError error) -> std::expected<Decimal, DecimalParseError>
{
    return std::unexpected(error);
}

constexpr std::array DecimalCases{
    DecimalCase{"1.0", Accept(10'000)},
    DecimalCase{"-1.0", Accept(-10'000)},
    DecimalCase{"123.456", Accept(1'234'560)},
    DecimalCase{"0.1234", Accept(1'234)},
    DecimalCase{"-0.0123", Accept(-123)},
    DecimalCase{"55.1", Accept(551'000)},
    DecimalCase{"-922337203685477.5808", Accept(Minimum)},
    DecimalCase{"922337203685477.5807", Accept(Maximum)},
    DecimalCase{"00.000", Accept(0)},
    DecimalCase{"123.0", Accept(1'230'000)},
    DecimalCase{"123.0000", Accept(1'230'000)},
    DecimalCase{"0123.0", Accept(1'230'000)},
    DecimalCase{"0.0", Accept(0)},
    DecimalCase{"-0.0", Accept(0)},
    DecimalCase{"-0.5", Accept(-5'000)},
    DecimalCase{"-0.0001", Accept(-1)},
    DecimalCase{"1.2300", Accept(12'300)},
    DecimalCase{"-123.4560", Accept(-1'234'560)},
    DecimalCase{"1234", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"1.0.", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"1.", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{".1", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"1.a", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"-.", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"-", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"+1.0", Reject(DecimalParseError::InvalidSyntax)},     // oracle
    DecimalCase{"1.0\n", Reject(DecimalParseError::InvalidSyntax)},    // oracle
    DecimalCase{" 1.0", Reject(DecimalParseError::InvalidSyntax)},
    DecimalCase{"\xD9\xA1.0", Reject(DecimalParseError::InvalidSyntax)}, // oracle: an Arabic-Indic digit
    DecimalCase{"1000000000000000.0", Reject(DecimalParseError::Overflow)},
    DecimalCase{"922337203685477.5808", Reject(DecimalParseError::Overflow)},
    DecimalCase{"-922337203685477.5809", Reject(DecimalParseError::Overflow)},
    DecimalCase{"-922337203685478.0", Reject(DecimalParseError::Overflow)},
    DecimalCase{"99999999999999999999.0", Reject(DecimalParseError::Overflow)},
    DecimalCase{"0.12345", Reject(DecimalParseError::TooManyFractionalDigits)},
    DecimalCase{"0.00000", Reject(DecimalParseError::TooManyFractionalDigits)},
};

constexpr auto DecimalCaseMatches(const DecimalCase& testCase) -> bool
{
    return cedar::detail::ParseDecimal(testCase.text) == testCase.expected;
}

static_assert(AllPass(DecimalCases, DecimalCaseMatches));

/// A parsed decimal printed canonically parses back to the same value (`decimal_display`).
constexpr auto DecimalRoundTrips(const std::string_view text) -> bool
{
    const auto parsed = cedar::detail::ParseDecimal(text);
    if (!parsed) { return false; }
    std::string printed;
    cedar::AppendCanonicalString(printed, *parsed);
    return cedar::detail::ParseDecimal(printed) == parsed;
}

constexpr std::array<std::string_view, 6> DecimalRoundTripCases{"123.0", "1.2300", "123.4560", "-123.4560", "0.0",
                                                                "-922337203685477.5808"};

constexpr auto AllDecimalsRoundTrip() -> bool
{
    for (const std::string_view text : DecimalRoundTripCases)
    {
        if (!DecimalRoundTrips(text)) { return false; }
    }
    return true;
}

static_assert(AllDecimalsRoundTrip());

/// `lessThan`, `lessThanOrEqual`, `greaterThan`, `greaterThanOrEqual` on `left` and `right`.
struct DecimalComparisonCase
{
    std::string_view text;
    std::string_view right;
    bool lessThan;
    bool lessThanOrEqual;
    bool greaterThan;
    bool greaterThanOrEqual;
};

constexpr std::array DecimalComparisonCases{
    DecimalComparisonCase{"1.23", "1.24", true, true, false, false},
    DecimalComparisonCase{"1.23", "1.23", false, true, false, true},
    DecimalComparisonCase{"123.45", "1.23", false, false, true, true},
    DecimalComparisonCase{"-1.23", "1.23", true, true, false, false},
    DecimalComparisonCase{"-1.23", "-1.24", false, false, true, true},
    DecimalComparisonCase{"-0.0001", "0.0", true, true, false, false},
    DecimalComparisonCase{"-0.0023", "-0.23", false, false, true, true},
    DecimalComparisonCase{"-1.0000", "-0.9999", true, true, false, false},
};

constexpr auto DecimalComparisonMatches(const DecimalComparisonCase& testCase) -> bool
{
    const auto left = cedar::detail::ParseDecimal(testCase.text);
    const auto right = cedar::detail::ParseDecimal(testCase.right);
    return left && right && (*left < *right) == testCase.lessThan && (*left <= *right) == testCase.lessThanOrEqual &&
           (*left > *right) == testCase.greaterThan && (*left >= *right) == testCase.greaterThanOrEqual;
}

static_assert(AllPass(DecimalComparisonCases, DecimalComparisonMatches));

// ---- ipaddr ----

struct IpCase
{
    std::string_view text;
    std::expected<IpAddress, IpAddressParseError> expected;
};

constexpr auto V4(const std::uint32_t address, const std::uint8_t prefix = 32) -> std::expected<IpAddress, IpAddressParseError>
{
    return IpAddress::V4(address, prefix);
}

constexpr auto V6(const std::uint64_t high, const std::uint64_t low, const std::uint8_t prefix = 128)
    -> std::expected<IpAddress, IpAddressParseError>
{
    return IpAddress::V6(high, low, prefix);
}

constexpr auto Refuse(const IpAddressParseError error) -> std::expected<IpAddress, IpAddressParseError>
{
    return std::unexpected(error);
}

constexpr std::array IpCases{
    IpCase{"127.0.0.1", V4(0x7F00'0001)},
    IpCase{"::1", V6(0, 1)},
    IpCase{"::ffff:ff00:1", V6(0, 0x0000'FFFF'FF00'0001)},
    IpCase{"127.0.0.1/24", V4(0x7F00'0001, 24)},
    IpCase{"ffee::/64", V6(0xFFEE'0000'0000'0000, 0, 64)},
    IpCase{"127.0.0.1/0", V4(0x7F00'0001, 0)},
    IpCase{"127.0.0.1/32", V4(0x7F00'0001, 32)},
    IpCase{"ffee::/0", V6(0xFFEE'0000'0000'0000, 0, 0)},
    IpCase{"ffee::/128", V6(0xFFEE'0000'0000'0000, 0, 128)},
    IpCase{"ffee::11", V6(0xFFEE'0000'0000'0000, 0x11)},
    IpCase{"0.0.0.0", V4(0)},                                                   // oracle
    IpCase{"1:2:3:4:5:6:7::", V6(0x0001'0002'0003'0004, 0x0005'0006'0007'0000)}, // oracle
    IpCase{"::1:2:3:4:5:6:7", V6(0x0000'0001'0002'0003, 0x0004'0005'0006'0007)}, // oracle
    IpCase{"::", V6(0, 0)},                                                     // oracle
    IpCase{"ABCD::EF", V6(0xABCD'0000'0000'0000, 0xEF)},                        // oracle
    IpCase{"1:2:3:4:5:6:7:8/128", V6(0x0001'0002'0003'0004, 0x0005'0006'0007'0008)},
    IpCase{"::0:0:0:0:0:0:0", V6(0, 0)},  // oracle
    IpCase{"0:0:0:0:0:0:0::", V6(0, 0)},  // oracle
    IpCase{"ABCD:EF01:2345:6789:ABCD:EF01:2345:6789/128", V6(0xABCD'EF01'2345'6789, 0xABCD'EF01'2345'6789)},
    IpCase{"ABCD:EF01:2345:6789:ABCD:EF01:2345:6789/1280", Refuse(IpAddressParseError::TooLong)},
    IpCase{"::ffff:127.0.0.1", Refuse(IpAddressParseError::EmbeddedIpv4)},
    IpCase{"::127.0.0.1", Refuse(IpAddressParseError::EmbeddedIpv4)},
    IpCase{"380.0.0.1", Refuse(IpAddressParseError::InvalidAddress)},
    IpCase{"?", Refuse(IpAddressParseError::InvalidAddress)},
    IpCase{"ab.ab.ab.ab", Refuse(IpAddressParseError::InvalidAddress)},
    IpCase{"foo::1", Refuse(IpAddressParseError::InvalidAddress)},
    IpCase{"", Refuse(IpAddressParseError::InvalidAddress)},
    IpCase{"01.2.3.4", Refuse(IpAddressParseError::InvalidAddress)},           // oracle
    IpCase{"::00001", Refuse(IpAddressParseError::InvalidAddress)},            // oracle
    IpCase{"1:2:3:4:5:6:7:8::", Refuse(IpAddressParseError::InvalidAddress)},  // oracle
    IpCase{"1::2::3", Refuse(IpAddressParseError::InvalidAddress)},            // oracle
    IpCase{"1:2:3:4:5:6:7:8:9", Refuse(IpAddressParseError::InvalidAddress)},  // oracle
    IpCase{"1:2:3:4:5:6:7", Refuse(IpAddressParseError::InvalidAddress)},      // oracle
    IpCase{" 1.2.3.4", Refuse(IpAddressParseError::InvalidAddress)},           // oracle
    IpCase{"256.1.1.1", Refuse(IpAddressParseError::InvalidAddress)},          // oracle
    IpCase{"1.2.3", Refuse(IpAddressParseError::InvalidAddress)},              // oracle
    IpCase{"1.2.3.4:5", Refuse(IpAddressParseError::InvalidAddress)},          // oracle
    IpCase{"::1.2", Refuse(IpAddressParseError::InvalidAddress)},              // oracle
    IpCase{"1:2:3:4:5:6:7:1.2", Refuse(IpAddressParseError::InvalidAddress)},  // oracle
    IpCase{"fe80::1%eth0", Refuse(IpAddressParseError::InvalidAddress)},       // oracle
    IpCase{"[::1]", Refuse(IpAddressParseError::InvalidAddress)},              // oracle
    IpCase{"::0:0:0:0:0:0:0:0", Refuse(IpAddressParseError::InvalidAddress)},  // oracle
    IpCase{"127.0.0.1/8/24", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"fee::/64::1", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"172.0.0.1/64", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"ffee::/132", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"ffee::/+1", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"ffee::/01", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"ffee::/1234", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"10.0.0.0/33", Refuse(IpAddressParseError::InvalidPrefix)},
    IpCase{"1.2.3.4/032", Refuse(IpAddressParseError::InvalidPrefix)},  // oracle
    IpCase{"::/0128", Refuse(IpAddressParseError::InvalidPrefix)},      // oracle
    IpCase{"1.2.3.4/", Refuse(IpAddressParseError::InvalidPrefix)},     // oracle
};

constexpr auto IpCaseMatches(const IpCase& testCase) -> bool
{
    return cedar::detail::ParseIpAddress(testCase.text) == testCase.expected;
}

static_assert(AllPass(IpCases, IpCaseMatches));

/// The `std::net` grammar alone, including the embedded-IPv4 forms Cedar's colon-and-dot check
/// refuses before parsing.
struct StandardIpCase
{
    std::string_view text;
    std::optional<IpAddress> expected;
};

constexpr std::array StandardIpCases{
    StandardIpCase{"::ffff:1.2.3.4", IpAddress::V6(0, 0x0000'FFFF'0102'0304)},
    StandardIpCase{"::1.2.3.4", IpAddress::V6(0, 0x0102'0304)},
    StandardIpCase{"1:2:3:4:5:6:1.2.3.4", IpAddress::V6(0x0001'0002'0003'0004, 0x0005'0006'0102'0304)},
    StandardIpCase{"1:2:3:4:5::1.2.3.4", IpAddress::V6(0x0001'0002'0003'0004, 0x0005'0000'0102'0304)},
    StandardIpCase{"1.2.3.4::", std::nullopt},
    StandardIpCase{"1.2.3.4::1", std::nullopt},
    StandardIpCase{"1:2:3:4:5:6:7:1.2.3.4", std::nullopt},
    StandardIpCase{"::1.2.3.04", std::nullopt},
    StandardIpCase{"1.2.3.4", IpAddress::V4(0x0102'0304)},
    StandardIpCase{"1.2.3.4/8", std::nullopt},
};

constexpr auto StandardIpCaseMatches(const StandardIpCase& testCase) -> bool
{
    return cedar::detail::StandardIpAddressParser{testCase.text}.ParseWhole() == testCase.expected;
}

static_assert(AllPass(StandardIpCases, StandardIpCaseMatches));

struct IpPredicateCase
{
    std::string_view text;
    bool isIpv4;
    bool isLoopback;
    bool isMulticast;
};

constexpr std::array IpPredicateCases{
    IpPredicateCase{"127.0.0.1", true, true, false},
    IpPredicateCase{"::1", false, true, false},
    IpPredicateCase{"::ffff:ff00:1", false, false, false},
    IpPredicateCase{"127.0.0.1/24", true, true, false},
    IpPredicateCase{"ffee::/64", false, false, true},
    IpPredicateCase{"127.0.0.2", true, true, false},
    IpPredicateCase{"::2", false, false, false},
    IpPredicateCase{"127.255.200.200/0", true, false, false},
    IpPredicateCase{"127.0.0.1/7", true, false, false},
    IpPredicateCase{"::1/127", false, false, false},
    IpPredicateCase{"228.228.228.0", true, false, true},
    IpPredicateCase{"224.0.0.0/3", true, false, false},
    IpPredicateCase{"224.0.0.0/5", true, false, true},
    IpPredicateCase{"ff00::/7", false, false, false},
    IpPredicateCase{"ff00::/9", false, false, true},
    IpPredicateCase{"127.0.0.1/1", true, false, false},
    IpPredicateCase{"ff00::2", false, false, true},
    IpPredicateCase{"::ffff:7f00:1", false, false, false},  // oracle: IPv4-mapped is not loopback
    IpPredicateCase{"::ffff:e000:1", false, false, false},  // oracle: nor multicast
};

constexpr auto IpPredicateMatches(const IpPredicateCase& testCase) -> bool
{
    const auto address = cedar::detail::ParseIpAddress(testCase.text);
    return address && cedar::detail::IsIpv4(*address) == testCase.isIpv4 &&
           cedar::detail::IsIpv6(*address) == !testCase.isIpv4 &&
           cedar::detail::IsLoopback(*address) == testCase.isLoopback &&
           cedar::detail::IsMulticast(*address) == testCase.isMulticast;
}

static_assert(AllPass(IpPredicateCases, IpPredicateMatches));

/// `ip(text).isInRange(ip(range))`.
struct IpRangeCase
{
    std::string_view text;
    std::string_view range;
    bool inRange;
};

constexpr std::array IpRangeCases{
    IpRangeCase{"192.168.0.1/24", "192.168.0.1/24", true},
    IpRangeCase{"192.168.0.1", "192.168.0.1/28", true},
    IpRangeCase{"192.168.0.10", "192.168.0.1/24", true},
    IpRangeCase{"192.168.0.10", "192.168.0.1/28", true},
    IpRangeCase{"192.168.0.75", "192.168.0.1/24", true},
    IpRangeCase{"192.168.0.75", "192.168.0.1/28", false},
    IpRangeCase{"192.168.0.1", "192.168.0.1", true},
    IpRangeCase{"1:2:3:4::", "1:2:3:4::/48", true},
    IpRangeCase{"1:2:3:4::", "1:2:3:4::/52", true},
    IpRangeCase{"1:2:3:6::", "1:2:3:4::/48", true},
    IpRangeCase{"1:2:3:6::", "1:2:3:4::/52", true},
    IpRangeCase{"1:2:3:ffff::", "1:2:3:4::/48", true},
    IpRangeCase{"1:2:3:ffff::", "1:2:3:4::/52", false},
    IpRangeCase{"1:2:3:4::", "1:2:3:4::", true},
    IpRangeCase{"192.168.0.1", "1:2:3:4::/48", false},
    IpRangeCase{"10.0.0.0", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.0", "10.0.0.0/32", true},
    IpRangeCase{"10.0.0.0", "10.0.0.1/24", true},
    IpRangeCase{"10.0.0.0", "10.0.0.1/32", false},
    IpRangeCase{"10.0.0.1", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.1", "10.0.0.1/24", true},
    IpRangeCase{"10.0.0.0/24", "10.0.0.0/32", false},
    IpRangeCase{"10.0.0.0/32", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.1/24", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.1/24", "10.0.0.1/24", true},
    IpRangeCase{"10.0.0.0/24", "10.0.0.1/24", true},
    IpRangeCase{"10.0.0.0/24", "10.0.0.0/29", false},
    IpRangeCase{"10.0.0.0/29", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.0/24", "10.0.0.1/29", false},
    IpRangeCase{"10.0.0.0/29", "10.0.0.1/24", true},
    IpRangeCase{"10.0.0.1/24", "10.0.0.0/29", false},
    IpRangeCase{"10.0.0.1/29", "10.0.0.0/24", true},
    IpRangeCase{"10.0.0.0/32", "10.0.0.0/32", true},
    IpRangeCase{"10.0.0.0/32", "10.0.0.0", true},
    // The mask edges: an empty mask covers everything of the version, a full one only itself.
    IpRangeCase{"255.255.255.255", "0.0.0.0/0", true},
    IpRangeCase{"0.0.0.0/0", "255.255.255.255/0", true},
    IpRangeCase{"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", "::/0", true},
    IpRangeCase{"::/0", "::1/128", false},
    IpRangeCase{"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:0/112", true},
    IpRangeCase{"1:2:3:4:5:6:8:8", "1:2:3:4:5:6:7:0/112", false},
    IpRangeCase{"1:2:3:4:8000::", "1:2:3:4::/65", false},
    IpRangeCase{"1:2:3:4:7fff::", "1:2:3:4::/65", true},
    IpRangeCase{"1:2:3:5::", "1:2:3:4::/64", false},
};

constexpr auto IpRangeMatches(const IpRangeCase& testCase) -> bool
{
    const auto address = cedar::detail::ParseIpAddress(testCase.text);
    const auto range = cedar::detail::ParseIpAddress(testCase.range);
    return address && range && cedar::detail::IsInRange(*address, *range) == testCase.inRange;
}

static_assert(AllPass(IpRangeCases, IpRangeMatches));

// ---- datetime ----

struct DatetimeCase
{
    std::string_view text;
    std::expected<Datetime, DatetimeParseError> expected;
};

constexpr auto At(const std::int64_t millisecondsSinceEpoch) -> std::expected<Datetime, DatetimeParseError>
{
    return Datetime{millisecondsSinceEpoch};
}

constexpr auto Refuse(const DatetimeParseError error) -> std::expected<Datetime, DatetimeParseError>
{
    return std::unexpected(error);
}

constexpr std::array DatetimeCases{
    DatetimeCase{"2024-10-15", At(1'728'950'400'000)},
    DatetimeCase{"2024-10-15T11:38:02Z", At(1'728'992'282'000)},
    DatetimeCase{"2024-10-15T11:38:02.101Z", At(1'728'992'282'101)},
    DatetimeCase{"2024-10-15T11:38:02.101+1134", At(1'728'950'642'101)},
    DatetimeCase{"2024-10-15T11:38:02.101-1134", At(1'729'033'922'101)},
    DatetimeCase{"2024-10-15T11:38:02+1134", At(1'728'950'642'000)},
    DatetimeCase{"2024-10-15T11:38:02-1134", At(1'729'033'922'000)},
    DatetimeCase{"2024-10-15T23:59:00+2359", At(1'728'950'400'000)},
    DatetimeCase{"2024-10-15T00:00:00-2359", At(1'729'036'740'000)},
    DatetimeCase{"2024-10-28", At(1'730'073'600'000)},
    DatetimeCase{"2024-10-28T01:22:33.456Z", At(1'730'078'553'456)},
    DatetimeCase{"2024-10-28T10:12:13.456-0700", At(1'730'135'533'456)},
    DatetimeCase{"1970-01-01", At(0)},
    DatetimeCase{"1900-01-01", At(-2'208'988'800'000)},
    DatetimeCase{"0000-01-01", At(-62'167'219'200'000)},                    // oracle
    DatetimeCase{"9999-12-31T23:59:59.999-2359", At(253'402'387'139'999)}, // oracle
    DatetimeCase{"2024-02-29", At(1'709'164'800'000)},                      // oracle
    DatetimeCase{"2000-02-29", At(951'782'400'000)},                        // oracle
    DatetimeCase{"", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"a", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"-", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"-1", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"11-12-13", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"1111-1x-20", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"0000-0a-01", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"10000-01-01", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"10000-01-01T00:00:00Z", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"22024-30-28T10:12:13.456-0700", Refuse(DatetimeParseError::InvalidDatePattern)},
    DatetimeCase{"2024-00-01", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-01-00", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-02-30", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2025-02-29", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-20-01", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-01-32", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-01-99", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"2024-04-31", Refuse(DatetimeParseError::InvalidDate)},
    DatetimeCase{"1900-02-29", Refuse(DatetimeParseError::InvalidDate)},        // oracle
    DatetimeCase{"2024-02-30T25:00:00Z", Refuse(DatetimeParseError::InvalidDate)}, // the date is checked first
    DatetimeCase{"2024-10-15Z", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01Ta", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T01:", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T01:02", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T01:02:0b", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T01::02:03", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T01::02::03", Refuse(DatetimeParseError::InvalidTimePattern)},
    DatetimeCase{"2024-01-01T31:02:03Z", Refuse(DatetimeParseError::InvalidTime)},
    DatetimeCase{"2024-01-01T01:60:03Z", Refuse(DatetimeParseError::InvalidTime)},
    DatetimeCase{"2016-12-31T23:59:60Z", Refuse(DatetimeParseError::InvalidTime)},
    DatetimeCase{"2016-12-31T23:59:61Z", Refuse(DatetimeParseError::InvalidTime)},
    DatetimeCase{"2024-10-15T11:38:02ZZ", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00T", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00ZZ", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00x001Z", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.001ZZ", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00\xE2\x9E\x95" "0000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00\xE2\x9E\x96" "0000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.0001Z", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.001\xE2\x9E\x96" "0000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.001\xE2\x9E\x95" "0000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.001+00000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.001-00000", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00.01Z", Refuse(DatetimeParseError::InvalidMillisecondOrZone)},
    DatetimeCase{"2024-01-01T00:00:00Z\n", Refuse(DatetimeParseError::InvalidMillisecondOrZone)}, // oracle
    DatetimeCase{"2016-12-31T00:00:00+1160", Refuse(DatetimeParseError::InvalidOffset)},
    DatetimeCase{"2016-12-31T00:00:00+1199", Refuse(DatetimeParseError::InvalidOffset)},
    DatetimeCase{"2016-12-31T00:00:00+2400", Refuse(DatetimeParseError::InvalidOffset)},
};

constexpr auto DatetimeCaseMatches(const DatetimeCase& testCase) -> bool
{
    return cedar::detail::ParseDatetime(testCase.text) == testCase.expected;
}

static_assert(AllPass(DatetimeCases, DatetimeCaseMatches));

// ---- duration ----

struct DurationCase
{
    std::string_view text;
    std::expected<Duration, DurationParseError> expected;
};

constexpr auto Lasting(const std::int64_t milliseconds) -> std::expected<Duration, DurationParseError>
{
    return Duration{milliseconds};
}

constexpr auto Refuse(const DurationParseError error) -> std::expected<Duration, DurationParseError>
{
    return std::unexpected(error);
}

constexpr std::array DurationCases{
    DurationCase{"1h", Lasting(3'600'000)},
    DurationCase{"-10h", Lasting(-36'000'000)},
    DurationCase{"5d3ms", Lasting(432'000'003)},
    DurationCase{"-3h5m", Lasting(-11'100'000)},
    DurationCase{"1d2h3m4s50ms", Lasting(93'784'050)},
    DurationCase{"-1d2h3m4s50ms", Lasting(-93'784'050)},
    DurationCase{"10d23h59m58s999ms", Lasting(950'398'999)},
    DurationCase{"106751991167d7h12m55s807ms", Lasting(Maximum)},
    DurationCase{"-106751991167d7h12m55s808ms", Lasting(Minimum)},
    DurationCase{"106751991167d7h12m55s806ms", Lasting(Maximum - 1)},
    DurationCase{"1m1ms", Lasting(60'001)},                         // oracle
    DurationCase{"1ms", Lasting(1)},                                // oracle
    DurationCase{"-9223372036854775808ms", Lasting(Minimum)},       // oracle
    DurationCase{"106751991167d", Lasting(9'223'372'036'828'800'000)}, // oracle
    DurationCase{"-0ms", Lasting(0)},                               // oracle
    DurationCase{"0d0h0m0s0ms", Lasting(0)},
    DurationCase{"", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"a", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-1", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"\xE2\x9E\x96" "1ms", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"11dd", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"00000mm", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-h", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-1hh", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-h2d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"-ms", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1ms1s", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1ms1m", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1ms1h", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"0ms1d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1s1m", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1s1h", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"0s0d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"0m0h", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"0m0d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1h1d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1ms1m1d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1dd2h3m4s50ms", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1d1d", Refuse(DurationParseError::InvalidPattern)}, // oracle
    DurationCase{"--1d", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"1d ", Refuse(DurationParseError::InvalidPattern)},
    DurationCase{"106751991167d7h12m55s808ms", Refuse(DurationParseError::Overflow)},
    DurationCase{"-106751991167d7h12m55s809ms", Refuse(DurationParseError::Overflow)},
    DurationCase{"9223372036854775808ms", Refuse(DurationParseError::Overflow)}, // oracle
    DurationCase{"18446744073709551616ms", Refuse(DurationParseError::Overflow)},
    DurationCase{"106751991168d", Refuse(DurationParseError::Overflow)},
    DurationCase{"9223372036854775808s", Refuse(DurationParseError::Overflow)},
};

constexpr auto DurationCaseMatches(const DurationCase& testCase) -> bool
{
    return cedar::detail::ParseDuration(testCase.text) == testCase.expected;
}

static_assert(AllPass(DurationCases, DurationCaseMatches));

// ---- datetime and duration operations ----

constexpr auto ParsedDatetime(const std::string_view text) -> Datetime { return *cedar::detail::ParseDatetime(text); }

constexpr auto ParsedDuration(const std::string_view text) -> Duration { return *cedar::detail::ParseDuration(text); }

constexpr auto OffsetsWork() -> bool
{
    using cedar::detail::Offset;
    constexpr Datetime epoch{0};
    const Datetime latest = *Offset(epoch, Duration{Maximum});
    const Datetime earliest = *Offset(epoch, Duration{Minimum});
    return !Offset(latest, ParsedDuration("1ms")) && Offset(latest, ParsedDuration("-1ms")) == Datetime{Maximum - 1} &&
           !Offset(earliest, ParsedDuration("-1ms")) && Offset(earliest, ParsedDuration("1ms")) == Datetime{Minimum + 1} &&
           Offset(epoch, ParsedDuration("1d")) == ParsedDatetime("1970-01-02") &&
           Offset(epoch, ParsedDuration("-1d")) == ParsedDatetime("1969-12-31") &&
           Offset(ParsedDatetime("2024-10-28T10:12:13.456-0700"), ParsedDuration("-7h")) ==
               ParsedDatetime("2024-10-28T10:12:13.456Z") &&
           Offset(ParsedDatetime("2024-10-28T10:12:13.456+0700"), ParsedDuration("7h")) ==
               ParsedDatetime("2024-10-28T10:12:13.456Z");
}

static_assert(OffsetsWork());

constexpr auto DurationsSinceWork() -> bool
{
    using cedar::detail::DurationSince;
    constexpr Datetime epoch{0};
    const Datetime today = ParsedDatetime("2024-10-24");
    const Datetime yesterday = ParsedDatetime("2024-10-23");
    return DurationSince(today, epoch) == ParsedDuration("20020d") && DurationSince(yesterday, today) == ParsedDuration("-1d") &&
           DurationSince(today, yesterday) == ParsedDuration("1d") && !DurationSince(today, Datetime{Minimum}) &&
           DurationSince(ParsedDatetime("2024-10-28T10:12:13.456+0700"), ParsedDatetime("2024-10-28T10:12:13.456Z")) ==
               ParsedDuration("-7h") &&
           DurationSince(ParsedDatetime("2024-10-28T10:12:13.456-0700"), ParsedDatetime("2024-10-28T10:12:13.456Z")) ==
               ParsedDuration("7h");
}

static_assert(DurationsSinceWork());

constexpr auto DateAndTimeWork() -> bool
{
    using cedar::detail::Offset;
    using cedar::detail::ToDate;
    using cedar::detail::ToTime;
    const Duration latestInDay = ParsedDuration("23h59m59s999ms");
    const Duration earliestInDay = ParsedDuration("-23h59m59s999ms");
    const std::array days{ParsedDatetime("2024-10-24"), ParsedDatetime("2024-10-23"), Datetime{0},
                          ParsedDatetime("1900-01-01")};
    for (const Datetime day : days)
    {
        if (ToDate(day) != day) { return false; }
        if (ToDate(*Offset(day, latestInDay)) != day) { return false; }
        if (ToDate(*Offset(day, earliestInDay)) != Offset(day, ParsedDuration("-1d"))) { return false; }
        if (ToTime(*Offset(day, latestInDay)) != latestInDay) { return false; }
        if (ToTime(*Offset(day, earliestInDay)) != ParsedDuration("1ms")) { return false; }
    }
    return !ToDate(Datetime{Minimum}) && ToTime(Datetime{-1}) == Duration{86'399'999} &&
           ToDate(ParsedDatetime("2024-10-28T10:12:13.456+0700")) == ParsedDatetime("2024-10-28") &&
           ToDate(ParsedDatetime("2024-10-28T10:12:13.456-0700")) == ParsedDatetime("2024-10-28") &&
           ToTime(ParsedDatetime("2024-10-28T10:12:13.456+0700")) == ParsedDuration("3h12m13s456ms") &&
           ToTime(ParsedDatetime("2024-10-28T10:12:13.456-0700")) == ParsedDuration("17h12m13s456ms");
}

static_assert(DateAndTimeWork());

/// The duration conversions truncate toward zero.
struct DurationConversionCase
{
    std::string_view text;
    std::int64_t milliseconds;
    std::int64_t seconds;
    std::int64_t minutes;
    std::int64_t hours;
    std::int64_t days;
};

constexpr std::array DurationConversionCases{
    DurationConversionCase{"10d23h59m58s999ms", 950'398'999, 950'398, 15'839, 263, 10},
    DurationConversionCase{"-10d23h59m58s999ms", -950'398'999, -950'398, -15'839, -263, -10},
    DurationConversionCase{"1d2h3m4s50ms", 93'784'050, 93'784, 1'563, 26, 1},
    DurationConversionCase{"-1d2h3m4s50ms", -93'784'050, -93'784, -1'563, -26, -1},
    DurationConversionCase{"-59s", -59'000, -59, 0, 0, 0}, // oracle
    DurationConversionCase{"999ms", 999, 0, 0, 0, 0},
};

constexpr auto DurationConversionMatches(const DurationConversionCase& testCase) -> bool
{
    const Duration duration = ParsedDuration(testCase.text);
    return cedar::detail::ToMilliseconds(duration) == testCase.milliseconds &&
           cedar::detail::ToSeconds(duration) == testCase.seconds && cedar::detail::ToMinutes(duration) == testCase.minutes &&
           cedar::detail::ToHours(duration) == testCase.hours && cedar::detail::ToDays(duration) == testCase.days;
}

static_assert(AllPass(DurationConversionCases, DurationConversionMatches));

constexpr auto ComparisonsWork() -> bool
{
    const Datetime epoch{0};
    const Datetime today = ParsedDatetime("2024-10-24");
    const Datetime yesterday = ParsedDatetime("2024-10-23");
    const Datetime before = ParsedDatetime("1900-01-01");
    return epoch <= epoch && today == today && today != yesterday && epoch < today && today > yesterday && before <= epoch &&
           ParsedDatetime("2024-10-28T10:12:13.456-0700") > ParsedDatetime("2024-10-28T10:12:13.456Z") &&
           ParsedDatetime("2024-10-28T10:12:13.456-0700") == ParsedDatetime("2024-10-28T17:12:13.456Z") &&
           ParsedDatetime("2024-10-28T10:12:13.456-0700") < ParsedDatetime("2024-10-28T17:12:13.456-0800") &&
           ParsedDuration("-2h") < ParsedDuration("1h") && ParsedDuration("-3d") == ParsedDuration("-72h") &&
           ParsedDuration("2h") > ParsedDuration("1h");
}

static_assert(ComparisonsWork());

// ---- the dispatch table ----

static_assert(cedar::detail::ExtensionCalls.size() == 22);
static_assert(cedar::detail::IsExtensionFunction("ip", cedar::detail::ExtensionCallStyle::Function));
static_assert(cedar::detail::IsExtensionFunction("toTime", cedar::detail::ExtensionCallStyle::Method));
static_assert(!cedar::detail::IsExtensionFunction("decimal", cedar::detail::ExtensionCallStyle::Method));
static_assert(cedar::detail::FindExtensionCall("offset")->returnKind == cedar::ValueKind::Datetime);
static_assert(cedar::detail::FindExtensionCall("isInRange") != nullptr);
static_assert(cedar::detail::FindExtensionCall("A::ip") == nullptr);
static_assert(cedar::detail::FindExtensionCall("") == nullptr);

auto Call(const std::string_view name, const std::vector<Value>& arguments) -> std::expected<Value, cedar::EvaluationError>
{
    cedar::ValueArena arena;
    return cedar::detail::CallExtensionFunction(name, arguments, arena);
}

auto Make(const std::string_view constructor, const std::string_view text) -> Value
{
    const auto value = Call(constructor, {Value::FromString(text)});
    EXPECT_TRUE(value.has_value()) << constructor << "(\"" << text << "\")";
    return value.value_or(Value::FromBool(false));
}

auto ErrorKind(const std::expected<Value, cedar::EvaluationError>& result) -> std::optional<EvaluationErrorKind>
{
    if (result) { return std::nullopt; }
    return result.error().Kind();
}

TEST(ExtensionTests, Decimal)
{
    ExpectAllPass(DecimalCases, DecimalCaseMatches);
    ExpectAllPass(DecimalComparisonCases, DecimalComparisonMatches);
    EXPECT_TRUE(AllDecimalsRoundTrip());
}

TEST(ExtensionTests, IpAddress)
{
    ExpectAllPass(IpCases, IpCaseMatches);
    ExpectAllPass(StandardIpCases, StandardIpCaseMatches);
    ExpectAllPass(IpPredicateCases, IpPredicateMatches);
    ExpectAllPass(IpRangeCases, IpRangeMatches);
}

TEST(ExtensionTests, Datetime)
{
    ExpectAllPass(DatetimeCases, DatetimeCaseMatches);
    EXPECT_TRUE(OffsetsWork());
    EXPECT_TRUE(DurationsSinceWork());
    EXPECT_TRUE(DateAndTimeWork());
    EXPECT_TRUE(ComparisonsWork());
}

TEST(ExtensionTests, Duration)
{
    ExpectAllPass(DurationCases, DurationCaseMatches);
    ExpectAllPass(DurationConversionCases, DurationConversionMatches);
}

TEST(ExtensionTests, CallsComputeValues)
{
    const Value decimal = Make("decimal", "-1.23");
    ASSERT_EQ(decimal.Kind(), ValueKind::Decimal);
    EXPECT_EQ(decimal.AsDecimal(), Decimal{-12'300});
    EXPECT_EQ(Call("lessThan", {decimal, Make("decimal", "1.23")})->AsBool(), true);
    EXPECT_EQ(Call("greaterThanOrEqual", {decimal, decimal})->AsBool(), true);

    const Value mapped = Make("ip", "::ffff:ff00:1");
    EXPECT_EQ(Call("isIpv4", {mapped})->AsBool(), false);
    EXPECT_EQ(Call("isIpv6", {mapped})->AsBool(), true);
    EXPECT_EQ(Call("isInRange", {Make("ip", "10.0.0.1"), Make("ip", "10.0.0.0/24")})->AsBool(), true);
    EXPECT_EQ(Call("isLoopback", {Make("ip", "127.0.0.1/8")})->AsBool(), true);
    EXPECT_EQ(Call("isMulticast", {Make("ip", "ff00::2")})->AsBool(), true);

    const Value datetime = Make("datetime", "2024-10-28T10:12:13.456-0700");
    EXPECT_EQ(datetime.AsDatetime(), Datetime{1'730'135'533'456});
    EXPECT_EQ(Call("offset", {datetime, Make("duration", "-7h")})->AsDatetime(), ParsedDatetime("2024-10-28T10:12:13.456Z"));
    EXPECT_EQ(Call("durationSince", {datetime, Make("datetime", "2024-10-28T10:12:13.456Z")})->AsDuration(),
              ParsedDuration("7h"));
    EXPECT_EQ(Call("toDate", {datetime})->AsDatetime(), ParsedDatetime("2024-10-28"));
    EXPECT_EQ(Call("toTime", {datetime})->AsDuration(), ParsedDuration("17h12m13s456ms"));

    const Value duration = Make("duration", "-1d2h3m4s50ms");
    EXPECT_EQ(Call("toMilliseconds", {duration})->AsLong(), -93'784'050);
    EXPECT_EQ(Call("toSeconds", {duration})->AsLong(), -93'784);
    EXPECT_EQ(Call("toMinutes", {duration})->AsLong(), -1'563);
    EXPECT_EQ(Call("toHours", {duration})->AsLong(), -26);
    EXPECT_EQ(Call("toDays", {duration})->AsLong(), -1);
}

TEST(ExtensionTests, CallErrorsHaveRustsKinds)
{
    const Value text = Value::FromString("1.23");
    const Value address = Make("ip", "192.168.1.100");
    const Value datetime = Make("datetime", "2024-10-28");

    // Lookup, then argument count before any argument kind, then kinds left to right.
    EXPECT_EQ(ErrorKind(Call("foo", {text})), EvaluationErrorKind::ExtensionFunctionLookup);
    EXPECT_EQ(ErrorKind(Call("A::ip", {text})), EvaluationErrorKind::ExtensionFunctionLookup);
    EXPECT_EQ(ErrorKind(Call("isInRange", {address, address, address})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("isInRange", {address})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("isInRange", {text})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("isInRange", {})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("toDate", {datetime, Value::FromLong(1)})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("decimal", {text, text})), EvaluationErrorKind::WrongArgumentCount);
    EXPECT_EQ(ErrorKind(Call("decimal", {Value::FromLong(1)})), EvaluationErrorKind::TypeError);
    EXPECT_EQ(ErrorKind(Call("isInRange", {address, Value::FromString("10.0.0.0/8")})), EvaluationErrorKind::TypeError);
    EXPECT_EQ(ErrorKind(Call("offset", {datetime, Value::FromString("7h")})), EvaluationErrorKind::TypeError);
    EXPECT_EQ(ErrorKind(Call("durationSince", {Value::FromString("x"), datetime})), EvaluationErrorKind::TypeError);
    EXPECT_EQ(ErrorKind(Call("toDays", {datetime})), EvaluationErrorKind::TypeError);

    const auto stringForDecimal = Call("lessThan", {Make("decimal", "-1.23"), text});
    ASSERT_FALSE(stringForDecimal.has_value());
    EXPECT_EQ(stringForDecimal.error().Help(), "maybe you forgot to apply the `decimal` constructor?");
    const auto stringForIp = Call("isLoopback", {text});
    ASSERT_FALSE(stringForIp.has_value());
    EXPECT_EQ(stringForIp.error().Help(), "maybe you forgot to apply the `ip` constructor?");

    // Parse failures and overflows are extension function execution errors.
    EXPECT_EQ(ErrorKind(Call("decimal", {Value::FromString("1.0.")})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(ErrorKind(Call("ip", {Value::FromString("::ffff:127.0.0.1")})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(ErrorKind(Call("datetime", {Value::FromString("2024-02-30")})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(ErrorKind(Call("duration", {Value::FromString("9223372036854775808ms")})),
              EvaluationErrorKind::ExtensionFunctionExecution);
    const Value latest = Value::FromDatetime(Datetime{Maximum});
    const Value earliest = Value::FromDatetime(Datetime{Minimum});
    EXPECT_EQ(ErrorKind(Call("offset", {latest, Make("duration", "1ms")})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(ErrorKind(Call("durationSince", {datetime, earliest})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(ErrorKind(Call("toDate", {earliest})), EvaluationErrorKind::ExtensionFunctionExecution);
    EXPECT_EQ(Call("toTime", {earliest})->AsDuration(), cedar::detail::ToTime(Datetime{Minimum}));
}

} // namespace
