#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>

#include <algorithm>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cedar::detail
{

// The unlinked schema model behind `SchemaFragment` (Rust's `json_schema::Fragment<RawName>`,
// `cedar-policy-core/src/validator/json_schema.rs`). Both schema formats read into it and both
// printers write from it. Type references are kept as written: a name is not yet qualified with
// the namespace it appears in, and `EntityOrCommon` is not yet resolved. Resolving them is the
// linker's job.
//
// Every keyed collection is a vector sorted by its key with each key once (Rust's `BTreeMap`), so
// a defaulted `==` is Rust's map equality. Lists that Rust keeps as a `Vec` (parent types,
// principal and resource types, enum choices, an action's parents) keep the written order, and
// their order counts in equality, as in Rust.

/// A source span carried by a model node. It never takes part in equality, as Rust's
/// `#[educe(PartialEq(ignore))] loc` fields do not. Nodes read from JSON have none; the Cedar
/// schema parser sets them so the linker can point its errors at the declaration.
struct ModelSpan
{
    std::optional<SourceSpan> value{};

    friend constexpr auto operator==(const ModelSpan&, const ModelSpan&) noexcept -> bool { return true; }
};

/// A reference to an entity or common type, as written (Rust's `RawName`, an `InternalName` that
/// may name `__cedar` and is not yet qualified with the enclosing namespace).
struct TypeName
{
    Name name;
    ModelSpan span{};

    friend auto operator==(const TypeName&, const TypeName&) -> bool = default;
};

struct SchemaType;

/// One attribute of a record type (Rust's `TypeOfAttribute`). The type is boxed so that a record
/// type can hold its attributes before SchemaType is complete. Its `==` (and Set's) is defined in
/// the .cpp: defaulting it inline makes Clang instantiate the recursive comparison while
/// SchemaType is incomplete.
struct RecordAttribute
{
    std::string name;
    std::indirect<SchemaType> type;
    bool required = true;
    Annotations annotations{};
    ModelSpan span{};

    friend auto operator==(const RecordAttribute&, const RecordAttribute&) -> bool;
};

/// The kinds of schema type (Rust's `TypeVariant` plus `Type::CommonTypeRef`).
namespace schema_type
{

struct String
{
    friend auto operator==(const String&, const String&) -> bool = default;
};

struct Long
{
    friend auto operator==(const Long&, const Long&) -> bool = default;
};

struct Boolean
{
    friend auto operator==(const Boolean&, const Boolean&) -> bool = default;
};

struct Set
{
    std::indirect<SchemaType> element;

    friend auto operator==(const Set&, const Set&) -> bool;
};

/// Attributes sorted by name, each name once (Rust's `BTreeMap<SmolStr, TypeOfAttribute>`).
struct Record
{
    std::vector<RecordAttribute> attributes{};
    bool additionalAttributes = false;

    friend auto operator==(const Record&, const Record&) -> bool = default;
};

/// `{"type": "Entity", "name": ..}`: must name an entity type.
struct Entity
{
    TypeName name;

    friend auto operator==(const Entity&, const Entity&) -> bool = default;
};

/// `{"type": "EntityOrCommon", "name": ..}`, and every type name in Cedar schema syntax: names an
/// entity type or a common type, which the linker decides.
struct EntityOrCommon
{
    TypeName name;

    friend auto operator==(const EntityOrCommon&, const EntityOrCommon&) -> bool = default;
};

/// `{"type": "Extension", "name": ..}`. The name is an unreserved identifier; whether it is a
/// known extension type is checked when linking.
struct Extension
{
    Identifier name;

    friend auto operator==(const Extension&, const Extension&) -> bool = default;
};

/// `{"type": "<name>"}` with any other name: must name a common type (or a `__cedar` built-in).
struct CommonTypeReference
{
    TypeName name;

    friend auto operator==(const CommonTypeReference&, const CommonTypeReference&) -> bool = default;
};

} // namespace schema_type

/// A schema type as written (Rust's `json_schema::Type`).
struct SchemaType
{
    using Kind = std::variant<schema_type::String, schema_type::Long, schema_type::Boolean, schema_type::Set,
                              schema_type::Record, schema_type::Entity, schema_type::EntityOrCommon,
                              schema_type::Extension, schema_type::CommonTypeReference>;

    Kind kind;
    ModelSpan span{};

    friend auto operator==(const SchemaType&, const SchemaType&) -> bool = default;
};

/// The record type with no attributes that does not allow others: what an entity shape or an
/// action context is when none is given.
[[nodiscard]] auto EmptyRecordType() -> SchemaType;

/// Whether `type` is the empty record EmptyRecordType gives (Rust's `is_empty_record`). The
/// printers leave such a shape or context out.
[[nodiscard]] auto IsEmptyRecord(const SchemaType& type) noexcept -> bool;

/// A common type declaration, `type Name = ...;` (Rust's `CommonType`). The name is an unreserved
/// identifier that is not one of the JSON type keywords (IsReservedCommonTypeName).
struct CommonTypeDeclaration
{
    Identifier name;
    SchemaType type;
    Annotations annotations{};
    ModelSpan span{};

    friend auto operator==(const CommonTypeDeclaration&, const CommonTypeDeclaration&) -> bool = default;
};

/// An entity type with parents, a shape and tags (Rust's `StandardEntityType`).
struct StandardEntityType
{
    std::vector<TypeName> memberOfTypes{};
    SchemaType shape = EmptyRecordType();
    std::optional<SchemaType> tags{};

    friend auto operator==(const StandardEntityType&, const StandardEntityType&) -> bool = default;
};

/// An enumerated entity type: only these entity ids exist (Rust's `EntityTypeKind::Enum`, RFC 53).
/// Never empty; the same id may appear twice.
struct EnumEntityType
{
    std::vector<Eid> choices;

    friend auto operator==(const EnumEntityType&, const EnumEntityType&) -> bool = default;
};

/// An entity type declaration (Rust's `json_schema::EntityType`). An external one (RFC 116) is a
/// StandardEntityType whose shape is the empty record and which has no tags: only its parents are
/// declared here.
struct EntityTypeDeclaration
{
    Identifier name;
    std::variant<StandardEntityType, EnumEntityType> kind = StandardEntityType{};
    bool external = false;
    Annotations annotations{};
    ModelSpan span{};

    friend auto operator==(const EntityTypeDeclaration&, const EntityTypeDeclaration&) -> bool = default;
};

/// A reference to an action (Rust's `ActionEntityUID<RawName>`). No type means `Action` in the
/// namespace the reference is resolved in.
struct ActionReference
{
    std::string id;
    std::optional<TypeName> type{};
    ModelSpan span{};

    friend auto operator==(const ActionReference&, const ActionReference&) -> bool = default;
};

/// What an action applies to (Rust's `ApplySpec`). An empty list means no principal or resource.
struct AppliesTo
{
    std::vector<TypeName> principalTypes{};
    std::vector<TypeName> resourceTypes{};
    SchemaType context = EmptyRecordType();

    friend auto operator==(const AppliesTo&, const AppliesTo&) -> bool = default;
};

/// An action declaration (Rust's `ActionType`). `appliesTo` and `memberOf` keep whether they were
/// given at all: Rust's `Option` fields, which take part in equality and in what the JSON printer
/// writes. An external action (RFC 116) has no `appliesTo`. Rust's vestigial `attributes` field is
/// not modelled: a fragment that sets it is refused (`schema.unsupported-schema-feature`).
struct ActionDeclaration
{
    std::string name;
    std::optional<AppliesTo> appliesTo{};
    std::optional<std::vector<ActionReference>> memberOf{};
    bool external = false;
    Annotations annotations{};
    ModelSpan span{};

    friend auto operator==(const ActionDeclaration&, const ActionDeclaration&) -> bool = default;
};

/// One namespace of a fragment (Rust's `NamespaceDefinition`). No name is the empty namespace,
/// which has no annotations. Declarations are sorted by name text, each name once.
struct NamespaceDefinition
{
    std::optional<Name> name{};
    std::vector<CommonTypeDeclaration> commonTypes{};
    std::vector<EntityTypeDeclaration> entityTypes{};
    std::vector<ActionDeclaration> actions{};
    Annotations annotations{};
    ModelSpan span{};

    friend auto operator==(const NamespaceDefinition&, const NamespaceDefinition&) -> bool = default;
};

/// A whole fragment: its namespaces sorted by name in Name order with the empty namespace first
/// (Rust's `BTreeMap<Option<Name>, NamespaceDefinition>`), each once. `source` is the text the
/// fragment was read from, which the spans point into; it takes no part in equality.
class SchemaFragmentModel final
{
public:
    std::vector<NamespaceDefinition> namespaces{};
    SharedSourceText source{};

    friend auto operator==(const SchemaFragmentModel& left, const SchemaFragmentModel& right) -> bool
    {
        return left.namespaces == right.namespaces;
    }
};

/// The basenames a common type may not have, because the JSON format reads them as built-in
/// types (Rust's `CommonTypeId::is_reserved_schema_keyword`).
[[nodiscard]] constexpr auto IsReservedCommonTypeName(const std::string_view name) noexcept -> bool
{
    return name == "Bool" || name == "Boolean" || name == "Entity" || name == "Extension" || name == "Long" ||
           name == "Record" || name == "Set" || name == "String";
}

/// Inserts `declaration` into `declarations`, keeping them sorted by `name` (any of the key types
/// above: Identifier, std::string, an optional Name). Returns false, changing nothing, when the
/// name is already there.
template <typename Declaration>
[[nodiscard]] auto InsertSorted(std::vector<Declaration>& declarations, Declaration declaration) -> bool
{
    const auto position = std::ranges::lower_bound(declarations, declaration.name, std::less<>{}, &Declaration::name);
    if (position != declarations.end() && position->name == declaration.name) { return false; }
    declarations.insert(position, std::move(declaration));
    return true;
}

/// The declaration named `name` in sorted `declarations`, or null.
template <typename Declaration, typename Key>
[[nodiscard]] auto FindSorted(const std::span<const Declaration> declarations, const Key& name) -> const Declaration*
{
    const auto position = std::ranges::lower_bound(declarations, name, std::less<>{}, &Declaration::name);
    return position != declarations.end() && position->name == name ? &*position : nullptr;
}

/// The checks Rust makes when a fragment becomes a public `SchemaFragment` (`api.rs`
/// `SchemaFragment::from_json_str` and `from_cedarschema_str`, through
/// `ValidatorNamespaceDef::check_action_restrictions`) that the model itself cannot express: no
/// namespace declares an entity type named `Action` (`schema.action-entity-type-declared`).
[[nodiscard]] auto CheckFragmentRestrictions(const SchemaFragmentModel& model) -> std::expected<void, SchemaError>;

} // namespace cedar::detail
