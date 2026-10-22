#pragma once

#include "extensions/Digits.hpp"

#include <cedar/ExtensionValues.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

namespace cedar::detail
{

// The `datetime` extension (`cedar-policy-core/src/extensions/datetime.rs`): the `datetime` and
// `duration` constructors and their methods. Both types are i64 milliseconds.

/// Why `datetime(...)` refused its argument (`DateTimeParseError`).
enum class DatetimeParseError : std::uint8_t
{
    InvalidDatePattern,       ///< Does not start with `YYYY-MM-DD`.
    InvalidDate,              ///< The date is not a day of the proleptic Gregorian calendar.
    InvalidTimePattern,       ///< Something other than `THH:MM:SS` follows the date.
    InvalidTime,              ///< Hour 24 or more, minute or second 60 or more (no leap seconds).
    InvalidMillisecondOrZone, ///< Not `Z`, `.SSSZ`, `(+|-)hhmm` or `.SSS(+|-)hhmm` after the time.
    InvalidOffset,            ///< Offset hours 24 or more, or offset minutes 60 or more.
};

[[nodiscard]] constexpr auto Describe(const DatetimeParseError error) noexcept -> std::string_view
{
    switch (error)
    {
    case DatetimeParseError::InvalidDatePattern: return "does not start with a YYYY-MM-DD date";
    case DatetimeParseError::InvalidDate: return "names a date that does not exist";
    case DatetimeParseError::InvalidTimePattern: return "does not have THH:MM:SS after the date";
    case DatetimeParseError::InvalidTime: return "names a time of day that does not exist";
    case DatetimeParseError::InvalidMillisecondOrZone:
        return "does not end with Z, .SSSZ, (+|-)hhmm or .SSS(+|-)hhmm after the time";
    case DatetimeParseError::InvalidOffset: return "has an offset outside [00:00, 24:00)";
    }
    return "is not a valid datetime";
}

/// Why `duration(...)` refused its argument (`DurationParseError`).
enum class DurationParseError : std::uint8_t
{
    InvalidPattern, ///< Not an optional `-` followed by quantity-unit pairs in the order d, h, m, s, ms.
    Overflow,       ///< The total does not fit in a signed 64-bit count of milliseconds.
};

[[nodiscard]] constexpr auto Describe(const DurationParseError error) noexcept -> std::string_view
{
    switch (error)
    {
    case DurationParseError::InvalidPattern: return "is not a sequence of quantity-unit pairs in the order d, h, m, s, ms";
    case DurationParseError::Overflow: return "does not fit in a signed 64-bit number of milliseconds";
    }
    return "is not a valid duration";
}

inline constexpr std::int64_t MillisecondsPerDay = 1000 * 3600 * 24;

/// The value of exactly `count` ASCII digits at `position` of `text`, or nothing.
[[nodiscard]] constexpr auto FixedDigits(const std::string_view text, const std::size_t position, const std::size_t count) noexcept
    -> std::optional<std::uint32_t>
{
    if (position + count > text.size()) { return std::nullopt; }
    std::uint32_t value = 0;
    for (std::size_t index = position; index < position + count; ++index)
    {
        if (!IsAsciiDigit(text[index])) { return std::nullopt; }
        value = value * 10 + static_cast<std::uint32_t>(text[index] - '0');
    }
    return value;
}

/// Parses the argument of `datetime(...)`. The five accepted shapes are `YYYY-MM-DD` (midnight
/// UTC), `YYYY-MM-DDTHH:MM:SSZ`, `YYYY-MM-DDTHH:MM:SS.SSSZ`, `YYYY-MM-DDTHH:MM:SS(+|-)hhmm` and
/// `YYYY-MM-DDTHH:MM:SS.SSS(+|-)hhmm`. The year is exactly four digits (0000 to 9999). An offset
/// `+hhmm` means the local time is ahead of UTC, so it is subtracted. Pattern errors are found
/// left to right before any range check, as Rust does.
[[nodiscard]] constexpr auto ParseDatetime(const std::string_view text) noexcept -> std::expected<Datetime, DatetimeParseError>
{
    const auto year = FixedDigits(text, 0, 4);
    const auto month = FixedDigits(text, 5, 2);
    const auto day = FixedDigits(text, 8, 2);
    if (!year || !month || !day || text[4] != '-' || text[7] != '-')
    {
        return std::unexpected(DatetimeParseError::InvalidDatePattern);
    }
    const std::chrono::year_month_day date{std::chrono::year{static_cast<int>(*year)}, std::chrono::month{*month},
                                           std::chrono::day{*day}};
    const auto dateMilliseconds = [&]() -> std::expected<std::int64_t, DatetimeParseError> {
        if (!date.ok()) { return std::unexpected(DatetimeParseError::InvalidDate); }
        return static_cast<std::int64_t>(std::chrono::sys_days{date}.time_since_epoch().count()) * MillisecondsPerDay;
    };
    if (text.size() == 10) { return dateMilliseconds().transform([](const std::int64_t value) { return Datetime{value}; }); }

    // `^T([0-9]{2}):([0-9]{2}):([0-9]{2})` after the date.
    const auto hour = FixedDigits(text, 11, 2);
    const auto minute = FixedDigits(text, 14, 2);
    const auto second = FixedDigits(text, 17, 2);
    if (text[10] != 'T' || !hour || !minute || !second || text[13] != ':' || text[16] != ':')
    {
        return std::unexpected(DatetimeParseError::InvalidTimePattern);
    }

    // `^(\.([0-9]{3}))?(Z|((\+|-)([0-9]{2})([0-9]{2})))$` after the time.
    std::size_t position = 19;
    std::uint32_t millisecond = 0;
    if (position < text.size() && text[position] == '.')
    {
        const auto digits = FixedDigits(text, position + 1, 3);
        if (!digits) { return std::unexpected(DatetimeParseError::InvalidMillisecondOrZone); }
        millisecond = *digits;
        position += 4;
    }
    bool hasOffset = false;
    bool offsetPositive = true;
    std::uint32_t offsetHours = 0;
    std::uint32_t offsetMinutes = 0;
    if (position + 1 == text.size() && text[position] == 'Z') {}
    else if (position + 5 == text.size() && (text[position] == '+' || text[position] == '-') &&
             FixedDigits(text, position + 1, 4))
    {
        hasOffset = true;
        offsetPositive = text[position] == '+';
        offsetHours = *FixedDigits(text, position + 1, 2);
        offsetMinutes = *FixedDigits(text, position + 3, 2);
    }
    else
    {
        return std::unexpected(DatetimeParseError::InvalidMillisecondOrZone);
    }

    const auto dayStart = dateMilliseconds();
    if (!dayStart) { return std::unexpected(dayStart.error()); }
    if (*hour >= 24 || *minute >= 60 || *second >= 60) { return std::unexpected(DatetimeParseError::InvalidTime); }
    if (hasOffset && (offsetHours >= 24 || offsetMinutes >= 60)) { return std::unexpected(DatetimeParseError::InvalidOffset); }
    const std::int64_t offsetSeconds = static_cast<std::int64_t>(offsetHours * 3600 + offsetMinutes * 60);
    const std::int64_t timeOfDay = static_cast<std::int64_t>(*hour) * 3'600'000 + static_cast<std::int64_t>(*minute) * 60'000 +
                                   static_cast<std::int64_t>(*second) * 1000 + static_cast<std::int64_t>(millisecond);
    return Datetime{*dayStart + timeOfDay - (offsetPositive ? offsetSeconds : -offsetSeconds) * 1000};
}

/// Parses the argument of `duration(...)`: an optional `-`, then one or more quantity-unit pairs
/// with units in the order `d`, `h`, `m`, `s`, `ms`, each at most once (Rust's
/// `^-?((\d+)d)?((\d+)h)?((\d+)m)?((\d+)s)?((\d+)ms)?$`, which has exactly one way to match). A
/// leading `-` negates every quantity. The total is accumulated with checked arithmetic as
/// `parse_duration` does (milliseconds first, then seconds, minutes, hours, days); any overflow is
/// a parse error, never an integer overflow.
[[nodiscard]] constexpr auto ParseDuration(const std::string_view text) noexcept -> std::expected<Duration, DurationParseError>
{
    if (text.empty() || text == "-") { return std::unexpected(DurationParseError::InvalidPattern); }
    const bool negative = text.front() == '-';
    std::size_t position = negative ? 1 : 0;

    // Quantity text per unit, in unit order d, h, m, s, ms.
    std::array<std::string_view, 5> quantities{};
    std::size_t firstAllowedUnit = 0;
    while (position < text.size())
    {
        const std::size_t digitsBegin = position;
        while (position < text.size() && IsAsciiDigit(text[position])) { ++position; }
        if (position == digitsBegin || position == text.size()) { return std::unexpected(DurationParseError::InvalidPattern); }
        const std::string_view digits = text.substr(digitsBegin, position - digitsBegin);
        std::size_t unit = 0;
        switch (text[position])
        {
        case 'd': unit = 0; break;
        case 'h': unit = 1; break;
        case 'm':
            if (position + 1 < text.size() && text[position + 1] == 's')
            {
                unit = 4;
                ++position;
            }
            else
            {
                unit = 2;
            }
            break;
        case 's': unit = 3; break;
        default: return std::unexpected(DurationParseError::InvalidPattern);
        }
        ++position;
        if (unit < firstAllowedUnit) { return std::unexpected(DurationParseError::InvalidPattern); }
        firstAllowedUnit = unit + 1;
        quantities[unit] = digits;
    }

    std::array<std::uint64_t, 5> values{};
    for (std::size_t unit = 0; unit < 5; ++unit)
    {
        if (quantities[unit].empty()) { continue; }
        const auto value = ParseUnsignedDigits(quantities[unit]);
        if (!value) { return std::unexpected(DurationParseError::Overflow); }
        values[unit] = *value;
    }

    constexpr std::uint64_t largestMagnitude = std::uint64_t{1} << 63;
    if (values[4] > (negative ? largestMagnitude : largestMagnitude - 1)) { return std::unexpected(DurationParseError::Overflow); }
    std::int64_t total = negative ? static_cast<std::int64_t>(0 - values[4]) : static_cast<std::int64_t>(values[4]);
    constexpr std::array<std::int64_t, 4> multipliers{MillisecondsPerDay, 3'600'000, 60'000, 1000};
    for (std::size_t step = 0; step < 4; ++step)
    {
        const std::size_t unit = 3 - step;
        if (values[unit] > largestMagnitude - 1) { return std::unexpected(DurationParseError::Overflow); }
        std::int64_t term = 0;
        if (__builtin_mul_overflow(static_cast<std::int64_t>(values[unit]), multipliers[unit], &term))
        {
            return std::unexpected(DurationParseError::Overflow);
        }
        const bool overflowed = negative ? __builtin_sub_overflow(total, term, &total) : __builtin_add_overflow(total, term, &total);
        if (overflowed) { return std::unexpected(DurationParseError::Overflow); }
    }
    return Duration{total};
}

/// `datetime.offset(duration)`, or nothing on overflow.
[[nodiscard]] constexpr auto Offset(const Datetime datetime, const Duration duration) noexcept -> std::optional<Datetime>
{
    std::int64_t result = 0;
    if (__builtin_add_overflow(datetime.millisecondsSinceEpoch, duration.milliseconds, &result)) { return std::nullopt; }
    return Datetime{result};
}

/// `later.durationSince(earlier)` = `later - earlier`, or nothing on overflow.
[[nodiscard]] constexpr auto DurationSince(const Datetime later, const Datetime earlier) noexcept -> std::optional<Duration>
{
    std::int64_t result = 0;
    if (__builtin_sub_overflow(later.millisecondsSinceEpoch, earlier.millisecondsSinceEpoch, &result)) { return std::nullopt; }
    return Duration{result};
}

/// The time of day in milliseconds: the Euclidean remainder by one day, so always in [0, 1 day).
[[nodiscard]] constexpr auto ToTime(const Datetime datetime) noexcept -> Duration
{
    const std::int64_t remainder = datetime.millisecondsSinceEpoch % MillisecondsPerDay;
    return Duration{remainder < 0 ? remainder + MillisecondsPerDay : remainder};
}

/// Midnight UTC of the datetime's day (rounding toward negative infinity), or nothing when that
/// lies below the i64 range.
[[nodiscard]] constexpr auto ToDate(const Datetime datetime) noexcept -> std::optional<Datetime>
{
    std::int64_t result = 0;
    if (__builtin_sub_overflow(datetime.millisecondsSinceEpoch, ToTime(datetime).milliseconds, &result)) { return std::nullopt; }
    return Datetime{result};
}

// The duration conversions truncate toward zero, each from the previous unit as Rust chains them.

[[nodiscard]] constexpr auto ToMilliseconds(const Duration duration) noexcept -> std::int64_t { return duration.milliseconds; }

[[nodiscard]] constexpr auto ToSeconds(const Duration duration) noexcept -> std::int64_t { return ToMilliseconds(duration) / 1000; }

[[nodiscard]] constexpr auto ToMinutes(const Duration duration) noexcept -> std::int64_t { return ToSeconds(duration) / 60; }

[[nodiscard]] constexpr auto ToHours(const Duration duration) noexcept -> std::int64_t { return ToMinutes(duration) / 60; }

[[nodiscard]] constexpr auto ToDays(const Duration duration) noexcept -> std::int64_t { return ToHours(duration) / 24; }

} // namespace cedar::detail
