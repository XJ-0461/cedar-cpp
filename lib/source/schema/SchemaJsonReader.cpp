// The JSON schema format into the fragment model (Rust's `SchemaFragment::from_json_str`,
// `cedar-policy/src/api.rs`, over serde's reading of `json_schema::Fragment<RawName>`,
// `cedar-policy-core/src/validator/json_schema.rs`).
//
// Rust reads a schema strictly everywhere: every struct denies unknown fields (the flattened
// `CommonType` and `TypeOfAttribute` pass theirs to the `Type` visitor, which denies them), every
// map refuses a repeated key (`maps_duplicate_key_is_error`), every struct refuses a repeated
// field, and no field is skipped unread. No schema field is a number. So one CheckStrictJson pass
// that refuses repeated keys anywhere, then a walk that knows each object's fields, accepts exactly
// what serde accepts. That includes serde's sequence form of the structs it derives without
// `flatten` (a namespace, an entity type, an action, `appliesTo`, an action reference): `[]` is an
// entity type with every field left out.

#include "json/JsonReader.hpp"
#include "json/ValueJson.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"

#include <cedar/Schema.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

using detail::JsonKind;
using detail::JsonMember;
using detail::JsonNode;
using detail::JsonReadError;
using detail::SchemaType;
using detail::TypeName;
namespace schema_type = detail::schema_type;

template <typename Value>
using Read = std::expected<Value, JsonReadError>;

[[nodiscard]] auto ShapeError(const JsonNode& node, std::string message) -> JsonReadError
{
    return JsonReadError{.code = DiagnosticCode::JsonUnexpectedShape,
                         .offset = node.offset,
                         .length = node.length,
                         .message = std::move(message)};
}

[[nodiscard]] auto KeyError(const JsonMember& member, std::string message) -> JsonReadError
{
    return JsonReadError{.code = DiagnosticCode::JsonUnexpectedShape,
                         .offset = member.keyOffset,
                         .length = static_cast<std::uint32_t>(member.key.size() + 2),
                         .message = std::move(message)};
}

[[nodiscard]] auto KindName(const JsonNode& node) -> std::string_view
{
    switch (node.kind)
    {
    case JsonKind::Null: return "null";
    case JsonKind::False:
    case JsonKind::True: return "a boolean";
    case JsonKind::Number: return "a number";
    case JsonKind::String: return "a string";
    case JsonKind::Array: return "an array";
    case JsonKind::Object: return "an object";
    }
    return "a value";
}

[[nodiscard]] auto Expected(const JsonNode& node, const std::string_view what, const std::string_view expected)
    -> JsonReadError
{
    return ShapeError(node, std::format("{}: expected {}, found {}", what, expected, KindName(node)));
}

/// The fields of one struct-like object, by position in the names it was read with. Null pointers
/// are fields not given.
template <std::size_t Count>
using Fields = std::array<const JsonNode*, Count>;

/// How a struct may be written. A struct whose `Deserialize` serde derives without `flatten` also
/// reads serde's sequence form: an array giving the fields in declaration order, possibly fewer of
/// them (the rest are not given), never more.
enum class StructForm : std::uint8_t
{
    ObjectOnly,
    ObjectOrSequence,
};

