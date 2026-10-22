// Partial entities from Rust's partial entity JSON format (`PartialEntities::from_json_value` and
// `parse_ejson`, `cedar-policy-core/src/tpe/entities.rs`). Reading is the deserialization phase:
// its errors are the JSON reader's codes, the evaluation codes of a failing extension call, and
// `partial-evaluation.unexpected-action` and `partial-evaluation.undeclared-type-with-data`.
// Checking the result against the schema is PartialEntities::Create's.

#include "entities/TransitiveClosure.hpp"
#include "json/JsonReader.hpp"
#include "json/ValueJson.hpp"
#include "partial-evaluation/PartialEvaluationAccess.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Json.hpp>
#include <cedar/PartialEvaluation.hpp>

#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

using detail::DuplicateKeys;
using detail::EntityTypeNames;
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

/// One entity's fields as serde reads Rust's partial `EntityJson` from a `serde_json::Value`. A
/// field that is absent or null is unknown (null here).
struct EntityFields
{
    const JsonNode* uid = nullptr;
    const JsonNode* attributes = nullptr;
    const JsonNode* parents = nullptr;
    const JsonNode* tags = nullptr;
};

/// `node` when it is given and not null.
[[nodiscard]] auto Known(const JsonNode* node) noexcept -> const JsonNode*
{
    return node != nullptr && node->kind != detail::JsonKind::Null ? node : nullptr;
}

/// Splits an entity into its fields and checks their shapes, as serde's derived struct does: an
/// object (`uid` required, other keys ignored) or a sequence of one to four elements in field
/// order; `attrs` and `tags` objects, `parents` an array.
[[nodiscard]] auto SplitEntity(const JsonNode& node) -> std::expected<EntityFields, JsonReadError>
{
    EntityFields fields;
    if (node.IsArray())
    {
        const auto elements = node.Elements();
        if (elements.empty() || elements.size() > 4)
        {
            return std::unexpected{ShapeError(
                node, std::format("an entity written as an array has 1 to 4 elements (uid, attrs, parents, tags), "
                                  "not {}",
                                  elements.size()))};
        }
        const JsonNode** const slots[] = {&fields.uid, &fields.attributes, &fields.parents, &fields.tags};
        for (std::size_t index = 0; index < elements.size(); ++index) { *slots[index] = &elements[index]; }
    }
    else if (node.IsObject())
    {
        std::vector<const JsonMember*> members;
        detail::EffectiveMembers(node, DuplicateKeys::LastWins, members);
        for (const JsonMember* member : members)
        {
            if (member->key == "uid") { fields.uid = &member->value; }
            else if (member->key == "attrs") { fields.attributes = &member->value; }
            else if (member->key == "parents") { fields.parents = &member->value; }
            else if (member->key == "tags") { fields.tags = &member->value; }
        }
        if (fields.uid == nullptr) { return std::unexpected{ShapeError(node, "the entity has no `uid` field")}; }
    }
    else
    {
        return std::unexpected{ShapeError(
            node, std::format("expected an entity object, but got `{}`", detail::DescribeJson(node)))};
    }
    fields.attributes = Known(fields.attributes);
    fields.parents = Known(fields.parents);
    fields.tags = Known(fields.tags);
    for (const auto& [field, name] : {std::pair{fields.attributes, "attrs"}, std::pair{fields.tags, "tags"}})
    {
        if (field != nullptr && !field->IsObject())
        {
            return std::unexpected{ShapeError(
                *field, std::format("`{}` must be an object or null, not `{}`", name, detail::DescribeJson(*field)))};
        }
    }
    if (fields.parents != nullptr && !fields.parents->IsArray())
    {
        return std::unexpected{ShapeError(*fields.parents, std::format("`parents` must be an array or null, not `{}`",
                                                                        detail::DescribeJson(*fields.parents)))};
    }
    return fields;
}

