#pragma once

#include "Arguments.hpp"

#include <cedar/Diagnostic.hpp>

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cedar::cli
{

/// The process exit status, the values the Rust CLI uses (`cedar-policy-cli/src/lib.rs`
/// `CedarExitCode`, and clap's 2 for a malformed command line).
enum class ExitCode : int
{
    Success = 0,
    Failure = 1,
    Usage = 2,
    /// `authorize` answered Deny.
    AuthorizeDeny = 2,
    /// `validate` found an error (or, with `--deny-warnings`, a warning).
    ValidationFailure = 3,
};

/// How failures are printed (`--error-format`). Plain and human print the same text here.
enum class ErrorFormat
{
    Human,
    Plain,
    Json,
};

/// A failure to report: what failed, and the diagnostics that say why.
struct Failure
{
    std::string message;
    std::vector<Diagnostic> causes{};
};

/// Prints `failure` to standard output, where the Rust CLI prints a command's errors.
auto Report(ErrorFormat format, const Failure& failure) -> void;

/// One run of a command: the options given on the command line, the positional argument when the
/// command takes one, and the error format.
struct Invocation
{
    const ParsedArguments& arguments;
    std::optional<std::string_view> positional;
    ErrorFormat errorFormat;
};

/// A subcommand. `options` lists the command's own options; the global ones are added by Main.
/// `positionalName` names the one required positional argument the command takes, if any (as
/// `EXPRESSION`). `run` returns a UsageError for an option value it refuses before doing any work.
struct Command
{
    std::string_view name;
    std::string_view summary;
    std::span<const OptionSpecification> options;
    auto (*run)(const Invocation& invocation) -> std::expected<ExitCode, UsageError>;
    std::string_view positionalName{};
};

/// Every command, in the order help lists them.
[[nodiscard]] auto Commands() noexcept -> std::span<const Command* const>;

} // namespace cedar::cli