/// Reads `object` as a struct with fields `names` that denies unknown fields; `passed` names keys
/// another reader takes (the flattened `Type` inside a common type or an attribute), which are
/// neither read nor refused here.
template <std::size_t Count>
[[nodiscard]] auto ReadFields(const JsonNode& object, const std::string_view what,
                              const std::array<std::string_view, Count>& names, const StructForm form,
                              const std::span<const std::string_view> passed = {}) -> Read<Fields<Count>>
{
    Fields<Count> fields{};
    if (form == StructForm::ObjectOrSequence && object.IsArray())
    {
        if (object.count > Count)
        {
            return std::unexpected{ShapeError(object, std::format("{} written as an array has at most {} elements, found {}",
                                                                  what, Count, object.count))};
        }
        for (std::uint32_t index = 0; index < object.count; ++index) { fields[index] = &object.elements[index]; }
        return fields;
    }
    if (!object.IsObject()) { return std::unexpected{Expected(object, what, "an object")}; }
    for (const JsonMember& member : object.Members())
    {
        const auto known = std::ranges::find(names, member.key);
        if (known != names.end())
        {
            fields[static_cast<std::size_t>(known - names.begin())] = &member.value;
            continue;
        }
        if (std::ranges::find(passed, member.key) != passed.end()) { continue; }
        std::string expected;
        for (const std::string_view name : names) { expected += std::format("{}`{}`", expected.empty() ? "" : ", ", name); }
        for (const std::string_view name : passed) { expected += std::format(", `{}`", name); }
        return std::unexpected{KeyError(member, std::format("unknown field `{}` in {}; expected one of {}", member.key,
                                                            what, expected))};
    }
    return fields;
}

[[nodiscard]] auto IsNull(const JsonNode* node) noexcept -> bool { return node != nullptr && node->kind == JsonKind::Null; }

[[nodiscard]] auto ReadBool(const JsonNode& node, const std::string_view what) -> Read<bool>
{
    if (node.kind == JsonKind::True) { return true; }
    if (node.kind == JsonKind::False) { return false; }
    return std::unexpected{Expected(node, what, "a boolean")};
}

[[nodiscard]] auto ReadString(const JsonNode& node, const std::string_view what) -> Read<std::string_view>
{
    if (!node.IsString()) { return std::unexpected{Expected(node, what, "a string")}; }
    return node.text;
}

[[nodiscard]] auto DescribeNameError(const NameError error) -> std::string_view
{
    switch (error)
    {
    case NameError::InvalidSyntax: return "not a valid name";
    case NameError::ReservedKeyword: return "a reserved keyword";
    case NameError::ReservedNamespace: return "in the reserved `__cedar` namespace";
    }
    return "invalid";
}

/// A type reference (Rust's `RawName`: an `InternalName`, so `__cedar` is allowed).
[[nodiscard]] auto ReadTypeName(const JsonNode& node, const std::string_view what) -> Read<TypeName>
{
    auto text = ReadString(node, what);
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto name = Name::ParseInternal(*text);
    if (!name)
    {
        return std::unexpected{ShapeError(node, std::format("{}: `{}` is {}", what, *text, DescribeNameError(name.error())))};
    }
    return TypeName{.name = *name};
}

[[nodiscard]] auto ReadTypeNames(const JsonNode& node, const std::string_view what) -> Read<std::vector<TypeName>>
{
    if (!node.IsArray()) { return std::unexpected{Expected(node, what, "an array of type names")}; }
    std::vector<TypeName> names;
    names.reserve(node.count);
    for (const JsonNode& element : node.Elements())
    {
        auto name = ReadTypeName(element, what);
        if (!name) { return std::unexpected{std::move(name).error()}; }
        names.push_back(*std::move(name));
    }
    return names;
}

/// Rust's `UnreservedId`: an identifier that is not a keyword and not `__cedar`.
[[nodiscard]] auto ParseUnreservedIdentifier(const std::string_view text) -> std::expected<Identifier, NameError>
{
    auto identifier = Identifier::Parse(text);
    if (identifier && identifier->IsReserved()) { return std::unexpected{NameError::ReservedNamespace}; }
    return identifier;
}

/// Rust's `est::Annotations`: identifier-syntax keys (keywords allowed), each a string or null.
[[nodiscard]] auto ReadAnnotations(const JsonNode* node) -> Read<Annotations>
{
    Annotations annotations;
    if (node == nullptr) { return annotations; }
    if (!node->IsObject()) { return std::unexpected{Expected(*node, "annotations", "an object")}; }
    for (const JsonMember& member : node->Members())
    {
        if (!IsIdentifierSyntax(member.key))
        {
            return std::unexpected{KeyError(member, std::format("annotation key `{}` is not an identifier", member.key))};
        }
        std::optional<std::string> value;
        if (member.value.IsString()) { value.emplace(member.value.text); }
        else if (member.value.kind != JsonKind::Null)
        {
            return std::unexpected{Expected(member.value, std::format("annotation `{}`", member.key), "a string or null")};
        }
        if (!annotations.Insert(Symbol::Intern(member.key), std::move(value)))
        {
            return std::unexpected{KeyError(member, std::format("annotation `{}` given twice", member.key))};
        }
    }
    return annotations;
}

