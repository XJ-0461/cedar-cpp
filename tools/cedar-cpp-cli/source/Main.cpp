#include <cedar-cpp-cli/Arguments.hpp>
#include <cedar-cpp-cli/Command.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// cedar-cpp-cli: the test CLI that takes the Rust `cedar` binary's arguments, so the parity tools
// can run both binaries on identical command lines. `-f`/`--error-format` and `-h`/`--help`
// are accepted before or after the command, as clap's global options are; `-V`/`--version` only
// before it.

namespace
{

using namespace cedar::cli;

constexpr OptionSpecification ErrorFormatOption{
    .longName = "error-format", .shortName = 'f', .valueName = "ERR_FMT",
    .help = "The output format to use for error reporting [env: CEDAR_ERROR_FORMAT] [default: human] "
            "[possible values: human, plain, json]"};
constexpr OptionSpecification HelpOption{.longName = "help", .shortName = 'h', .takesValue = false, .help = "Print help"};
constexpr OptionSpecification VersionOption{
    .longName = "version", .shortName = 'V', .takesValue = false, .help = "Print version"};
constexpr std::array GlobalOptions{ErrorFormatOption, HelpOption, VersionOption};
// The global options a command accepts after its name; clap takes `--version` only before it.
constexpr std::array CommandGlobalOptions{ErrorFormatOption, HelpOption};

auto Print(std::FILE* stream, const std::string_view text) -> void { std::fwrite(text.data(), 1, text.size(), stream); }

auto MainUsage() -> std::string
{
    std::string text = "usage: cedar-cpp-cli [OPTIONS] <COMMAND>\n\nCommands:\n";
    for (const Command* command : Commands()) { text += std::format("  {:<20} {}\n", command->name, command->summary); }
    text += "\nOptions:\n" + DescribeOptions(GlobalOptions);
    return text;
}

auto CommandUsage(const Command& command, const std::span<const OptionSpecification> options) -> std::string
{
    const std::string positional =
        command.positionalName.empty() ? std::string{} : std::format(" <{}>", command.positionalName);
    return std::format("{}\n\nusage: cedar-cpp-cli {} [OPTIONS]{}\n\nOptions:\n{}", command.summary, command.name,
                       positional, DescribeOptions(options));
}

auto UsageFailure(const std::string_view message) -> int
{
    Print(stderr, std::format("error: {}\n\nFor more information, try '--help'.\n", message));
    return static_cast<int>(ExitCode::Usage);
}

auto ReadErrorFormat(const std::optional<std::string_view> given) -> std::expected<ErrorFormat, UsageError>
{
    std::optional<std::string_view> value = given;
    if (!value)
    {
        if (const char* environment = std::getenv("CEDAR_ERROR_FORMAT")) { value = environment; }
    }
    if (!value || *value == "human") { return ErrorFormat::Human; }
    if (*value == "plain") { return ErrorFormat::Plain; }
    if (*value == "json") { return ErrorFormat::Json; }
    return std::unexpected{UsageError{std::format(
        "invalid value '{}' for '--error-format <ERR_FMT>' [possible values: human, plain, json]", *value)}};
}

} // namespace

auto main(const int argc, char** argv) -> int
{
    const std::vector<std::string_view> arguments(argv + 1, argv + argc);

    auto leading = ParseArguments(arguments, GlobalOptions, PositionalArguments::Stop);
    if (!leading) { return UsageFailure(leading.error().message); }
    if (leading->options.Has(HelpOption.longName))
    {
        Print(stdout, MainUsage());
        return static_cast<int>(ExitCode::Success);
    }
    if (leading->options.Has(VersionOption.longName))
    {
        Print(stdout, "cedar-cpp-cli " CEDAR_CPP_VERSION "\n");
        return static_cast<int>(ExitCode::Success);
    }
    if (leading->rest.empty())
    {
        Print(stderr, MainUsage());
        return static_cast<int>(ExitCode::Usage);
    }

    const std::string_view name = leading->rest.front();
    const auto commands = Commands();
    const auto found = std::ranges::find(commands, name, &Command::name);
    if (found == commands.end()) { return UsageFailure(std::format("unrecognized subcommand '{}'", name)); }
    const Command& command = **found;

    std::vector<OptionSpecification> options(command.options.begin(), command.options.end());
    options.insert(options.end(), CommandGlobalOptions.begin(), CommandGlobalOptions.end());
    const bool takesPositional = !command.positionalName.empty();
    auto parsed = ParseArguments(leading->rest.subspan(1), options,
                                 takesPositional ? PositionalArguments::Collect : PositionalArguments::Reject);
    if (!parsed) { return UsageFailure(parsed.error().message); }
    if (parsed->positionals.size() > 1)
    {
        return UsageFailure(std::format("unexpected argument '{}' found", parsed->positionals[1]));
    }
    if (parsed->options.Has(HelpOption.longName))
    {
        Print(stdout, CommandUsage(command, options));
        return static_cast<int>(ExitCode::Success);
    }
    std::optional<std::string_view> positional;
    if (takesPositional)
    {
        if (parsed->positionals.empty())
        {
            return UsageFailure(std::format("the following required arguments were not provided:\n  <{}>",
                                            command.positionalName));
        }
        positional = parsed->positionals.front();
    }

    // A global option given both before and after the command takes the later value.
    ParsedArguments& merged = parsed->options;
    if (!merged.Has(ErrorFormatOption.longName))
    {
        if (const auto early = leading->options.Value(ErrorFormatOption.longName))
        {
            merged.Set(ErrorFormatOption.longName, *early);
        }
    }
    const auto errorFormat = ReadErrorFormat(merged.Value(ErrorFormatOption.longName));
    if (!errorFormat) { return UsageFailure(errorFormat.error().message); }

    const auto result = command.run(Invocation{.arguments = merged, .positional = positional, .errorFormat = *errorFormat});
    if (!result) { return UsageFailure(result.error().message); }
    std::fflush(stdout);
    return static_cast<int>(*result);
}
