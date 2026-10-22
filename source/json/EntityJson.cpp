// Entities and entity uids from the entity JSON format, with or without a schema (Rust's
// `EntityJsonParser`, `cedar-policy-core/src/entities/json/entities.rs`, and
// `EntityUid::from_json`, `cedar-policy/src/api/id.rs`).

#include "entities/Conformance.hpp"
#include "json/JsonReader.hpp"
#include "json/ValueJson.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Json.hpp>

#include <array>
#include <format>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

using detail::DuplicateKeys;
using detail::EntityTypeNames;
using detail::JsonKind;
using detail::JsonMember;
using detail::JsonNode;
using detail::JsonReadError;

[[nodiscard]] auto ShapeError(const JsonNode& node, std::string message) -> JsonReadError
{
    return JsonReadError{.code = DiagnosticCode::JsonUnexpectedShape,
                         .offset = node.offset,
                         .length = node.length,
                         .message = std::move(message)};
}

/// One entity's fields as Rust's `EntityJson` reads them from a stream.
struct EntityFields
{
    const JsonNode* uid = nullptr;
    const JsonNode* attributes = nullptr;
    const JsonNode* parents = nullptr;
    const JsonNode* tags = nullptr;
};

/// Splits an entity into its fields. serde reads the derived struct from an object (fields
/// `uid`, `attrs`, `parents` required, `tags` optional, any other key skipped unread, a field
/// given twice refused) or from a sequence of three or four elements in field order.
[[nodiscard]] auto SplitEntity(const JsonNode& node) -> std::expected<EntityFields, JsonReadError>
{
    EntityFields fields;
    if (node.IsArray())
    {
        const auto elements = node.Elements();
        if (elements.size() != 3 && elements.size() != 4)
        {
            return std::unexpected{ShapeError(
                node, std::format("an entity written as an array has 3 or 4 elements (uid, attrs, parents, tags), "
                                  "not {}",
                                  elements.size()))};
        }
        fields.uid = &elements[0];
        fields.attributes = &elements[1];
        fields.parents = &elements[2];
        if (elements.size() == 4) { fields.tags = &elements[3]; }
        return fields;
    }
    if (!node.IsObject())
    {
        return std::unexpected{ShapeError(node, std::format("expected an entity object, but got `{}`",
                                                            detail::DescribeJson(node)))};
    }
    for (const JsonMember& member : node.Members())
    {
        if (!member.strictKey)
        {
            return std::unexpected{JsonReadError{.code = DiagnosticCode::JsonSyntax,
                                                 .offset = member.keyOffset,
                                                 .length = 1,
                                                 .message = "a `\\u` escape in the key is an unpaired surrogate"}};
        }
        const JsonNode** slot = nullptr;
        if (member.key == "uid") { slot = &fields.uid; }
        else if (member.key == "attrs") { slot = &fields.attributes; }
        else if (member.key == "parents") { slot = &fields.parents; }
        else if (member.key == "tags") { slot = &fields.tags; }
        else { continue; }
        if (*slot != nullptr)
        {
            return std::unexpected{JsonReadError{.code = DiagnosticCode::JsonUnexpectedShape,
                                                 .offset = member.keyOffset,
                                                 .length = static_cast<std::uint32_t>(member.key.size()),
                                                 .message = std::format("the field `{}` is given twice", member.key)}};
        }
        *slot = &member.value;
    }
    for (const auto& [field, name] : std::array{std::pair{fields.uid, "uid"}, std::pair{fields.attributes, "attrs"},
                                                 std::pair{fields.parents, "parents"}})
    {
        if (field == nullptr)
        {
            return std::unexpected{ShapeError(node, std::format("the entity has no `{}` field", name))};
        }
    }
    return fields;
}