constexpr std::array<std::string_view, 5> TypeFieldNames{"type", "element", "attributes", "additionalAttributes", "name"};
constexpr std::array<std::string_view, 1> CommonTypeOwnFields{"annotations"};
constexpr std::array<std::string_view, 2> AttributeOwnFields{"annotations", "required"};

auto ReadType(const JsonNode& node, std::span<const std::string_view> passed = {}) -> Read<SchemaType>;

/// A record attribute (Rust's `TypeOfAttribute`: `annotations` and `required` beside a flattened
/// `Type`).
[[nodiscard]] auto ReadAttribute(const JsonMember& member) -> Read<detail::RecordAttribute>
{
    auto own = ReadFields(member.value, "a record attribute", AttributeOwnFields, StructForm::ObjectOnly, TypeFieldNames);
    if (!own) { return std::unexpected{std::move(own).error()}; }
    const auto [annotationsNode, requiredNode] = *own;
    auto annotations = ReadAnnotations(annotationsNode);
    if (!annotations) { return std::unexpected{std::move(annotations).error()}; }
    bool required = true;
    if (requiredNode != nullptr)
    {
        auto value = ReadBool(*requiredNode, "`required`");
        if (!value) { return std::unexpected{std::move(value).error()}; }
        required = *value;
    }
    auto type = ReadType(member.value, AttributeOwnFields);
    if (!type) { return std::unexpected{std::move(type).error()}; }
    return detail::RecordAttribute{.name = std::string{member.key},
                                   .type = std::indirect<SchemaType>{*std::move(type)},
                                   .required = required,
                                   .annotations = *std::move(annotations)};
}

