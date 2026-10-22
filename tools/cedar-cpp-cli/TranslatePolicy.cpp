#include "Commands.hpp"
#include "Inputs.hpp"

#include <cedar/Json.hpp>
#include <cedar/Syntax.hpp>

#include <array>
#include <cstdio>
#include <format>
#include <optional>
#include <string>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification DirectionOption{
    .longName = "direction", .valueName = "DIRECTION",
    .help = "The direction of translation [possible values: cedar-to-json, json-to-cedar]"};
constexpr OptionSpecification TranslatePoliciesOption{
    .longName = "policies", .shortName = 'p', .valueName = "FILE",
    .help = "Filename to read the policies from. If not provided, will default to reading stdin"};

constexpr std::array Options{DirectionOption, TranslatePoliciesOption};

/// `translate_policy` (`cedar-policy-cli/src/command/translate_policy.rs`): Cedar text to the JSON
/// policy format (the text read as `check-parse` reads it, `@id` renaming included), or the JSON
/// policy format to Cedar text (read as `--policy-format json` reads it), printed with a newline.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    const auto direction = arguments.Value(DirectionOption.longName);
    if (!direction)
    {
        return std::unexpected{UsageError{"the following required arguments were not provided:\n  --direction <DIRECTION>"}};
    }
    if (*direction != "cedar-to-json" && *direction != "json-to-cedar")
    {
        return std::unexpected{UsageError{std::format(
            "invalid value '{}' for '--direction <DIRECTION>' [possible values: cedar-to-json, json-to-cedar]",
            *direction)}};
    }
    const bool toJson = *direction == "cedar-to-json";
    const PolicyInput input{.policiesFile = arguments.Value(TranslatePoliciesOption.longName),
                            .format = toJson ? PolicyFormat::Cedar : PolicyFormat::Json,
                            .templateLinkedFile = std::nullopt};
    auto policies = LoadPolicySet(input);
    if (!policies)
    {
        Report(invocation.errorFormat, policies.error());
        return ExitCode::Failure;
    }
    std::string text = toJson ? ToJson(*policies) : ToCedar(*policies);
    text.push_back('\n');
    std::fwrite(text.data(), 1, text.size(), stdout);
    return ExitCode::Success;
}

} // namespace

const Command TranslatePolicyCommand{
    .name = "translate-policy",
    .summary = "Translate Cedar policy syntax to the JSON policy format, or back",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