/// Reads an entity uid field (`uid` or a parent), which serde buffers as `Content`.
[[nodiscard]] auto ReadUidField(const JsonNode& node, const EntityTypeNames names)
    -> std::expected<EntityUid, JsonReadError>
{
    if (auto checked = detail::CheckStrictJson(node, DuplicateKeys::Keep); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    return detail::ReadEntityUidJson(node, DuplicateKeys::Keep, names);
}

/// The schema's description of the entity being read: nothing without a schema and for an action
/// (`EntitySchemaInfo::NoSchema`), else its declaration (`NonAction`).
struct EntityDescription
{
    EntityTypeNames names = EntityTypeNames::Intern;
    const detail::ValidatorEntityType* declaration = nullptr;
};

/// The type the declaration gives the attribute or tag `key`, or an error for one it does not
/// declare (`EntitySchemaConformanceError::unexpected_entity_attr` / `_tag`).
[[nodiscard]] auto DeclaredType(const detail::ValidatorEntityType& declaration, const std::string_view field,
                                const JsonMember& member, const EntityUid& uid)
    -> std::expected<const detail::Type*, JsonReadError>
{
    if (field == "attrs")
    {
        if (const detail::AttributeType* attribute = declaration.attributes.Find(member.key))
        {
            return attribute->type.get();
        }
        if (declaration.openAttributes == detail::OpenTag::OpenAttributes) { return nullptr; }
        return std::unexpected{JsonReadError{
            .code = DiagnosticCode::UnexpectedEntityAttribute,
            .offset = member.keyOffset,
            .length = static_cast<std::uint32_t>(member.key.size() + 2),
            .message = std::format("`{}` has the attribute `{}`, which the schema does not declare", uid.ToString(),
                                   member.key)}};
    }
    if (declaration.tags) { return &*declaration.tags; }
    return std::unexpected{
        JsonReadError{.code = DiagnosticCode::UnexpectedEntityTag,
                      .offset = member.keyOffset,
                      .length = static_cast<std::uint32_t>(member.key.size() + 2),
                      .message = std::format("`{}` has the tag `{}`, but the schema declares no tags for its type",
                                             uid.ToString(), member.key)}};
}

/// Reads `attrs` or `tags`: a map with no key twice, at any depth. With a declaration each value
/// is read by its declared type.
[[nodiscard]] auto ReadValueMap(const JsonNode& node, const std::string_view field, const EntityUid& uid,
                                const EntityDescription& description, ValueArena& arena)
    -> std::expected<Record, JsonReadError>
{
    if (!node.IsObject())
    {
        return std::unexpected{
            ShapeError(node, std::format("`{}` must be an object, not `{}`", field, detail::DescribeJson(node)))};
    }
    if (auto checked = detail::CheckStrictJson(node, DuplicateKeys::Reject); !checked)
    {
        return std::unexpected{std::move(checked).error()};
    }
    const std::string_view kind = field == "attrs" ? "attribute" : "tag";
    const std::span<RecordEntry> entries = arena.AllocateRecordEntries(node.count);
    for (std::size_t index = 0; index < node.count; ++index)
    {
        const JsonMember& member = node.Members()[index];
        const detail::Type* expected = nullptr;
        if (description.declaration != nullptr)
        {
            auto declared = DeclaredType(*description.declaration, field, member, uid);
            if (!declared) { return std::unexpected{std::move(declared).error()}; }
            expected = *declared;
        }
        auto value = expected != nullptr ? detail::ReadTypedCedarValue(member.value, *expected, DuplicateKeys::Reject,
                                                                       arena, description.names)
                                         : detail::ReadCedarValue(member.value, DuplicateKeys::Reject, arena,
                                                                  description.names);
        if (!value)
        {
            JsonReadError error = std::move(value).error();
            error.message = std::format("{} `{}` of `{}`: {}", kind, member.key, uid.ToString(), error.message);
            if (error.evaluationFailed)
            {
                error.code = DiagnosticCode::EntityAttributeEvaluation;
                error.message = "failed to evaluate " + error.message;
            }
            return std::unexpected{std::move(error)};
        }
        entries[index] = RecordEntry{arena.CopyText(member.key), *value};
    }
    auto record = arena.AdoptRecord(entries);
    if (!record)
    {
        return std::unexpected{ShapeError(node, std::format("the key `{}` appears twice", record.error().key))};
    }
    return record->AsRecord();
}

/// Reads one entity (Rust's `parse_ejson`). With a schema, an entity that is not an action must
/// have a declared type, and its attributes and tags are read by their declared types; the
/// remaining conformance checks come after.
[[nodiscard]] auto ReadEntity(const JsonNode& node, const std::shared_ptr<ValueArena>& arena,
                              const detail::ValidatorSchema* schema) -> std::expected<Entity, JsonReadError>
{
    auto fields = SplitEntity(node);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }

    EntityDescription description;
    if (schema != nullptr) { description.names = EntityTypeNames::Existing; }
    auto uid = ReadUidField(*fields->uid, description.names);
    if (!uid)
    {
        JsonReadError error = std::move(uid).error();
        error.message = "in the entity's `uid`: " + error.message;
        return std::unexpected{std::move(error)};
    }
    if (schema != nullptr && !uid->IsAction())
    {
        description.declaration = schema->FindEntityType(uid->Type());
        if (description.declaration == nullptr)
        {
            return std::unexpected{
                JsonReadError{.code = DiagnosticCode::UnexpectedEntityType,
                              .offset = fields->uid->offset,
                              .length = fields->uid->length,
                              .message = std::format("entity `{}` has the type `{}`, which the schema does not declare",
                                                     uid->ToString(), uid->Type().GetName().ToString())}};
        }
    }

    auto attributes = ReadValueMap(*fields->attributes, "attrs", *uid, description, *arena);
    if (!attributes) { return std::unexpected{std::move(attributes).error()}; }
    Record tags{};
    if (fields->tags != nullptr)
    {
        auto read = ReadValueMap(*fields->tags, "tags", *uid, description, *arena);
        if (!read) { return std::unexpected{std::move(read).error()}; }
        tags = *read;
    }

    if (!fields->parents->IsArray())
    {
        return std::unexpected{ShapeError(*fields->parents, std::format("`parents` must be an array, not `{}`",
                                                                         detail::DescribeJson(*fields->parents)))};
    }
    std::vector<EntityUid> parents;
    parents.reserve(fields->parents->count);
    for (const JsonNode& element : fields->parents->Elements())
    {
        auto parent = ReadUidField(element, description.names);
        if (!parent)
        {
            JsonReadError error = std::move(parent).error();
            error.message = std::format("in the parents of `{}`: {}", uid->ToString(), error.message);
            return std::unexpected{std::move(error)};
        }
        // entities.rs `is_parent_allowed`: an action's parents are actions.
        if (uid->IsAction() && !parent->IsAction())
        {
            return std::unexpected{JsonReadError{
                .code = DiagnosticCode::ActionParentIsNotAction,
                .offset = element.offset,
                .length = element.length,
                .message = std::format("action `{}` has a parent `{}` that is not an action", uid->ToString(),
                                       parent->ToString()),
                .help = "the parents of an action are actions: entities whose type is `Action`, perhaps namespaced"}};
        }
        parents.push_back(*std::move(parent));
    }
    return Entity{*std::move(uid), std::move(parents), *attributes, tags, arena};
}