/// Rust's `TypeVisitor::build_schema_type`: the `type` field picks the kind, and every field the
/// kind does not use is refused.
auto ReadType(const JsonNode& node, const std::span<const std::string_view> passed) -> Read<SchemaType>
{
    auto fields = ReadFields(node, "a type", TypeFieldNames, StructForm::ObjectOnly, passed);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [typeNode, elementNode, attributesNode, additionalNode, nameNode] = *fields;
    if (typeNode == nullptr) { return std::unexpected{ShapeError(node, "a type needs the field `type`")}; }
    auto typeText = ReadString(*typeNode, "`type`");
    if (!typeText) { return std::unexpected{std::move(typeText).error()}; }
    const std::string_view kind = *typeText;

    // Rust reads every given field before it looks at the kind, so a malformed field is refused
    // even where the kind would refuse it anyway; the result is the same either way.
    const auto refuse = [&](const std::initializer_list<std::pair<const JsonNode*, std::string_view>> unused)
        -> std::optional<JsonReadError>
    {
        for (const auto& [field, name] : unused)
        {
            if (field != nullptr)
            {
                return ShapeError(*field, std::format("the field `{}` does not belong in a type `{}`", name, kind));
            }
        }
        return std::nullopt;
    };
    const auto readName = [&](const std::string_view what) -> Read<std::string_view>
    {
        if (nameNode == nullptr) { return std::unexpected{ShapeError(node, std::format("a type `{}` needs the field `name`", kind))}; }
        return ReadString(*nameNode, what);
    };

    if (kind == "String" || kind == "Long" || kind == "Boolean" || !(kind == "Set" || kind == "Record" ||
                                                                      kind == "Entity" || kind == "EntityOrCommon" ||
                                                                      kind == "Extension"))
    {
        if (auto error = refuse({{elementNode, "element"},
                                 {attributesNode, "attributes"},
                                 {additionalNode, "additionalAttributes"},
                                 {nameNode, "name"}}))
        {
            return std::unexpected{*std::move(error)};
        }
        if (kind == "String") { return SchemaType{.kind = schema_type::String{}}; }
        if (kind == "Long") { return SchemaType{.kind = schema_type::Long{}}; }
        if (kind == "Boolean") { return SchemaType{.kind = schema_type::Boolean{}}; }
        auto name = Name::ParseInternal(kind);
        if (!name)
        {
            return std::unexpected{ShapeError(
                *typeNode, std::format("`{}` is not a built-in type and, as a common type name, is {}", kind,
                                       DescribeNameError(name.error())))};
        }
        return SchemaType{.kind = schema_type::CommonTypeReference{TypeName{.name = *name}}};
    }
    if (kind == "Set")
    {
        if (auto error = refuse({{attributesNode, "attributes"}, {additionalNode, "additionalAttributes"}, {nameNode, "name"}}))
        {
            return std::unexpected{*std::move(error)};
        }
        if (elementNode == nullptr) { return std::unexpected{ShapeError(node, "a type `Set` needs the field `element`")}; }
        auto element = ReadType(*elementNode);
        if (!element) { return std::unexpected{std::move(element).error()}; }
        return SchemaType{.kind = schema_type::Set{std::indirect<SchemaType>{*std::move(element)}}};
    }
    if (kind == "Record")
    {
        if (auto error = refuse({{elementNode, "element"}, {nameNode, "name"}})) { return std::unexpected{*std::move(error)}; }
        if (attributesNode == nullptr)
        {
            return std::unexpected{ShapeError(node, "a type `Record` needs the field `attributes`")};
        }
        if (!attributesNode->IsObject()) { return std::unexpected{Expected(*attributesNode, "`attributes`", "an object")}; }
        schema_type::Record record;
        record.attributes.reserve(attributesNode->count);
        for (const JsonMember& member : attributesNode->Members())
        {
            auto attribute = ReadAttribute(member);
            if (!attribute) { return std::unexpected{std::move(attribute).error()}; }
            if (!detail::InsertSorted(record.attributes, *std::move(attribute)))
            {
                return std::unexpected{KeyError(member, std::format("attribute `{}` given twice", member.key))};
            }
        }
        if (additionalNode != nullptr)
        {
            auto additional = ReadBool(*additionalNode, "`additionalAttributes`");
            if (!additional) { return std::unexpected{std::move(additional).error()}; }
            record.additionalAttributes = *additional;
        }
        return SchemaType{.kind = std::move(record)};
    }

    // Entity, EntityOrCommon and Extension name a type in `name`.
    if (auto error = refuse({{elementNode, "element"}, {attributesNode, "attributes"}, {additionalNode, "additionalAttributes"}}))
    {
        return std::unexpected{*std::move(error)};
    }
    auto text = readName("`name`");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    if (kind == "Extension")
    {
        auto name = ParseUnreservedIdentifier(*text);
        if (!name)
        {
            return std::unexpected{ShapeError(*nameNode, std::format("extension type `{}` is {}", *text,
                                                                     DescribeNameError(name.error())))};
        }
        return SchemaType{.kind = schema_type::Extension{*name}};
    }
    auto name = ReadTypeName(*nameNode, kind == "Entity" ? "entity type" : "entity or common type");
    if (!name) { return std::unexpected{std::move(name).error()}; }
    if (kind == "Entity") { return SchemaType{.kind = schema_type::Entity{*std::move(name)}}; }
    return SchemaType{.kind = schema_type::EntityOrCommon{*std::move(name)}};
}

