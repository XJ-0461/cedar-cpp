#include "Inputs.hpp"

#include "JsonInputs.hpp"

#include <json/JsonReader.hpp>
#include <json/PolicyJson.hpp>
#include <json/ValueJson.hpp>

#include <syntax/Utf8.hpp>

#include <cedar/Json.hpp>
#include <cedar/Syntax.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <memory_resource>

namespace cedar::cli
{

namespace
{

auto IsUtf8(const std::string_view text) -> bool
{
    std::size_t offset = 0;
    while (offset < text.size())
    {
        const detail::DecodedScalar decoded = detail::DecodeUtf8(text, offset);
        if (decoded.length == 0) { return false; }
        offset += decoded.length;
    }
    return true;
}

struct FileCloser
{
    auto operator()(std::FILE* file) const noexcept -> void { std::fclose(file); }
};

auto ReadStream(std::FILE* stream, std::string& text) -> bool
{
    char buffer[65536];
    while (true)
    {
        const std::size_t count = std::fread(buffer, 1, sizeof buffer, stream);
        text.append(buffer, count);
        if (count < sizeof buffer) { return std::ferror(stream) == 0; }
    }
}

} // namespace

auto ReadInput(const std::optional<std::string_view> path, const std::string_view context)
    -> std::expected<std::string, Failure>
{
    std::string text;
    const std::string where = path ? std::format("failed to open {} file {}", context, *path)
                                   : std::format("failed to read {} from stdin", context);
    if (path)
    {
        const std::string name{*path};
        const std::unique_ptr<std::FILE, FileCloser> file{std::fopen(name.c_str(), "rb")};
        if (!file || !ReadStream(file.get(), text))
        {
            return std::unexpected{Failure{std::format("{}: {}", where, std::strerror(errno))}};
        }
    }
    else if (!ReadStream(stdin, text))
    {
        return std::unexpected{Failure{std::format("{}: {}", where, std::strerror(errno))}};
    }
    if (!IsUtf8(text)) { return std::unexpected{Failure{std::format("{}: the input is not valid UTF-8", where)}}; }
    return text;
}

auto ReadPolicyInput(const ParsedArguments& arguments) -> std::expected<PolicyInput, UsageError>
{
    PolicyInput input{.policiesFile = arguments.Value(PoliciesOption.longName),
                      .templateLinkedFile = arguments.Value(TemplateLinkedOption.longName)};
    if (const auto format = arguments.Value(PolicyFormatOption.longName))
    {
        if (*format == "cedar") { input.format = PolicyFormat::Cedar; }
        else if (*format == "json") { input.format = PolicyFormat::Json; }
        else
        {
            return std::unexpected{UsageError{std::format(
                "invalid value '{}' for '--policy-format <POLICY_FORMAT>' [possible values: cedar, json]", *format)}};
        }
    }
    return input;
}

namespace
{

/// `rename_from_id_annotation`: a fresh set holding every template, then every policy, each under
/// its `@id` value when it has one. The parsed set holds only static policies, so each policy is
/// rebuilt from its renamed body.
auto RenameFromIdAnnotation(const PolicySet& parsed) -> std::expected<PolicySet, Failure>
{
    PolicySet renamed;
    for (const std::shared_ptr<const Template>& body : parsed.Templates())
    {
        const auto id = body->GetAnnotations().Find("id");
        auto added = renamed.AddTemplate(id ? body->WithId(PolicyId{*id}) : body);
        if (!added)
        {
            return std::unexpected{Failure{"failed to add template to policy set",
                                           std::move(added).error().TakeDiagnostics()}};
        }
    }
    for (const std::shared_ptr<const Policy>& policy : parsed.Policies())
    {
        const Template& body = policy->GetTemplate();
        const auto id = body.GetAnnotations().Find("id");
        auto rebuilt = Policy::Static(id ? body.WithId(PolicyId{*id}) : policy->SharedTemplate());
        if (!rebuilt)
        {
            return std::unexpected{Failure{"failed to add policy to policy set",
                                           std::move(rebuilt).error().TakeDiagnostics()}};
        }
        if (auto added = renamed.Add(std::move(*rebuilt)); !added)
        {
            return std::unexpected{Failure{"failed to add policy to policy set",
                                           std::move(added).error().TakeDiagnostics()}};
        }
    }
    return renamed;
}

} // namespace

namespace
{

/// `read_cedar_policy_set`: the parsed set, renamed by `@id`.
auto LoadCedarPolicySet(const PolicyInput& input) -> std::expected<PolicySet, Failure>
{
    auto text = ReadInput(input.policiesFile, "policy set");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto parsed = ParsePolicySet(*text, input.policiesFile.value_or("<stdin>"));
    if (!parsed)
    {
        return std::unexpected{Failure{"failed to parse policy set", std::move(parsed).error().TakeDiagnostics()}};
    }
    return RenameFromIdAnnotation(*parsed);
}

/// `read_json_policy_set`: the file read as a `serde_json::Value`, then as a policy set when it has
/// a policy set's fields, or else as one policy (static without slots, a template with them) named
/// `JSON policy`. No `@id` renaming.
auto LoadJsonPolicySet(const PolicyInput& input) -> std::expected<PolicySet, Failure>
{
    auto text = ReadInput(input.policiesFile, "JSON policy");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    const std::string_view sourceName = input.policiesFile.value_or("<stdin>");

    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(*text, scratch);
    if (!document) { return std::unexpected{Failure{"failed to parse JSON policy: " + document.error().message}}; }
    const detail::JsonNode& root = document->Root();
    if (auto checked = detail::CheckStrictJson(root, detail::DuplicateKeys::LastWins); !checked)
    {
        return std::unexpected{Failure{"failed to parse JSON policy: " + checked.error().message}};
    }
    // `get_json_policy_type`: which fields the document has, whatever their values.
    constexpr std::array<std::string_view, 3> PolicySetFields{"staticPolicies", "templates", "templateLinks"};
    constexpr std::array<std::string_view, 5> PolicyFields{"action", "effect", "principal", "resource", "conditions"};
    const auto hasAny = [&](const auto& names) {
        if (!root.IsObject()) { return false; }
        for (const detail::JsonMember& member : root.Members())
        {
            if (std::ranges::find(names, member.key) != names.end()) { return true; }
        }
        return false;
    };
    const bool isPolicySet = hasAny(PolicySetFields);
    const bool isPolicy = hasAny(PolicyFields);
    if (isPolicySet == isPolicy)
    {
        return std::unexpected{Failure{std::format(
            "cannot determine if json policy is a single policy or a policy set. Found {}",
            isPolicy ? "matching properties from both formats" : "no matching properties from either format")}};
    }
    if (isPolicySet)
    {
        auto set = detail::ReadPolicySetJson(*text, detail::DuplicateKeys::LastWins, sourceName);
        if (!set)
        {
            return std::unexpected{
                Failure{"failed to create policy set from JSON policy", std::move(set).error().TakeDiagnostics()}};
        }
        return *std::move(set);
    }
    auto body = detail::ReadPolicyOrTemplateJson(*text, detail::DuplicateKeys::LastWins, PolicyId{"JSON policy"},
                                                 sourceName);
    if (!body)
    {
        return std::unexpected{Failure{"failed to parse JSON policy", std::move(body).error().TakeDiagnostics()}};
    }
    PolicySet set;
    if ((*body)->IsStatic())
    {
        // A body without slots always makes a static policy.
        auto added = set.Add(*Policy::Static(*std::move(body)));
        if (!added)
        {
            return std::unexpected{Failure{"failed to create policy set from JSON policy",
                                           std::move(added).error().TakeDiagnostics()}};
        }
    }
    else if (auto added = set.AddTemplate(*std::move(body)); !added)
    {
        return std::unexpected{
            Failure{"failed to add template to policy set", std::move(added).error().TakeDiagnostics()}};
    }
    return set;
}

} // namespace

auto LoadPolicySet(const PolicyInput& input) -> std::expected<PolicySet, Failure>
{
    auto loaded = input.format == PolicyFormat::Json ? LoadJsonPolicySet(input) : LoadCedarPolicySet(input);
    if (!loaded || !input.templateLinkedFile) { return loaded; }

    auto links = ReadTemplateLinksFile(*input.templateLinkedFile, input.allowMissingTemplateLinkedFile);
    if (!links) { return std::unexpected{std::move(links).error()}; }
    for (TemplateLink& link : *links)
    {
        if (auto linked = loaded->Link(link.templateId, PolicyId{link.linkId}, std::move(link.bindings)); !linked)
        {
            return std::unexpected{
                Failure{"unable to link template", std::move(linked).error().TakeDiagnostics()}};
        }
    }
    return loaded;
}

auto AuthorizerOptionsFromEnvironment() -> std::expected<AuthorizerOptions, UsageError>
{
    const char* const value = std::getenv("CEDAR_CPP_POLICY_SELECTION");
    if (value == nullptr || std::string_view{value} == "every")
    {
        return AuthorizerOptions{.policySelection = PolicySelection::EvaluateEveryPolicy};
    }
    if (std::string_view{value} == "index") { return AuthorizerOptions{.policySelection = PolicySelection::ScopeIndex}; }
    return std::unexpected{
        UsageError{std::format("CEDAR_CPP_POLICY_SELECTION takes every or index, not `{}`", value)}};
}

auto CheckSchemaFormat(const ParsedArguments& arguments) -> std::expected<void, UsageError>
{
    if (const auto format = arguments.Value(SchemaFormatOption.longName); format && *format != "cedar" && *format != "json")
    {
        return std::unexpected{UsageError{std::format(
            "invalid value '{}' for '--schema-format <SCHEMA_FORMAT>' [possible values: cedar, json]", *format)}};
    }
    return {};
}

auto LoadEntities(const std::string_view path) -> std::expected<Entities, Failure>
{
    auto text = ReadInput(path, "entities");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto parsed = EntitiesFromJson(*text, path);
    if (!parsed)
    {
        return std::unexpected{Failure{std::format("failed to parse entities from file {}", path),
                                       std::move(parsed).error().TakeDiagnostics()}};
    }
    return *std::move(parsed);
}

} // namespace cedar::cli