/// Reads `attrs` or `tags` (Rust's `parse_value_map`): the entity's type must be declared, and each
/// value is read by the type `typeOf` gives its key, or without a type when it gives none.
[[nodiscard]] auto ReadValueMap(const JsonNode& node, const std::string_view kind, const EntityUid& uid,
                                const detail::ValidatorSchema& schema, ValueArena& arena, auto&& typeOf)
    -> std::expected<Record, JsonReadError>
{
    const detail::ValidatorEntityType* const declaration = schema.FindEntityType(uid.Type());
    if (declaration == nullptr)
    {
        return std::unexpected{JsonReadError{
            .code = DiagnosticCode::UndeclaredTypeWithData,
            .offset = node.offset,
            .length = node.length,
            .message = std::format("entity `{}` has {}s, but the schema does not declare its type `{}`",
                                   uid.ToString(), kind, uid.Type().GetName().ToString())}};
    }
    std::vector<const JsonMember*> members;
    detail::EffectiveMembers(node, DuplicateKeys::LastWins, members);
    const std::span<RecordEntry> entries = arena.AllocateRecordEntries(members.size());
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        const JsonMember& member = *members[index];
        const detail::Type* const expected = typeOf(*declaration, member.key);
        auto value = expected != nullptr
                         ? detail::ReadTypedCedarValue(member.value, *expected, DuplicateKeys::LastWins, arena,
                                                       EntityTypeNames::Intern)
                         : detail::ReadCedarValue(member.value, DuplicateKeys::LastWins, arena, EntityTypeNames::Intern);
        if (!value)
        {
            JsonReadError error = std::move(value).error();
            error.message = std::format("{} `{}` of `{}`: {}", kind, member.key, uid.ToString(), error.message);
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

/// Reads one entity (Rust's `parse_ejson`), without checking it against the schema.
[[nodiscard]] auto ReadEntity(const EntityFields& fields, const detail::ValidatorSchema& schema,
                              const std::shared_ptr<ValueArena>& arena) -> std::expected<PartialEntity, JsonReadError>
{
    auto uid = detail::ReadEntityUidJson(*fields.uid, DuplicateKeys::LastWins, EntityTypeNames::Intern);
    if (!uid)
    {
        JsonReadError error = std::move(uid).error();
        error.message = "in the entity's `uid`: " + error.message;
        return std::unexpected{std::move(error)};
    }
    if (uid->IsAction())
    {
        return std::unexpected{JsonReadError{
            .code = DiagnosticCode::UnexpectedAction,
            .offset = fields.uid->offset,
            .length = fields.uid->length,
            .message = std::format("the action `{}` is given as a partial entity", uid->ToString()),
            .help = "partial entities take their action entities from the schema"}};
    }

    std::optional<Record> attributes;
    if (fields.attributes != nullptr)
    {
        auto read = ReadValueMap(*fields.attributes, "attribute", *uid, schema, *arena,
                                 [](const detail::ValidatorEntityType& declaration,
                                    const std::string_view key) -> const detail::Type* {
                                     const detail::AttributeType* found = declaration.attributes.Find(key);
                                     return found != nullptr ? found->type.get() : nullptr;
                                 });
        if (!read) { return std::unexpected{std::move(read).error()}; }
        attributes = *read;
    }

    std::optional<std::vector<EntityUid>> ancestors;
    if (fields.parents != nullptr)
    {
        ancestors.emplace();
        for (const JsonNode& element : fields.parents->Elements())
        {
            auto parent = detail::ReadEntityUidJson(element, DuplicateKeys::LastWins, EntityTypeNames::Intern);
            if (!parent)
            {
                JsonReadError error = std::move(parent).error();
                error.message = std::format("in the parents of `{}`: {}", uid->ToString(), error.message);
                return std::unexpected{std::move(error)};
            }
            ancestors->push_back(*std::move(parent));
        }
        detail::SortUidSet(*ancestors);
    }

    std::optional<Record> tags;
    if (fields.tags != nullptr)
    {
        auto read = ReadValueMap(*fields.tags, "tag", *uid, schema, *arena,
                                 [](const detail::ValidatorEntityType& declaration,
                                    std::string_view) -> const detail::Type* {
                                     return declaration.tags ? &*declaration.tags : nullptr;
                                 });
        if (!read) { return std::unexpected{std::move(read).error()}; }
        tags = *read;
    }
    return detail::PartialEvaluationAccess::MakeEntity(*std::move(uid), attributes, std::move(ancestors), tags, arena);
}

} // namespace

auto PartialEntitiesFromJson(const std::string_view json, const Schema& schema, const std::string_view sourceName)
    -> std::expected<PartialEntities, PartialEntitiesError>
{
    const auto fail = [&](JsonReadError error) {
        return std::unexpected{PartialEntitiesError{
            detail::ToDiagnostic(std::move(error), MakeSourceText(std::string{sourceName}, std::string{json}))}};
    };
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(json, scratch);
    if (!document) { return fail(detail::FromSyntaxError(std::move(document).error())); }
    const JsonNode& root = document->Root();
    if (auto checked = detail::CheckStrictJson(root, DuplicateKeys::LastWins); !checked)
    {
        return fail(std::move(checked).error());
    }
    if (!root.IsArray())
    {
        return fail(ShapeError(root, std::format("expected an array of entities, but got `{}`", detail::DescribeJson(root))));
    }

    // serde reads the whole array before any entity is parsed.
    std::vector<EntityFields> split;
    split.reserve(root.count);
    for (const JsonNode& element : root.Elements())
    {
        auto fields = SplitEntity(element);
        if (!fields) { return fail(std::move(fields).error()); }
        split.push_back(*fields);
    }

    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    auto arena = std::make_shared<ValueArena>();
    std::vector<PartialEntity> entities;
    entities.reserve(split.size());
    for (const EntityFields& fields : split)
    {
        auto entity = ReadEntity(fields, validator, arena);
        if (!entity) { return fail(std::move(entity).error()); }
        entities.push_back(*std::move(entity));
    }
    return PartialEntities::Create(std::move(entities), schema);
}

} // namespace cedar