[[nodiscard]] auto ReadCommonType(const JsonMember& member) -> Read<detail::CommonTypeDeclaration>
{
    auto name = ParseUnreservedIdentifier(member.key);
    if (!name)
    {
        return std::unexpected{KeyError(member, std::format("common type name `{}` is {}", member.key,
                                                            DescribeNameError(name.error())))};
    }
    if (detail::IsReservedCommonTypeName(member.key))
    {
        return std::unexpected{KeyError(
            member, std::format("`{}` is a built-in type name and cannot be the name of a common type", member.key))};
    }
    auto own = ReadFields(member.value, "a common type", CommonTypeOwnFields, StructForm::ObjectOnly, TypeFieldNames);
    if (!own) { return std::unexpected{std::move(own).error()}; }
    auto annotations = ReadAnnotations((*own)[0]);
    if (!annotations) { return std::unexpected{std::move(annotations).error()}; }
    auto type = ReadType(member.value, CommonTypeOwnFields);
    if (!type) { return std::unexpected{std::move(type).error()}; }
    return detail::CommonTypeDeclaration{.name = *name, .type = *std::move(type), .annotations = *std::move(annotations)};
}

/// Rust's `Deserialize for EntityType`: every field is optional and none may be null; `external`
/// and then `enum` decide which other fields are allowed.
[[nodiscard]] auto ReadEntityType(const JsonMember& member) -> Read<detail::EntityTypeDeclaration>
{
    auto name = ParseUnreservedIdentifier(member.key);
    if (!name)
    {
        return std::unexpected{KeyError(member, std::format("entity type name `{}` is {}", member.key,
                                                            DescribeNameError(name.error())))};
    }
    constexpr std::array<std::string_view, 6> Names{"memberOfTypes", "shape", "tags", "enum", "external", "annotations"};
    auto fields = ReadFields(member.value, "an entity type", Names, StructForm::ObjectOrSequence);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [memberOfNode, shapeNode, tagsNode, enumNode, externalNode, annotationsNode] = *fields;

    detail::EntityTypeDeclaration declaration{.name = *name};
    detail::StandardEntityType standard;
    if (memberOfNode != nullptr)
    {
        auto parents = ReadTypeNames(*memberOfNode, "`memberOfTypes`");
        if (!parents) { return std::unexpected{std::move(parents).error()}; }
        standard.memberOfTypes = *std::move(parents);
    }
    if (shapeNode != nullptr)
    {
        auto shape = ReadType(*shapeNode);
        if (!shape) { return std::unexpected{std::move(shape).error()}; }
        standard.shape = *std::move(shape);
    }
    if (tagsNode != nullptr)
    {
        auto tags = ReadType(*tagsNode);
        if (!tags) { return std::unexpected{std::move(tags).error()}; }
        standard.tags = *std::move(tags);
    }
    std::optional<detail::EnumEntityType> enumerated;
    if (enumNode != nullptr)
    {
        if (!enumNode->IsArray()) { return std::unexpected{Expected(*enumNode, "`enum`", "an array of strings")}; }
        if (enumNode->count == 0) { return std::unexpected{ShapeError(*enumNode, "`enum` needs at least one entity id")}; }
        enumerated.emplace();
        for (const JsonNode& element : enumNode->Elements())
        {
            auto id = ReadString(element, "an `enum` entity id");
            if (!id) { return std::unexpected{std::move(id).error()}; }
            enumerated->choices.emplace_back(*id);
        }
    }
    if (externalNode != nullptr)
    {
        auto external = ReadBool(*externalNode, "`external`");
        if (!external) { return std::unexpected{std::move(external).error()}; }
        declaration.external = *external;
    }
    auto annotations = ReadAnnotations(annotationsNode);
    if (!annotations) { return std::unexpected{std::move(annotations).error()}; }
    declaration.annotations = *std::move(annotations);

    const auto refuse = [&](const std::string_view kind,
                            const std::initializer_list<std::pair<const JsonNode*, std::string_view>> unused)
        -> std::optional<JsonReadError>
    {
        for (const auto& [field, fieldName] : unused)
        {
            if (field != nullptr)
            {
                return ShapeError(*field, std::format("the field `{}` does not belong on {}", fieldName, kind));
            }
        }
        return std::nullopt;
    };
    if (declaration.external)
    {
        if (auto error = refuse("an external entity type", {{shapeNode, "shape"}, {tagsNode, "tags"}, {enumNode, "enum"}}))
        {
            return std::unexpected{*std::move(error)};
        }
        declaration.kind = detail::StandardEntityType{.memberOfTypes = std::move(standard.memberOfTypes)};
    }
    else if (enumerated)
    {
        if (auto error = refuse("an enumerated entity type",
                                {{memberOfNode, "memberOfTypes"}, {shapeNode, "shape"}, {tagsNode, "tags"}}))
        {
            return std::unexpected{*std::move(error)};
        }
        declaration.kind = *std::move(enumerated);
    }
    else { declaration.kind = std::move(standard); }
    return declaration;
}