/// The parse of `json`, or the syntax error as a diagnostic.
template <typename Error>
[[nodiscard]] auto Parse(const std::string_view json, std::pmr::memory_resource& resource,
                         const std::string_view sourceName) -> std::expected<detail::JsonDocument, Error>
{
    auto document = detail::ReadJson(json, resource);
    if (!document)
    {
        return std::unexpected{Error{detail::ToDiagnostic(detail::FromSyntaxError(std::move(document).error()),
                                                          MakeSourceText(std::string{sourceName}, std::string{json}))}};
    }
    return std::move(*document);
}

template <typename Error>
[[nodiscard]] auto Fail(JsonReadError error, const std::string_view json, const std::string_view sourceName) -> Error
{
    return Error{detail::ToDiagnostic(std::move(error), MakeSourceText(std::string{sourceName}, std::string{json}))};
}

/// The entities of the JSON array `json`, each read as ReadEntity reads it.
[[nodiscard]] auto ReadEntities(const std::string_view json, const std::string_view sourceName,
                                const detail::ValidatorSchema* schema) -> std::expected<std::vector<Entity>, EntitiesError>
{
    std::pmr::monotonic_buffer_resource scratch;
    auto document = Parse<EntitiesError>(json, scratch, sourceName);
    if (!document) { return std::unexpected{std::move(document).error()}; }
    const JsonNode& root = document->Root();
    if (!root.IsArray())
    {
        return std::unexpected{Fail<EntitiesError>(
            ShapeError(root, std::format("expected an array of entities, but got `{}`", detail::DescribeJson(root))),
            json, sourceName)};
    }
    auto arena = std::make_shared<ValueArena>();
    std::vector<Entity> entities;
    entities.reserve(root.count);
    for (const JsonNode& element : root.Elements())
    {
        auto entity = ReadEntity(element, arena, schema);
        if (!entity) { return std::unexpected{Fail<EntitiesError>(std::move(entity).error(), json, sourceName)}; }
        entities.push_back(*std::move(entity));
    }
    return entities;
}

