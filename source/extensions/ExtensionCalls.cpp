#include "extensions/ExtensionCalls.hpp"

#include "extensions/Datetime.hpp"
#include "extensions/Decimal.hpp"
#include "extensions/IpAddress.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace cedar::detail
{

namespace
{

constexpr std::string_view DecimalHelp =
    "valid decimal strings look like `12.34`: digits are required on both sides of `.`, up to 4 fractional digits are "
    "allowed, and the value must be in range -922337203685477.5808 to 922337203685477.5807";
constexpr std::string_view IpHelp =
    "valid IP strings are IPv4/IPv6 addresses or CIDR ranges like `127.0.0.1`, `127.0.0.1/24`, or `ffee::/64`";
constexpr std::string_view DatetimeHelp = "valid datetime strings start with `YYYY-MM-DD` and may optionally include "
                                          "`THH:MM:SS` plus `Z`, `.SSSZ`, or an offset like `+0700`";
constexpr std::string_view DurationHelp = "valid duration strings are concatenated quantity-unit pairs with an optional "
                                          "leading `-`, for example `1d2h3m4s50ms`";

/// The Cedar type name of a value kind, as Rust's `Type` displays it.
constexpr auto TypeName(const ValueKind kind) noexcept -> std::string_view
{
    switch (kind)
    {
    case ValueKind::Bool: return "bool";
    case ValueKind::Long: return "long";
    case ValueKind::String: return "string";
    case ValueKind::Entity: return "entity";
    case ValueKind::Set: return "set";
    case ValueKind::Record: return "record";
    case ValueKind::Datetime: return "datetime";
    case ValueKind::Decimal: return "decimal";
    case ValueKind::Duration: return "duration";
    case ValueKind::IpAddress: return "ipaddr";
    }
    return "value";
}

/// The constructor that makes values of an extension kind.
constexpr auto ConstructorName(const ValueKind kind) noexcept -> std::string_view
{
    return kind == ValueKind::IpAddress ? "ip" : TypeName(kind);
}

auto Quoted(const std::string_view text) -> std::string
{
    std::string quoted{"`"};
    quoted.append(text);
    quoted.push_back('`');
    return quoted;
}

auto ExecutionError(std::string message, const std::string_view help) -> std::unexpected<EvaluationError>
{
    return std::unexpected(EvaluationError{EvaluationErrorKind::ExtensionFunctionExecution, std::nullopt, std::move(message),
                                           std::string{help}});
}

auto ExecutionError(std::string message) -> std::unexpected<EvaluationError>
{
    return std::unexpected(EvaluationError{EvaluationErrorKind::ExtensionFunctionExecution, std::nullopt, std::move(message)});
}

auto ParseFailure(const std::string_view argument, const std::string_view description, const std::string_view help)
    -> std::unexpected<EvaluationError>
{
    std::string message = Quoted(argument);
    message.push_back(' ');
    message.append(description);
    return ExecutionError(std::move(message), help);
}

auto CanonicalCall(const Datetime value) -> std::string
{
    std::string text;
    AppendCanonicalCall(text, value);
    return text;
}

auto CanonicalCall(const Duration value) -> std::string
{
    std::string text;
    AppendCanonicalCall(text, value);
    return text;
}

} // namespace

namespace extension_bodies
{

auto ConstructDecimal(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const std::string_view text = arguments[0].AsString();
    const auto parsed = ParseDecimal(text);
    if (!parsed) { return ParseFailure(text, Describe(parsed.error()), DecimalHelp); }
    return Value::FromDecimal(*parsed);
}

auto LessThan(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(arguments[0].AsDecimal() < arguments[1].AsDecimal());
}

auto LessThanOrEqual(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(arguments[0].AsDecimal() <= arguments[1].AsDecimal());
}

auto GreaterThan(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(arguments[0].AsDecimal() > arguments[1].AsDecimal());
}

auto GreaterThanOrEqual(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(arguments[0].AsDecimal() >= arguments[1].AsDecimal());
}

auto ConstructIp(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const std::string_view text = arguments[0].AsString();
    const auto parsed = ParseIpAddress(text);
    if (!parsed) { return ParseFailure(text, Describe(parsed.error()), IpHelp); }
    return Value::FromIpAddress(*parsed);
}

auto IsIpv4(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(detail::IsIpv4(arguments[0].AsIpAddress()));
}

auto IsIpv6(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(detail::IsIpv6(arguments[0].AsIpAddress()));
}

auto IsLoopback(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(detail::IsLoopback(arguments[0].AsIpAddress()));
}

auto IsMulticast(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromBool(detail::IsMulticast(arguments[0].AsIpAddress()));
}

auto IsInRange(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const IpAddress address = arguments[0].AsIpAddress();
    bool inAnyRange = false;
    for (const Value& range : arguments.subspan(1)) { inAnyRange = inAnyRange || detail::IsInRange(address, range.AsIpAddress()); }
    return Value::FromBool(inAnyRange);
}

auto ConstructDatetime(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const std::string_view text = arguments[0].AsString();
    const auto parsed = ParseDatetime(text);
    if (!parsed) { return ParseFailure(text, Describe(parsed.error()), DatetimeHelp); }
    return Value::FromDatetime(*parsed);
}

auto ConstructDuration(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const std::string_view text = arguments[0].AsString();
    const auto parsed = ParseDuration(text);
    if (!parsed) { return ParseFailure(text, Describe(parsed.error()), DurationHelp); }
    return Value::FromDuration(*parsed);
}

auto Offset(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const Datetime datetime = arguments[0].AsDatetime();
    const Duration duration = arguments[1].AsDuration();
    const auto result = detail::Offset(datetime, duration);
    if (!result)
    {
        return ExecutionError("overflow when offsetting " + CanonicalCall(datetime) + " by " + CanonicalCall(duration));
    }
    return Value::FromDatetime(*result);
}

auto DurationSince(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const Datetime later = arguments[0].AsDatetime();
    const Datetime earlier = arguments[1].AsDatetime();
    const auto result = detail::DurationSince(later, earlier);
    if (!result)
    {
        return ExecutionError("overflow when computing the duration between " + CanonicalCall(later) + " and " +
                              CanonicalCall(earlier));
    }
    return Value::FromDuration(*result);
}

auto ToDate(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    const Datetime datetime = arguments[0].AsDatetime();
    const auto result = detail::ToDate(datetime);
    if (!result) { return ExecutionError("overflow when computing the date of " + CanonicalCall(datetime)); }
    return Value::FromDatetime(*result);
}

auto ToTime(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromDuration(detail::ToTime(arguments[0].AsDatetime()));
}

auto ToMilliseconds(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromLong(detail::ToMilliseconds(arguments[0].AsDuration()));
}

auto ToSeconds(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromLong(detail::ToSeconds(arguments[0].AsDuration()));
}

auto ToMinutes(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromLong(detail::ToMinutes(arguments[0].AsDuration()));
}

auto ToHours(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromLong(detail::ToHours(arguments[0].AsDuration()));
}

auto ToDays(const std::span<const Value> arguments) -> std::expected<Value, EvaluationError>
{
    return Value::FromLong(detail::ToDays(arguments[0].AsDuration()));
}

} // namespace extension_bodies

auto CallExtensionFunction(const std::string_view name, const std::span<const Value> arguments,
                           [[maybe_unused]] ValueArena& arena) -> std::expected<Value, EvaluationError>
{
    const ExtensionCall* function = FindExtensionCall(name);
    if (function == nullptr)
    {
        return std::unexpected(EvaluationError{EvaluationErrorKind::ExtensionFunctionLookup, std::nullopt,
                                               Quoted(name) + " is not an extension function"});
    }

    const bool countMatches = function->lastArgumentRepeats ? arguments.size() >= function->arity
                                                            : arguments.size() == function->arity;
    if (!countMatches)
    {
        std::string message = Quoted(name) + " expects " + (function->lastArgumentRepeats ? "at least " : "") +
                              std::to_string(function->arity) + (function->arity == 1 ? " argument" : " arguments") +
                              ", got " + std::to_string(arguments.size());
        return std::unexpected(EvaluationError{EvaluationErrorKind::WrongArgumentCount, std::nullopt, std::move(message)});
    }

    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::size_t slot = index < function->arity ? index : function->arity - 1U;
        const ValueKind expected = function->argumentKinds[slot];
        const ValueKind actual = arguments[index].Kind();
        if (actual == expected) { continue; }
        std::string message = "type error: expected ";
        message.append(TypeName(expected)).append(", got ").append(TypeName(actual));
        std::optional<std::string> help;
        if (actual == ValueKind::String)
        {
            help = "maybe you forgot to apply the " + Quoted(ConstructorName(expected)) + " constructor?";
        }
        return std::unexpected(EvaluationError{EvaluationErrorKind::TypeError, std::nullopt, std::move(message), std::move(help)});
    }

    return function->body(arguments);
}

} // namespace cedar::detail
