#include <cedar-cpp-cli/Commands.hpp>
#include <cedar-cpp-cli/Inputs.hpp>
#include <cedar-cpp-cli/JsonInputs.hpp>

#include <json/JsonReader.hpp>

#include <cedar/Syntax.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification LinkPoliciesOption{
    .longName = "policies", .shortName = 'p', .valueName = "FILE",
    .help = "File containing the static Cedar policies and/or templates. If not provided, read policies from stdin"};
constexpr OptionSpecification TemplateIdOption{
    .longName = "template-id", .valueName = "TEMPLATE_ID", .help = "Id of the template to link"};
constexpr OptionSpecification NewIdOption{
    .longName = "new-id", .shortName = 'n', .valueName = "NEW_ID", .help = "Id for the new template linked policy"};
constexpr OptionSpecification ArgumentsOption{
    .longName = "arguments", .shortName = 'a', .valueName = "ARGUMENTS", .help = "Arguments to fill slots"};

constexpr std::array Options{LinkPoliciesOption, PolicyFormatOption, TemplateLinkedOption,
                             TemplateIdOption,   NewIdOption,        ArgumentsOption};

/// `--arguments`, read as clap reads it (`command/link.rs` `Arguments::from_str`): a JSON object
/// read as a `HashMap<String, String>` (a repeated key keeps its last value), each key `?principal`
/// or `?resource`. The uid texts are parsed later.
auto ReadArguments(const std::string_view text) -> std::expected<std::array<std::optional<std::string>, 2>, UsageError>
{
    const auto fail = [&](const std::string_view why) {
        return std::unexpected{UsageError{std::format("invalid value '{}' for '--arguments <ARGUMENTS>': {}", text, why)}};
    };
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(text, scratch);
    if (!document) { return fail(document.error().message); }
    const detail::JsonNode& root = document->Root();
    if (!root.IsObject()) { return fail(std::format("invalid type: {}, expected a map", detail::DescribeJson(root))); }
    std::array<std::optional<std::string>, 2> arguments{};
    std::vector<std::string_view> unknownKeys;
    for (const detail::JsonMember& member : root.Members())
    {
        if (!member.strictKey) { return fail("a key holds an unpaired surrogate"); }
        if (!member.value.IsString()) { return fail(std::format("invalid type: {}, expected a string", detail::DescribeJson(member.value))); }
        if (!member.value.strictString) { return fail("a value holds an unpaired surrogate"); }
        if (member.key == "?principal") { arguments[0] = std::string{member.value.text}; }
        else if (member.key == "?resource") { arguments[1] = std::string{member.value.text}; }
        else { unknownKeys.push_back(member.key); }
    }
    if (!unknownKeys.empty())
    {
        return fail(std::format("Invalid SlotId! Expected ?principal|?resource, got: {}", unknownKeys.front()));
    }
    return arguments;
}

auto Fail(const Invocation& invocation, const Failure& failure) -> ExitCode
{
    Report(invocation.errorFormat, failure);
    return ExitCode::Failure;
}

struct FileCloser
{
    auto operator()(std::FILE* file) const noexcept -> void { std::fclose(file); }
};

/// Replaces the file `path` with `text`, creating it when it does not exist.
auto WriteFile(const std::string& path, const std::string_view text) -> std::expected<void, Failure>
{
    const std::unique_ptr<std::FILE, FileCloser> file{std::fopen(path.c_str(), "wb")};
    if (!file || std::fwrite(text.data(), 1, text.size(), file.get()) != text.size() || std::fflush(file.get()) != 0)
    {
        return std::unexpected{
            Failure{std::format("failed to open template link file {}: {}", path, std::strerror(errno))}};
    }
    return {};
}

/// `link` (`cedar-policy-cli/src/command/link.rs`): reads the policies and the template-linked
/// file (a missing file holds no links), links `--template-id` as `--new-id` with `--arguments`,
/// prints the new policy, and when `--template-linked` is given rewrites that file with the new
/// link appended.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    std::vector<std::string_view> missing;
    for (const OptionSpecification& required : {TemplateIdOption, NewIdOption, ArgumentsOption})
    {
        if (!arguments.Has(required.longName)) { missing.push_back(required.longName); }
    }
    auto slotTexts = arguments.Has(ArgumentsOption.longName) ? ReadArguments(*arguments.Value(ArgumentsOption.longName))
                                                             : std::array<std::optional<std::string>, 2>{};
    if (!slotTexts) { return std::unexpected{std::move(slotTexts).error()}; }
    if (!missing.empty())
    {
        std::string message = "the following required arguments were not provided:";
        for (const std::string_view name : missing)
        {
            const OptionSpecification& option = name == TemplateIdOption.longName ? TemplateIdOption
                                                : name == NewIdOption.longName    ? NewIdOption
                                                                                  : ArgumentsOption;
            message += std::format("\n  --{} <{}>", option.longName, option.valueName);
        }
        return std::unexpected{UsageError{std::move(message)}};
    }
    auto input = ReadPolicyInput(arguments);
    if (!input) { return std::unexpected{std::move(input).error()}; }
    input->allowMissingTemplateLinkedFile = true;

    auto policies = LoadPolicySet(*input);
    if (!policies) { return Fail(invocation, policies.error()); }

    // `create_slot_env`.
    SlotBindings bindings;
    for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
    {
        const std::optional<std::string>& text = (*slotTexts)[static_cast<std::size_t>(slot)];
        if (!text) { continue; }
        auto uid = ParseNormalizedEntityUid(*text, "--arguments");
        if (!uid) { return Fail(invocation, Failure{"failed to parse a slot argument", std::move(uid).error().TakeDiagnostics()}); }
        bindings.Bind(slot, *std::move(uid));
    }

    const std::string_view templateId = *arguments.Value(TemplateIdOption.longName);
    const std::string_view newId = *arguments.Value(NewIdOption.longName);
    TemplateLink added{.templateId = std::string{templateId}, .linkId = std::string{newId}, .bindings = bindings,
                       .arguments = *slotTexts};
    if (auto linked = policies->Link(templateId, PolicyId{newId}, std::move(bindings)); !linked)
    {
        return Fail(invocation, Failure{"unable to link template", std::move(linked).error().TakeDiagnostics()});
    }
    const std::string printed = std::format("Template-linked policy added: {}\n", ToCedar(*policies->GetPolicy(newId)));
    std::fwrite(printed.data(), 1, printed.size(), stdout);

    if (const auto linksFile = input->templateLinkedFile)
    {
        auto links = ReadTemplateLinksFile(*linksFile, true);
        if (!links) { return Fail(invocation, links.error()); }
        links->push_back(std::move(added));
        if (auto written = WriteFile(std::string{*linksFile}, WriteTemplateLinks(*links)); !written)
        {
            return Fail(invocation, written.error());
        }
    }
    return ExitCode::Success;
}

} // namespace

const Command LinkCommand{
    .name = "link",
    .summary = "Link a template",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