[[nodiscard]] auto ReadActionReference(const JsonNode& node) -> Read<detail::ActionReference>
{
    constexpr std::array<std::string_view, 2> Names{"id", "type"};
    auto fields = ReadFields(node, "an action reference", Names, StructForm::ObjectOrSequence);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [idNode, typeNode] = *fields;
    if (idNode == nullptr) { return std::unexpected{ShapeError(node, "an action reference needs the field `id`")}; }
    auto id = ReadString(*idNode, "`id`");
    if (!id) { return std::unexpected{std::move(id).error()}; }
    detail::ActionReference reference{.id = std::string{*id}};
    if (typeNode != nullptr && !IsNull(typeNode))
    {
        auto type = ReadTypeName(*typeNode, "action type");
        if (!type) { return std::unexpected{std::move(type).error()}; }
        reference.type = *std::move(type);
    }
    return reference;
}

[[nodiscard]] auto ReadAppliesTo(const JsonNode& node) -> Read<detail::AppliesTo>
{
    constexpr std::array<std::string_view, 3> Names{"resourceTypes", "principalTypes", "context"};
    auto fields = ReadFields(node, "`appliesTo`", Names, StructForm::ObjectOrSequence);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [resourceNode, principalNode, contextNode] = *fields;
    if (resourceNode == nullptr) { return std::unexpected{ShapeError(node, "`appliesTo` needs the field `resourceTypes`")}; }
    if (principalNode == nullptr) { return std::unexpected{ShapeError(node, "`appliesTo` needs the field `principalTypes`")}; }
    detail::AppliesTo appliesTo;
    auto resources = ReadTypeNames(*resourceNode, "`resourceTypes`");
    if (!resources) { return std::unexpected{std::move(resources).error()}; }
    appliesTo.resourceTypes = *std::move(resources);
    auto principals = ReadTypeNames(*principalNode, "`principalTypes`");
    if (!principals) { return std::unexpected{std::move(principals).error()}; }
    appliesTo.principalTypes = *std::move(principals);
    if (contextNode != nullptr)
    {
        auto context = ReadType(*contextNode);
        if (!context) { return std::unexpected{std::move(context).error()}; }
        appliesTo.context = *std::move(context);
    }
    return appliesTo;
}