[[nodiscard]] auto ReadSingleEntity(const std::string_view json, const std::string_view sourceName,
                                    const detail::ValidatorSchema* schema) -> std::expected<Entity, EntitiesError>
{
    std::pmr::monotonic_buffer_resource scratch;
    auto document = Parse<EntitiesError>(json, scratch, sourceName);
    if (!document) { return std::unexpected{std::move(document).error()}; }
    auto entity = ReadEntity(document->Root(), std::make_shared<ValueArena>(), schema);
    if (!entity) { return std::unexpected{Fail<EntitiesError>(std::move(entity).error(), json, sourceName)}; }
    return *std::move(entity);
}

} // namespace

auto EntitiesFromJson(const std::string_view json, const std::string_view sourceName)
    -> std::expected<Entities, EntitiesError>
{
    auto entities = ReadEntities(json, sourceName, nullptr);
    if (!entities) { return std::unexpected{std::move(entities).error()}; }
    // api.rs `Entities::from_json_str` computes the transitive closure.
    return Entities::Create(*std::move(entities), TransitiveClosureMode::ComputeNow);
}

auto EntitiesFromJson(const std::string_view json, const Schema& schema, const std::string_view sourceName)
    -> std::expected<Entities, EntitiesError>
{
    auto entities = ReadEntities(json, sourceName, &detail::SchemaAccess::Validator(schema));
    if (!entities) { return std::unexpected{std::move(entities).error()}; }
    return Entities::Create(*std::move(entities), TransitiveClosureMode::ComputeNow, schema);
}

auto EntityFromJson(const std::string_view json, const std::string_view sourceName) -> std::expected<Entity, EntitiesError>
{
    return ReadSingleEntity(json, sourceName, nullptr);
}

auto EntityFromJson(const std::string_view json, const Schema& schema, const std::string_view sourceName)
    -> std::expected<Entity, EntitiesError>
{
    // `single_from_ejson`: the entity alone is checked, its parents standing for its ancestors.
    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    auto entity = ReadSingleEntity(json, sourceName, &validator);
    if (!entity) { return entity; }
    if (auto checked = detail::CheckEntityConformance(validator, *entity); !checked)
    {
        return std::unexpected{EntitiesError{std::move(checked).error()}};
    }
    return entity;
}

auto EntityUidFromJson(const std::string_view json, const std::string_view sourceName)
    -> std::expected<EntityUid, ParseError>
{
    // Rust reads the text into a `serde_json::Value` first, so a repeated key keeps its last value.
    std::pmr::monotonic_buffer_resource scratch;
    auto document = Parse<ParseError>(json, scratch, sourceName);
    if (!document) { return std::unexpected{std::move(document).error()}; }
    const JsonNode& root = document->Root();
    if (auto checked = detail::CheckStrictJson(root, DuplicateKeys::LastWins); !checked)
    {
        return std::unexpected{Fail<ParseError>(std::move(checked).error(), json, sourceName)};
    }
    auto uid = detail::ReadEntityUidJson(root, DuplicateKeys::LastWins);
    if (!uid) { return std::unexpected{Fail<ParseError>(std::move(uid).error(), json, sourceName)}; }
    return *std::move(uid);
}

} // namespace cedar
