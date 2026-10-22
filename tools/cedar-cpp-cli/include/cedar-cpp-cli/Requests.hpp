#pragma once

#include <cedar-cpp-cli/Arguments.hpp>
#include <cedar-cpp-cli/Command.hpp>

#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>

#include <array>
#include <expected>
#include <optional>

namespace cedar::cli
{

// The request options shared by `authorize` and `evaluate` (`cedar-policy-cli/src/utils/request.rs`
// `RequestArgs`).

inline constexpr OptionSpecification PrincipalOption{
    .longName = "principal", .shortName = 'l', .valueName = "PRINCIPAL",
    .help = "Principal for the request, e.g., User::\"alice\""};
inline constexpr OptionSpecification ActionOption{
    .longName = "action", .shortName = 'a', .valueName = "ACTION",
    .help = "Action for the request, e.g., Action::\"view\""};
inline constexpr OptionSpecification ResourceOption{
    .longName = "resource", .shortName = 'r', .valueName = "RESOURCE",
    .help = "Resource for the request, e.g., File::\"myfile.txt\""};
inline constexpr OptionSpecification ContextOption{
    .longName = "context", .shortName = 'c', .valueName = "FILE",
    .help = "File containing a JSON object representing the context for the request"};
inline constexpr OptionSpecification RequestJsonOption{
    .longName = "request-json", .valueName = "FILE",
    .help = "File containing a JSON object representing the entire request, with fields \"principal\", "
            "\"action\", \"resource\" and \"context\". Replaces --principal, --action, --resource and --context"};
inline constexpr OptionSpecification RequestValidationOption{
    .longName = "request-validation", .valueName = "REQUEST_VALIDATION",
    .help = "Whether to enable request validation. Has no effect without a schema [default: true] "
            "[possible values: true, false]"};

inline constexpr std::array RequestOptions{PrincipalOption, ActionOption,      ResourceOption,
                                           ContextOption,   RequestJsonOption, RequestValidationOption};

/// Refuses, as clap does, `--request-json` together with any of `--principal`, `--action`,
/// `--resource` and `--context`, and a `--request-validation` other than `true` or `false`.
[[nodiscard]] auto CheckRequestArguments(const ParsedArguments& arguments) -> std::expected<void, UsageError>;

/// The request the options describe (`RequestArgs::get_request`): read from `--request-json`, or
/// built from `--principal`, `--action`, `--resource` (all three required) and the optional
/// `--context` file. With a schema the context is read by the type the schema declares for the
/// action, and unless `--request-validation false` the request is checked against the schema.
[[nodiscard]] auto LoadRequest(const ParsedArguments& arguments, const std::optional<Schema>& schema)
    -> std::expected<Request, Failure>;

} // namespace cedar::cli