/// Rust's `UncheckedActionType` then its `TryFrom`. Unlike an entity type's fields, `attributes`,
/// `appliesTo`, `memberOf` and `external` may be null, meaning not given. `hasAttributes` reports
/// a non-null `attributes`, which the caller refuses once the whole document has been read.
[[nodiscard]] auto ReadAction(const JsonMember& member, bool& hasAttributes) -> Read<detail::ActionDeclaration>
{
    constexpr std::array<std::string_view, 5> Names{"attributes", "appliesTo", "memberOf", "external", "annotations"};
    auto fields = ReadFields(member.value, "an action", Names, StructForm::ObjectOrSequence);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [attributesNode, appliesToNode, memberOfNode, externalNode, annotationsNode] = *fields;

    detail::ActionDeclaration declaration{.name = std::string{member.key}};
    // Any non-null `attributes` is refused, so its contents need no reading beyond the syntax
    // checks every value already passed.
    hasAttributes = attributesNode != nullptr && !IsNull(attributesNode);
    if (appliesToNode != nullptr && !IsNull(appliesToNode))
    {
        auto appliesTo = ReadAppliesTo(*appliesToNode);
        if (!appliesTo) { return std::unexpected{std::move(appliesTo).error()}; }
        declaration.appliesTo = *std::move(appliesTo);
    }
    if (memberOfNode != nullptr && !IsNull(memberOfNode))
    {
        if (!memberOfNode->IsArray())
        {
            return std::unexpected{Expected(*memberOfNode, "`memberOf`", "an array of action references")};
        }
        std::vector<detail::ActionReference> parents;
        parents.reserve(memberOfNode->count);
        for (const JsonNode& element : memberOfNode->Elements())
        {
            auto parent = ReadActionReference(element);
            if (!parent) { return std::unexpected{std::move(parent).error()}; }
            parents.push_back(*std::move(parent));
        }
        declaration.memberOf = std::move(parents);
    }
    if (externalNode != nullptr && !IsNull(externalNode))
    {
        auto external = ReadBool(*externalNode, "`external`");
        if (!external) { return std::unexpected{std::move(external).error()}; }
        declaration.external = *external;
    }
    auto annotations = ReadAnnotations(annotationsNode);
    if (!annotations) { return std::unexpected{std::move(annotations).error()}; }
    declaration.annotations = *std::move(annotations);

    if (declaration.external && (hasAttributes || declaration.appliesTo))
    {
        return std::unexpected{ShapeError(member.value, std::format("external action `{}` may not have {}", member.key,
                                                                    hasAttributes ? "`attributes`" : "`appliesTo`"))};
    }
    return declaration;
}

/// One namespace's declarations, and the actions in it that set `attributes`.
struct NamespaceRead
{
    detail::NamespaceDefinition definition;
    std::vector<std::string> actionsWithAttributes;
};

[[nodiscard]] auto ReadNamespace(const JsonMember& member) -> Read<NamespaceRead>
{
    NamespaceRead result;
    if (!member.key.empty())
    {
        auto name = Name::Parse(member.key);
        if (!name)
        {
            return std::unexpected{
                KeyError(member, std::format("namespace `{}` is {}", member.key, DescribeNameError(name.error())))};
        }
        result.definition.name = *name;
    }
    constexpr std::array<std::string_view, 4> Names{"commonTypes", "entityTypes", "actions", "annotations"};
    auto fields = ReadFields(member.value, "a namespace", Names, StructForm::ObjectOrSequence);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    const auto [commonTypesNode, entityTypesNode, actionsNode, annotationsNode] = *fields;
    if (entityTypesNode == nullptr) { return std::unexpected{ShapeError(member.value, "a namespace needs the field `entityTypes`")}; }
    if (actionsNode == nullptr) { return std::unexpected{ShapeError(member.value, "a namespace needs the field `actions`")}; }

    if (commonTypesNode != nullptr)
    {
        if (!commonTypesNode->IsObject()) { return std::unexpected{Expected(*commonTypesNode, "`commonTypes`", "an object")}; }
        for (const JsonMember& entry : commonTypesNode->Members())
        {
            auto declaration = ReadCommonType(entry);
            if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
            if (!detail::InsertSorted(result.definition.commonTypes, *std::move(declaration)))
            {
                return std::unexpected{KeyError(entry, std::format("common type `{}` declared twice", entry.key))};
            }
        }
    }
    if (!entityTypesNode->IsObject()) { return std::unexpected{Expected(*entityTypesNode, "`entityTypes`", "an object")}; }
    for (const JsonMember& entry : entityTypesNode->Members())
    {
        auto declaration = ReadEntityType(entry);
        if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
        if (!detail::InsertSorted(result.definition.entityTypes, *std::move(declaration)))
        {
            return std::unexpected{KeyError(entry, std::format("entity type `{}` declared twice", entry.key))};
        }
    }
    if (!actionsNode->IsObject()) { return std::unexpected{Expected(*actionsNode, "`actions`", "an object")}; }
    for (const JsonMember& entry : actionsNode->Members())
    {
        bool hasAttributes = false;
        auto declaration = ReadAction(entry, hasAttributes);
        if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
        if (hasAttributes) { result.actionsWithAttributes.emplace_back(entry.key); }
        if (!detail::InsertSorted(result.definition.actions, *std::move(declaration)))
        {
            return std::unexpected{KeyError(entry, std::format("action `{}` declared twice", entry.key))};
        }
    }
    auto annotations = ReadAnnotations(annotationsNode);
    if (!annotations) { return std::unexpected{std::move(annotations).error()}; }
    if (!result.definition.name && !annotations->IsEmpty())
    {
        return std::unexpected{ShapeError(*annotationsNode, "the empty namespace may not have annotations")};
    }
    result.definition.annotations = *std::move(annotations);
    return result;
}

} // namespace

