#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar::cli
{

/// One command-line option. Matching follows clap, the Rust CLI's argument library: a long option is
/// `--name value` or `--name=value`, a short one `-n value`, `-nvalue` or `-n=value`, and short flags
/// may be grouped (`-vt`, `-vp file`). A separate value may not start with `-` (except `-` alone),
/// and an option given twice is an error.
struct OptionSpecification
{
    std::string_view longName;
    char shortName = '\0';
    bool takesValue = true;
    std::string_view valueName{};
    std::string_view help{};
};

/// The options that were given, each under its long name, and for flags an empty value.
class ParsedArguments final
{
public:
    [[nodiscard]] auto Has(std::string_view longName) const noexcept -> bool;

    /// The value given for `longName`, or nothing when the option is absent.
    [[nodiscard]] auto Value(std::string_view longName) const noexcept -> std::optional<std::string_view>;

    /// Adds or replaces the value of `longName`.
    auto Set(std::string_view longName, std::string_view value) -> void;

    [[nodiscard]] auto Entries() const noexcept -> std::span<const std::pair<std::string_view, std::string_view>>
    {
        return entries_;
    }

private:
    std::vector<std::pair<std::string_view, std::string_view>> entries_;
};

/// What to do on reaching an argument that is not an option.
enum class PositionalArguments
{
    Stop,   ///< Stop there; ParseResult::rest holds that argument and everything after it.
    Reject,  ///< Fail with "unexpected argument".
    Collect, ///< Keep it in ParseResult::positionals and go on reading options.
};

struct ParseResult
{
    ParsedArguments options;
    std::span<const std::string_view> rest;
    std::vector<std::string_view> positionals;
};

/// A malformed command line. The CLI prints the message and exits 2, as clap does.
struct UsageError
{
    std::string message;
};

/// Reads `arguments` against `options`. `--` ends the options; what follows it is positional.
[[nodiscard]] auto ParseArguments(std::span<const std::string_view> arguments,
                                  std::span<const OptionSpecification> options, PositionalArguments positional)
    -> std::expected<ParseResult, UsageError>;

/// One line per option, `  -p, --policies <FILE>  help`, for a help text.
[[nodiscard]] auto DescribeOptions(std::span<const OptionSpecification> options) -> std::string;

} // namespace cedar::cli
