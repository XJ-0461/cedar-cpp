#pragma once

#include <cedar-cpp-cli/Command.hpp>

#include <json/JsonReader.hpp>
#include <json/ValueJson.hpp>

#include <cedar/Policy.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cedar::cli
{

// The CLI's own JSON inputs (`cedar-policy-cli/src/utils/{request,links}.rs`, `command/run_test.rs`),
// read over the library's JSON syntax tree with serde's rules for each.

/// How serde reached a JSON node, which decides how a repeated key reads.
enum class JsonOrigin
{
    /// Streamed from the file into a derived struct: a known field given twice fails, an unknown
    /// field is skipped after a syntax check only.
    Stream,
    /// Read from a `serde_json::Value`: the whole document passed the strict checks, and the last
    /// value of a repeated key is the only one there is.
    Value,
};

/// Appends `node` as JSON text. With JsonOrigin::Value each object keeps only the last member of
/// each key, as a `serde_json::Value` does; with Stream every member is written. `node` must hold
/// strictly decodable strings (CheckStrictJson).
void AppendJson(std::string& output, const detail::JsonNode& node, JsonOrigin origin);

/// The fields of a derived struct read from `node`, an object or an array (serde's sequence form,
/// fields in declaration order). A struct field given twice fails; with JsonOrigin::Value a
/// repeated key has already become its last value. Unknown members are skipped.
template <std::size_t Count>
[[nodiscard]] auto ReadStructFields(const detail::JsonNode& node, const std::array<std::string_view, Count>& names,
                                    const JsonOrigin origin, std::string_view structName)
    -> std::expected<std::array<const detail::JsonNode*, Count>, std::string>
{
    std::array<const detail::JsonNode*, Count> fields{};
    if (node.IsArray())
    {
        // serde reads the elements in order; a missing element fails, and so does an extra one.
        if (node.count != Count)
        {
            return std::unexpected{std::format("invalid length {}, expected struct {} with {} elements",
                                               node.count, structName, Count)};
        }
        for (std::size_t index = 0; index < Count; ++index) { fields[index] = &node.Elements()[index]; }
        return fields;
    }
    if (!node.IsObject())
    {
        return std::unexpected{std::format("invalid type: {}, expected struct {}", detail::DescribeJson(node), structName)};
    }
    std::vector<const detail::JsonMember*> members;
    if (origin == JsonOrigin::Value) { detail::EffectiveMembers(node, detail::DuplicateKeys::LastWins, members); }
    else
    {
        for (const detail::JsonMember& member : node.Members()) { members.push_back(&member); }
    }
    for (const detail::JsonMember* member : members)
    {
        if (!member->strictKey) { return std::unexpected{std::string{"a key holds an unpaired surrogate"}}; }
        for (std::size_t index = 0; index < Count; ++index)
        {
            if (member->key != names[index]) { continue; }
            if (fields[index] != nullptr) { return std::unexpected{std::format("duplicate field `{}`", names[index])}; }
            fields[index] = &member->value;
        }
    }
    return fields;
}

/// How a request is read when a schema is given (Rust's `RequestArgs::get_request` with a schema).
struct RequestSchema
{
    /// The context is read by the type `schema` declares for the request's action.
    const Schema* schema = nullptr;
    /// The request is then checked against `schema` (Rust's `Request::new` with a schema).
    bool validate = false;
};

/// Reads `node` as the CLI's `RequestJSON` (`{"principal", "action", "resource", "context"}`, or the
/// same four as an array), then builds the request: each uid parsed from Cedar text, the context
/// read as a context, by `requestSchema` when it has a schema. `sourceName` names the input in
/// messages.
[[nodiscard]] auto ReadRequestJson(const detail::JsonNode& node, JsonOrigin origin, std::string_view sourceName,
                                   RequestSchema requestSchema = {}) -> std::expected<Request, Failure>;

/// The request of `principal`, `action`, `resource` and `context`, checked against the schema
/// when `requestSchema` asks for it.
[[nodiscard]] auto MakeRequest(EntityUid principal, EntityUid action, EntityUid resource, Context context,
                               RequestSchema requestSchema) -> std::expected<Request, Failure>;

/// Reads an entity uid as every uid option and field of the Rust CLI is read (the public
/// `EntityUid::from_str`, `from_normalized_str`): Cedar syntax that is exactly the uid's printed
/// form, so whitespace, comments and escapes Rust would not print are refused.
[[nodiscard]] auto ParseNormalizedEntityUid(std::string_view text, std::string_view sourceName)
    -> std::expected<EntityUid, ParseError>;

/// One entry of a template-linked file (`utils/links.rs` `TemplateLinked`). `arguments` keeps
/// each slot's uid text as written, for writing the file back.
struct TemplateLink
{
    std::string templateId;
    std::string linkId;
    SlotBindings bindings;
    std::array<std::optional<std::string>, 2> arguments{};
};

/// Reads the template-linked file `path` (`load_links_from_file`). With `allowMissing` a path that
/// does not exist holds no links (`load_links_from_file_or_empty`, which the `link` command uses).
[[nodiscard]] auto ReadTemplateLinksFile(std::string_view path, bool allowMissing)
    -> std::expected<std::vector<TemplateLink>, Failure>;

/// Writes `links` as Rust's `serde_json::to_writer` writes a `Vec<TemplateLinked>`: one compact
/// line-less array of `{"template_id","link_id","args"}`, `args` mapping `?principal` and
/// `?resource` to the uid texts.
[[nodiscard]] auto WriteTemplateLinks(std::span<const TemplateLink> links) -> std::string;

/// Reads a template-linked file's text: empty means no links, otherwise a JSON array of
/// `{"template_id", "link_id", "args": {"?principal": uid, "?resource": uid}}`.
[[nodiscard]] auto ReadTemplateLinks(std::string_view text, std::string_view path)
    -> std::expected<std::vector<TemplateLink>, Failure>;

} // namespace cedar::cli