auto SchemaFragmentFromJson(const std::string_view text, const std::string_view sourceName)
    -> std::expected<SchemaFragment, SchemaError>
{
    const SharedSourceText source = MakeSourceText(std::string{sourceName}, std::string{text});
    const auto fail = [&](JsonReadError error)
    { return std::unexpected{SchemaError{detail::ToDiagnostic(std::move(error), source)}}; };

    std::pmr::monotonic_buffer_resource resource;
    auto document = detail::ReadJson(text, resource);
    if (!document) { return fail(detail::FromSyntaxError(std::move(document).error())); }
    const JsonNode& root = document->Root();
    if (auto strict = detail::CheckStrictJson(root, detail::DuplicateKeys::Reject); !strict)
    {
        // Rust reports a repeated key in a schema as a JSON deserialization error like any other.
        JsonReadError error = std::move(strict).error();
        if (error.code == DiagnosticCode::DuplicateKey) { error.code = DiagnosticCode::JsonUnexpectedShape; }
        return fail(std::move(error));
    }
    if (!root.IsObject())
    {
        JsonReadError error = Expected(root, "a schema", "an object mapping namespace names to namespaces");
        error.help = "a schema in the JSON format is an object such as `{\"\": {\"entityTypes\": {}, \"actions\": {}}}`";
        return fail(std::move(error));
    }

    auto model = std::make_shared<detail::SchemaFragmentModel>();
    model->source = source;
    std::vector<std::pair<std::optional<Name>, std::vector<std::string>>> actionsWithAttributes;
    for (const JsonMember& member : root.Members())
    {
        auto read = ReadNamespace(member);
        if (!read) { return fail(std::move(read).error()); }
        if (!read->actionsWithAttributes.empty())
        {
            actionsWithAttributes.emplace_back(read->definition.name, std::move(read->actionsWithAttributes));
        }
        if (!detail::InsertSorted(model->namespaces, std::move(read->definition)))
        {
            return fail(KeyError(member, std::format("namespace `{}` given twice", member.key)));
        }
    }

    // Rust's `api::SchemaFragment` also makes the fragment into a `ValidatorSchemaFragment`, which
    // refuses an `Action` entity type and action attributes in each namespace.
    std::vector<Diagnostic> diagnostics;
    if (auto restrictions = detail::CheckFragmentRestrictions(*model); !restrictions)
    {
        diagnostics = std::move(restrictions).error().TakeDiagnostics();
    }
    for (auto& [name, actions] : actionsWithAttributes)
    {
        std::ranges::sort(actions);
        std::string list;
        for (const std::string& action : actions) { list += std::format("{}`{}`", list.empty() ? "" : ", ", action); }
        diagnostics.push_back(Diagnostic{
            .code = DiagnosticCode::UnsupportedSchemaFeature,
            .message = std::format("actions declared with attributes{}: {}",
                                   name ? std::format(" in namespace `{}`", name->QualifiedText()) : std::string{}, list),
            .help = "action attributes are not supported; remove `attributes` or set it to null",
            .source = source,
        });
    }
    if (auto error = SchemaError::FromDiagnostics(std::move(diagnostics))) { return std::unexpected{*std::move(error)}; }
    return detail::SchemaAccess::MakeFragment(std::move(model));
}

} // namespace cedar
