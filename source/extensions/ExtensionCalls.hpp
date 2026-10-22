#pragma once

#include <cedar/Errors.hpp>
#include <cedar/Value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

namespace cedar::detail
{

/// Rust's experimental `variadic-is-in-range` feature, off as in the default build: when true,
/// `isInRange` takes one or more ranges after the receiver and is true if any contains it.
inline constexpr bool VariadicIsInRange = false;

/// How an extension function is written: `ip("10.0.0.1")` or `a.lessThan(b)` (Rust's
/// `CallStyle`). The parser refuses the other style, and the printer writes this one.
enum class ExtensionCallStyle : std::uint8_t
{
    Function,
    Method,
};

/// The body of one extension function. CallExtensionFunction has already checked the argument
/// count and each argument's kind against the table row, so a body only computes.
using ExtensionFunctionBody = auto (*)(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;

/// One function of the default extensions (`decimal`, `ipaddr`, `datetime`): its unqualified
/// name, how it is written, its argument count (a method's receiver counts), whether the last
/// argument may repeat, the kind each argument must have, the kind it returns, and its body.
struct ExtensionCall
{
    std::string_view name;
    ExtensionCallStyle style;
    std::uint8_t arity;
    bool lastArgumentRepeats;
    std::array<ValueKind, 2> argumentKinds;
    ValueKind returnKind;
    ExtensionFunctionBody body;
};

namespace extension_bodies
{

// Bodies, defined in ExtensionCalls.cpp. Their arguments are already of the table's kinds.
auto ConstructDatetime(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ConstructDecimal(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ConstructDuration(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto DurationSince(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto GreaterThan(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto GreaterThanOrEqual(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ConstructIp(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto IsInRange(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto IsIpv4(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto IsIpv6(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto IsLoopback(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto IsMulticast(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto LessThan(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto LessThanOrEqual(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto Offset(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToDate(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToDays(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToHours(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToMilliseconds(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToMinutes(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToSeconds(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;
auto ToTime(std::span<const Value> arguments) -> std::expected<Value, EvaluationError>;

} // namespace extension_bodies

/// Every default extension function, sorted by name (byte order); the one table the parser, the
/// printer, the evaluator and the validator read. The rows follow the
/// `ExtensionFunction::{unary,binary,variadic}` registrations in
/// `cedar-policy-core/src/extensions/{decimal,ipaddr,datetime}.rs`. A one-argument row repeats its
/// kind in the unused second slot. `isInRange` is registered as variadic, which without the
/// `variadic-is-in-range` feature means exactly two arguments.
inline constexpr std::array ExtensionCalls{
    ExtensionCall{"datetime", ExtensionCallStyle::Function, 1, false,
                  {ValueKind::String, ValueKind::String}, ValueKind::Datetime, &extension_bodies::ConstructDatetime},
    ExtensionCall{"decimal", ExtensionCallStyle::Function, 1, false,
                  {ValueKind::String, ValueKind::String}, ValueKind::Decimal, &extension_bodies::ConstructDecimal},
    ExtensionCall{"duration", ExtensionCallStyle::Function, 1, false,
                  {ValueKind::String, ValueKind::String}, ValueKind::Duration, &extension_bodies::ConstructDuration},
    ExtensionCall{"durationSince", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Datetime, ValueKind::Datetime}, ValueKind::Duration, &extension_bodies::DurationSince},
    ExtensionCall{"greaterThan", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Decimal, ValueKind::Decimal}, ValueKind::Bool, &extension_bodies::GreaterThan},
    ExtensionCall{"greaterThanOrEqual", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Decimal, ValueKind::Decimal}, ValueKind::Bool, &extension_bodies::GreaterThanOrEqual},
    ExtensionCall{"ip", ExtensionCallStyle::Function, 1, false,
                  {ValueKind::String, ValueKind::String}, ValueKind::IpAddress, &extension_bodies::ConstructIp},
    ExtensionCall{"isInRange", ExtensionCallStyle::Method, 2, VariadicIsInRange,
                  {ValueKind::IpAddress, ValueKind::IpAddress}, ValueKind::Bool, &extension_bodies::IsInRange},
    ExtensionCall{"isIpv4", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::IpAddress, ValueKind::IpAddress}, ValueKind::Bool, &extension_bodies::IsIpv4},
    ExtensionCall{"isIpv6", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::IpAddress, ValueKind::IpAddress}, ValueKind::Bool, &extension_bodies::IsIpv6},
    ExtensionCall{"isLoopback", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::IpAddress, ValueKind::IpAddress}, ValueKind::Bool, &extension_bodies::IsLoopback},
    ExtensionCall{"isMulticast", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::IpAddress, ValueKind::IpAddress}, ValueKind::Bool, &extension_bodies::IsMulticast},
    ExtensionCall{"lessThan", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Decimal, ValueKind::Decimal}, ValueKind::Bool, &extension_bodies::LessThan},
    ExtensionCall{"lessThanOrEqual", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Decimal, ValueKind::Decimal}, ValueKind::Bool, &extension_bodies::LessThanOrEqual},
    ExtensionCall{"offset", ExtensionCallStyle::Method, 2, false,
                  {ValueKind::Datetime, ValueKind::Duration}, ValueKind::Datetime, &extension_bodies::Offset},
    ExtensionCall{"toDate", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Datetime, ValueKind::Datetime}, ValueKind::Datetime, &extension_bodies::ToDate},
    ExtensionCall{"toDays", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Duration, ValueKind::Duration}, ValueKind::Long, &extension_bodies::ToDays},
    ExtensionCall{"toHours", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Duration, ValueKind::Duration}, ValueKind::Long, &extension_bodies::ToHours},
    ExtensionCall{"toMilliseconds", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Duration, ValueKind::Duration}, ValueKind::Long, &extension_bodies::ToMilliseconds},
    ExtensionCall{"toMinutes", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Duration, ValueKind::Duration}, ValueKind::Long, &extension_bodies::ToMinutes},
    ExtensionCall{"toSeconds", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Duration, ValueKind::Duration}, ValueKind::Long, &extension_bodies::ToSeconds},
    ExtensionCall{"toTime", ExtensionCallStyle::Method, 1, false,
                  {ValueKind::Datetime, ValueKind::Datetime}, ValueKind::Duration, &extension_bodies::ToTime},
};

static_assert(std::ranges::is_sorted(ExtensionCalls, {}, &ExtensionCall::name));
static_assert(std::ranges::adjacent_find(ExtensionCalls, {}, &ExtensionCall::name) == ExtensionCalls.end());

/// The extension function called `name`, or null. A qualified name never matches: Rust registers
/// every extension function unqualified.
[[nodiscard]] constexpr auto FindExtensionCall(const std::string_view name) noexcept -> const ExtensionCall*
{
    const auto* found = std::ranges::lower_bound(ExtensionCalls, name, {}, &ExtensionCall::name);
    if (found == ExtensionCalls.end() || found->name != name) { return nullptr; }
    return found;
}

/// True when `name` is an extension function written in `style`.
[[nodiscard]] constexpr auto IsExtensionFunction(const std::string_view name, const ExtensionCallStyle style) noexcept
    -> bool
{
    const ExtensionCall* found = FindExtensionCall(name);
    return found != nullptr && found->style == style;
}

/// The name of the extension type whose values have `kind` (Rust's `ExtensionValue::typename`:
/// `datetime`, `decimal`, `duration`, `ipaddr`), or empty for a kind that is not an extension.
/// A type's name is not always its constructor's (`ip` makes an `ipaddr`).
[[nodiscard]] constexpr auto ExtensionTypeName(const ValueKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case ValueKind::Datetime: return "datetime";
    case ValueKind::Decimal: return "decimal";
    case ValueKind::Duration: return "duration";
    case ValueKind::IpAddress: return "ipaddr";
    case ValueKind::Bool:
    case ValueKind::Long:
    case ValueKind::String:
    case ValueKind::Entity:
    case ValueKind::Set:
    case ValueKind::Record: return {};
    }
    return {};
}

/// The extension types (Rust's `Extensions::ext_types`): the kinds the constructors, the
/// Function-style rows, return, in table order.
inline constexpr auto ExtensionTypes = []
{
    constexpr auto count = std::ranges::count(ExtensionCalls, ExtensionCallStyle::Function, &ExtensionCall::style);
    std::array<ValueKind, count> kinds{};
    std::size_t next = 0;
    for (const ExtensionCall& call : ExtensionCalls)
    {
        if (call.style == ExtensionCallStyle::Function) { kinds[next++] = call.returnKind; }
    }
    return kinds;
}();

static_assert(std::ranges::none_of(ExtensionTypes, [](const ValueKind kind) { return ExtensionTypeName(kind).empty(); }));

/// The extension type called `name` (unqualified), or nothing.
[[nodiscard]] constexpr auto FindExtensionType(const std::string_view name) noexcept -> std::optional<ValueKind>
{
    for (const ValueKind kind : ExtensionTypes)
    {
        if (ExtensionTypeName(kind) == name) { return kind; }
    }
    return std::nullopt;
}

/// Calls the extension function `name` on already evaluated `arguments` (a method's receiver
/// first), as Rust's evaluator does for `ExtensionFunctionApp` (`evaluator.rs`, `ExtensionFunction::call`
/// in `ast/extension.rs`). The checks run in Rust's order:
///   1. an unknown name is `ExtensionFunctionLookup`;
///   2. a wrong argument count is `WrongArgumentCount`, before any argument is looked at;
///   3. arguments are checked left to right, and the first of the wrong kind is `TypeError`;
///   4. a constructor argument that does not parse, or an `offset`, `durationSince` or `toDate`
///      that overflows, is `ExtensionFunctionExecution`.
/// No error carries a span: the evaluator attaches the call's. `arena` holds any composite value a
/// function makes; no default extension function makes one.
[[nodiscard]] auto CallExtensionFunction(std::string_view name, std::span<const Value> arguments, ValueArena& arena)
    -> std::expected<Value, EvaluationError>;

} // namespace cedar::detail
