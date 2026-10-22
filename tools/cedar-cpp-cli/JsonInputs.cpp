#include "JsonInputs.hpp"

#include "Inputs.hpp"

#include <json/ValueJson.hpp>

#include <cedar/Json.hpp>
#include <cedar/Syntax.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <memory_resource>
#include <optional>
#include <utility>

namespace cedar::cli
{

void AppendJson(std::string& output, const detail::JsonNode& node, const JsonOrigin origin)
{
    switch (node.kind)
    {
    case detail::JsonKind::Null: output += "null"; return;
    case detail::JsonKind::False: output += "false"; return;
    case detail::JsonKind::True: output += "true"; return;
    case detail::JsonKind::Number: output += node.text; return;
    case detail::JsonKind::String: detail::AppendJsonString(output, node.text); return;
    case detail::JsonKind::Array:
    {
        output += '[';
        bool first = true;
        for (const detail::JsonNode& element : node.Elements())
        {
            if (!first) { output += ','; }
            first = false;
            AppendJson(output, element, origin);
        }
        output += ']';
        return;
    }
    case detail::JsonKind::Object:
    {
        std::vector<const detail::JsonMember*> members;
        if (origin == JsonOrigin::Value) { detail::EffectiveMembers(node, detail::DuplicateKeys::LastWins, members); }
        else
        {
            for (const detail::JsonMember& member : node.Members()) { members.push_back(&member); }
        }
        output += '{';
        bool first = true;
        for (const detail::JsonMember* member : members)
        {
            if (!first) { output += ','; }
            first = false;
            detail::AppendJsonString(output, member->key);
            output += ':';
            AppendJson(output, member->value, origin);
        }
        output += '}';
        return;
    }
    }
}

namespace
{

auto Fail(const std::string_view sourceName, std::string message) -> std::unexpected<Failure>
{
    return std::unexpected{Failure{std::format("failed to parse request JSON {}: {}", sourceName, std::move(message))}};
}

/// A field that must be a string, as serde reads a `String`.
auto ReadString(const detail::JsonNode& node, const std::string_view field) -> std::expected<std::string_view, std::string>
{
    if (!node.IsString()) { return std::unexpected{std::format("`{}` must be a string", field)}; }
    if (!node.strictString) { return std::unexpected{std::format("`{}` holds an unpaired surrogate", field)}; }
    return node.text;
}

} // namespace

auto MakeRequest(EntityUid principal, EntityUid action, EntityUid resource, Context context,
                 const RequestSchema requestSchema) -> std::expected<Request, Failure>
{
    if (requestSchema.schema == nullptr || !requestSchema.validate)
    {
        return Request{std::move(principal), std::move(action), std::move(resource), std::move(context)};
    }
    auto request = Request::Create(std::move(principal), std::move(action), std::move(resource), std::move(context),
                                   *requestSchema.schema);
    if (!request)
    {
        return std::unexpected{Failure{"the request is not valid for the schema", std::move(request).error().TakeDiagnostics()}};
    }
    return *std::move(request);
}

auto ReadRequestJson(const detail::JsonNode& node, const JsonOrigin origin, const std::string_view sourceName,
                     const RequestSchema requestSchema) -> std::expected<Request, Failure>
{
    static constexpr std::array<std::string_view, 4> Names{"principal", "action", "resource", "context"};
    auto fields = ReadStructFields(node, Names, origin, "RequestJSON");
    if (!fields) { return Fail(sourceName, std::move(fields).error()); }

    // `principal`, `action` and `resource` are `Option<String>` with a default: absent or null
    // means none. `context` is a required `serde_json::Value`.
    std::array<std::optional<std::string_view>, 3> uids{};
    for (std::size_t index = 0; index < uids.size(); ++index)
    {
        const detail::JsonNode* field = (*fields)[index];
        if (field == nullptr || field->kind == detail::JsonKind::Null) { continue; }
        auto text = ReadString(*field, Names[index]);
        if (!text) { return Fail(sourceName, std::move(text).error()); }
        uids[index] = *text;
    }
    const detail::JsonNode* context = (*fields)[3];
    if (context == nullptr) { return Fail(sourceName, "missing field `context`"); }
    if (auto checked = detail::CheckStrictJson(*context, detail::DuplicateKeys::LastWins); !checked)
    {
        return Fail(sourceName, std::move(checked).error().message);
    }

    if (!uids[0] || !uids[1] || !uids[2])
    {
        return std::unexpected{Failure{"All three (`principal`, `action`, `resource`) variables must be specified"}};
    }
    std::array<std::optional<EntityUid>, 3> parsed{};
    for (std::size_t index = 0; index < uids.size(); ++index)
    {
        auto uid = ParseNormalizedEntityUid(*uids[index], Names[index]);
        if (!uid)
        {
            return std::unexpected{Failure{std::format("failed to parse {} in {} as entity Uid", Names[index], sourceName),
                                           std::move(uid).error().TakeDiagnostics()}};
        }
        parsed[index] = *std::move(uid);
    }

    std::string contextText;
    AppendJson(contextText, *context, JsonOrigin::Value);
    auto loaded = requestSchema.schema != nullptr
                      ? ContextFromJson(contextText, *requestSchema.schema, *parsed[1], sourceName)
                      : ContextFromJson(contextText, sourceName);
    if (!loaded)
    {
        return std::unexpected{Failure{std::format("failed to create a context from {}", sourceName),
                                       std::move(loaded).error().TakeDiagnostics()}};
    }
    return MakeRequest(*std::move(parsed[0]), *std::move(parsed[1]), *std::move(parsed[2]), *std::move(loaded),
                       requestSchema);
}

auto ParseNormalizedEntityUid(const std::string_view text, const std::string_view sourceName)
    -> std::expected<EntityUid, ParseError>
{
    auto uid = ParseEntityUid(text, sourceName);
    if (!uid) { return uid; }
    // `default_from_normalized_str`: the text must be what the parsed uid prints as.
    std::string normalized = uid->ToString();
    if (normalized == text) { return uid; }
    return std::unexpected{ParseError{Diagnostic{
        .code = DiagnosticCode::NonNormalizedString,
        .message = std::format("`Entity UID` needs to be normalized (e.g., whitespace removed): {}", text),
        .help = std::format("the normalized form is `{}`", normalized),
        .source = MakeSourceText(std::string{sourceName}, std::string{text})}}};
}

auto ReadTemplateLinksFile(const std::string_view path, const bool allowMissing)
    -> std::expected<std::vector<TemplateLink>, Failure>
{
    // Rust's `Path::exists`: false also when the path cannot be inspected.
    std::error_code error;
    if (allowMissing && !std::filesystem::exists(std::filesystem::path{path}, error)) { return std::vector<TemplateLink>{}; }
    auto text = ReadInput(path, "links");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    return ReadTemplateLinks(*text, path);
}

auto WriteTemplateLinks(const std::span<const TemplateLink> links) -> std::string
{
    std::string output = "[";
    for (const TemplateLink& link : links)
    {
        if (output.size() > 1) { output += ','; }
        output += "{\"template_id\":";
        detail::AppendJsonString(output, link.templateId);
        output += ",\"link_id\":";
        detail::AppendJsonString(output, link.linkId);
        output += ",\"args\":{";
        bool first = true;
        for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
        {
            const std::optional<std::string>& argument = link.arguments[static_cast<std::size_t>(slot)];
            if (!argument) { continue; }
            if (!first) { output += ','; }
            first = false;
            detail::AppendJsonString(output, ToString(slot));
            output += ':';
            detail::AppendJsonString(output, *argument);
        }
        output += "}}";
    }
    output += ']';
    return output;
}

auto ReadTemplateLinks(const std::string_view text, const std::string_view path)
    -> std::expected<std::vector<TemplateLink>, Failure>
{
    std::vector<TemplateLink> links;
    // `load_links_from_file`: an empty file holds no links.
    if (text.empty()) { return links; }
    const auto fail = [&](std::string message) {
        return std::unexpected{Failure{std::format("failed to read links file '{}': {}", path, std::move(message))}};
    };

    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(text, scratch);
    if (!document) { return fail(std::move(document).error().message); }
    const detail::JsonNode& root = document->Root();
    if (!root.IsArray()) { return fail(std::format("invalid type: {}, expected a sequence", detail::DescribeJson(root))); }

    static constexpr std::array<std::string_view, 3> Names{"template_id", "link_id", "args"};
    for (const detail::JsonNode& entry : root.Elements())
    {
        auto fields = ReadStructFields(entry, Names, JsonOrigin::Stream, "LiteralTemplateLinked");
        if (!fields) { return fail(std::move(fields).error()); }
        for (std::size_t index = 0; index < Names.size(); ++index)
        {
            if ((*fields)[index] == nullptr) { return fail(std::format("missing field `{}`", Names[index])); }
        }
        auto templateId = ReadString(*(*fields)[0], Names[0]);
        if (!templateId) { return fail(std::move(templateId).error()); }
        auto linkId = ReadString(*(*fields)[1], Names[1]);
        if (!linkId) { return fail(std::move(linkId).error()); }

        // `args` is a `HashMap<String, String>`: a repeated key keeps its last value. Each key must
        // name a slot; each value is parsed as a uid once the map is built.
        const detail::JsonNode& arguments = *(*fields)[2];
        if (!arguments.IsObject()) { return fail("`args` must be a map from slots to entity uids"); }
        std::array<std::optional<std::string_view>, 2> slots{};
        for (const detail::JsonMember& member : arguments.Members())
        {
            if (!member.strictKey) { return fail("a slot name holds an unpaired surrogate"); }
            auto value = ReadString(member.value, "args");
            if (!value) { return fail(std::move(value).error()); }
            if (member.key == "?principal") { slots[0] = *value; }
            else if (member.key == "?resource") { slots[1] = *value; }
            else { return fail(std::format("Invalid SlotId! Expected ?principal|?resource, got: {}", member.key)); }
        }
        TemplateLink link{.templateId = std::string{*templateId}, .linkId = std::string{*linkId}, .bindings = {}};
        for (std::size_t index = 0; index < slots.size(); ++index)
        {
            if (!slots[index]) { continue; }
            auto uid = ParseNormalizedEntityUid(*slots[index], path);
            if (!uid)
            {
                return std::unexpected{Failure{std::format("failed to parse a slot argument in links file '{}'", path),
                                               std::move(uid).error().TakeDiagnostics()}};
            }
            link.bindings.Bind(index == 0 ? SlotId::Principal : SlotId::Resource, *std::move(uid));
            link.arguments[index] = std::string{*slots[index]};
        }
        links.push_back(std::move(link));
    }
    return links;
}

} // namespace cedar::cli
