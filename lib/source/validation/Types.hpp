#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Names.hpp>
#include <cedar/Validator.hpp>
#include <cedar/Value.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cedar::detail
{

// The validator's types (Rust's `cedar-policy-core/src/validator/types.rs`, `Type`). The linked
// schema states every attribute, tag and context type in them, with common types inlined, and
// the typechecker computes with them. Composite types share their parts (Rust's `Arc<Type>`), so
// copying a type, or inlining one common type in many places, does not copy the tree.

struct Type;

/// A type shared between the places that use it. Never null where a type is required.
using SharedType = std::shared_ptr<const Type>;

/// Which booleans a boolean type admits (Rust's `BoolType`).
enum class BooleanType : std::uint8_t
{
    AnyBoolean,
    True,
    False,
};

/// Whether a record or entity type admits attributes it does not list (Rust's `OpenTag`). A schema
/// only declares closed records; the typechecker makes open ones.
enum class OpenTag : std::uint8_t
{
    ClosedAttributes,
    OpenAttributes,
};

/// The type of one attribute and whether it must be present (Rust's `AttributeType`).
struct AttributeType
{
    SharedType type;
    bool required = true;

    /// Compares the types, not the pointers.
    friend auto operator==(const AttributeType& left, const AttributeType& right) -> bool;
};

/// One named attribute of a record or entity type.
struct Attribute
{
    std::string name;
    AttributeType type;

    friend auto operator==(const Attribute&, const Attribute&) -> bool = default;
};

/// The attributes of a record or entity type, sorted by name (byte order, as Value's records),
/// each name once (Rust's `Attributes`). Immutable and shared between copies.
class Attributes final
{
public:
    /// No attributes.
    Attributes() = default;

    /// Takes `attributes`, which must be sorted by name with no name twice.
    explicit Attributes(std::vector<Attribute> attributes);

    /// The attribute called `name`, or null (binary search).
    [[nodiscard]] auto Find(std::string_view name) const noexcept -> const AttributeType*;

    [[nodiscard]] auto Entries() const noexcept -> std::span<const Attribute>
    {
        return entries_ ? std::span<const Attribute>{*entries_} : std::span<const Attribute>{};
    }

    [[nodiscard]] auto Size() const noexcept -> std::size_t { return Entries().size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return Entries().empty(); }
    [[nodiscard]] auto begin() const noexcept { return Entries().begin(); }
    [[nodiscard]] auto end() const noexcept { return Entries().end(); }

    friend auto operator==(const Attributes& left, const Attributes& right) -> bool;

private:
    std::shared_ptr<const std::vector<Attribute>> entries_;
};

/// One or more entity types: an entity of any of them (Rust's `EntityLUB`). Sorted in Name order,
/// each type once. A schema only states single types, and strict validation only makes single
/// types; least upper bounds in permissive validation make the others. A single type is stored
/// inline and several are shared between copies, so copying never allocates.
class EntityLub final
{
public:
    explicit EntityLub(EntityType type) noexcept : first_{type} {}

    /// Takes `types`, which must be non-empty, sorted in Name order and without repeats.
    explicit EntityLub(std::vector<EntityType> types);

    [[nodiscard]] auto Types() const noexcept -> std::span<const EntityType>
    {
        return several_ ? std::span<const EntityType>{*several_} : std::span<const EntityType>{&first_, 1};
    }

    [[nodiscard]] auto IsSingle() const noexcept -> bool { return several_ == nullptr; }

    /// The one type, or null when there are several (Rust's `get_single_entity`).
    [[nodiscard]] auto Single() const noexcept -> const EntityType* { return several_ ? nullptr : &first_; }

    [[nodiscard]] auto Contains(const EntityType& type) const noexcept -> bool;

    /// Every type of this is one of `other`'s (Rust's `EntityLUB::is_subtype`).
    [[nodiscard]] auto IsSubsetOf(const EntityLub& other) const noexcept -> bool;

    /// No type of this is one of `other`'s.
    [[nodiscard]] auto IsDisjointFrom(const EntityLub& other) const noexcept -> bool;

    /// The types of both (Rust's `EntityLUB::least_upper_bound`).
    [[nodiscard]] auto Union(const EntityLub& other) const -> EntityLub;

    friend auto operator==(const EntityLub& left, const EntityLub& right) noexcept -> bool;

private:
    EntityType first_;
    std::shared_ptr<const std::vector<EntityType>> several_;
};

/// The kinds of type.
namespace types
{

/// The type of no value: a subtype of every type.
struct Never
{
    friend auto operator==(const Never&, const Never&) -> bool = default;
};

struct Boolean
{
    BooleanType kind = BooleanType::AnyBoolean;

    friend auto operator==(const Boolean&, const Boolean&) -> bool = default;
};

struct Long
{
    friend auto operator==(const Long&, const Long&) -> bool = default;
};

struct String
{
    friend auto operator==(const String&, const String&) -> bool = default;
};

/// An entity of any type.
struct AnyEntity
{
    friend auto operator==(const AnyEntity&, const AnyEntity&) -> bool = default;
};

/// An entity of one of the given types. Its attributes are the schema's for those types.
struct Entity
{
    EntityLub lub;

    friend auto operator==(const Entity&, const Entity&) -> bool = default;
};

/// A set of `element`s. A null element is a set of anything; the typechecker uses it only to ask
/// whether a type is some set.
struct Set
{
    SharedType element;

    /// Compares the element types, not the pointers.
    friend auto operator==(const Set& left, const Set& right) -> bool;
};

struct Record
{
    Attributes attributes{};
    OpenTag open = OpenTag::ClosedAttributes;

    friend auto operator==(const Record&, const Record&) -> bool = default;
};

/// A value of one of the extension types (ExtensionTypes in ExtensionCalls.hpp), named by the
/// kind of its values.
struct Extension
{
    ValueKind kind;

    friend auto operator==(const Extension&, const Extension&) -> bool = default;
};

} // namespace types

/// A validator type (Rust's `Type`).
struct Type
{
    using Kind = std::variant<types::Never, types::Boolean, types::Long, types::String, types::AnyEntity,
                              types::Entity, types::Set, types::Record, types::Extension>;

    Kind kind;

    friend auto operator==(const Type&, const Type&) -> bool = default;
};

class ValidatorSchema;

/// `type` shared.
[[nodiscard]] auto MakeShared(Type type) -> SharedType;

/// The closed record type with no attributes.
[[nodiscard]] auto EmptyRecord() -> Type;

/// The record `type` is, or null.
[[nodiscard]] inline auto AsRecord(const Type& type) noexcept -> const types::Record*
{
    return std::get_if<types::Record>(&type.kind);
}

// ---- Constructors for the typechecker ----

[[nodiscard]] inline auto AnyBooleanType() -> Type { return Type{types::Boolean{}}; }

/// `True` or `False`.
[[nodiscard]] inline auto SingletonBooleanType(const bool value) -> Type
{
    return Type{types::Boolean{value ? BooleanType::True : BooleanType::False}};
}

[[nodiscard]] inline auto LongType() -> Type { return Type{types::Long{}}; }
[[nodiscard]] inline auto StringType() -> Type { return Type{types::String{}}; }
[[nodiscard]] inline auto NeverType() -> Type { return Type{types::Never{}}; }

/// A set of anything: only for asking whether a type is some set, and for messages.
[[nodiscard]] inline auto AnySetType() -> Type { return Type{types::Set{}}; }

[[nodiscard]] inline auto SetType(SharedType element) -> Type { return Type{types::Set{std::move(element)}}; }

/// The open record with no listed attributes: every record type is a permissive subtype of it.
[[nodiscard]] inline auto AnyRecordType() -> Type
{
    return Type{types::Record{Attributes{}, OpenTag::OpenAttributes}};
}

[[nodiscard]] inline auto NamedEntityType(const EntityType type) -> Type
{
    return Type{types::Entity{EntityLub{type}}};
}

[[nodiscard]] inline auto AnyEntityType() -> Type { return Type{types::AnyEntity{}}; }

[[nodiscard]] inline auto ExtensionType(const ValueKind kind) -> Type { return Type{types::Extension{kind}}; }

/// The type of the entity literal `uid`: its entity type, when `schema` declares that entity type
/// or, for an action type, that action (Rust's `Type::euid_literal`). Nothing otherwise.
[[nodiscard]] auto EntityUidLiteralType(const EntityUid& uid, const ValidatorSchema& schema) -> std::optional<Type>;

// ---- Subtyping and least upper bounds ----

/// Why two types have no least upper bound (Rust's `LubHelp`), for the incompatible-types hint.
enum class LubHelp : std::uint8_t
{
    /// Strict: an attribute is required in one record type and optional in the other.
    AttributeQualifier,
    /// Strict: the record types do not have the same attributes.
    RecordWidth,
    /// Strict: different entity types, or a named entity type and any entity.
    EntityType,
    /// An entity type and a record type.
    EntityRecord,
    /// Any other pair of different types.
    Mismatch,
};

/// One sentence explaining `help`, for messages.
[[nodiscard]] auto LubHelpText(LubHelp help) noexcept -> std::string_view;

/// Whether every value of `subtype` is a value of `supertype` (Rust's `Type::is_subtype`). Never is
/// below everything, True and False below Bool, sets are covariant, a closed record is below the
/// open one with the same attributes. Records otherwise need the same attributes (depth
/// subtyping), except that permissive mode allows width subtyping into an open record. Strict mode
/// relates entity types only to themselves and an attribute's optionality only to itself;
/// permissive mode orders entity type unions by inclusion below any entity, and required below
/// optional.
[[nodiscard]] auto IsSubtype(const Type& subtype, const Type& supertype, ValidationMode mode) -> bool;

/// The least type both are subtypes of (Rust's `Type::least_upper_bound`), or why there is none.
/// Two types are compatible (the strict checks of `==`, `contains`, set literals and conditionals)
/// when this succeeds.
[[nodiscard]] auto LeastUpperBound(const Type& left, const Type& right, ValidationMode mode)
    -> std::expected<Type, LubHelp>;

/// The least upper bound of all of `types`, folding from Never: Never when `types` is empty (Rust's
/// `Type::reduce_to_least_upper_bound`).
[[nodiscard]] auto ReduceToLeastUpperBound(std::span<const Type> types, ValidationMode mode)
    -> std::expected<Type, LubHelp>;

/// True only when no value has both types; conservative: only entity type unions with no common
/// type are known disjoint (Rust's `Type::are_types_disjoint`).
[[nodiscard]] auto AreTypesDisjoint(const Type& left, const Type& right) noexcept -> bool;

// ---- Attributes and entity types ----

/// The entity type union of `type`, or null when it is not a named entity type (any entity
/// included).
[[nodiscard]] inline auto AsEntityLub(const Type& type) noexcept -> const EntityLub*
{
    const auto* entity = std::get_if<types::Entity>(&type.kind);
    return entity != nullptr ? &entity->lub : nullptr;
}

/// AsEntityLub of `type`, or of its element type when it is a set with a known element type
/// (Rust's `Type::as_set_or_entity_lub`). Null for any entity, a set of any entity, any set and
/// every other type.
[[nodiscard]] auto AsSetOrEntityLub(const Type& type) noexcept -> const EntityLub*;

/// Whether every entity type a value of `type` could have is declared as an entity type in
/// `schema` (Rust's `Type::has_declared_entity_types`). True for every type but a named entity
/// type union. False for an action type, which is declared as an action rather than as an entity
/// type, so MayHaveAttribute and MayHaveTags report it as having no attributes and no tags.
[[nodiscard]] auto HasDeclaredEntityTypes(const ValidatorSchema& schema, const Type& type) noexcept -> bool;

/// Whether a value of `type` could have any tags according to `schema` (Rust's
/// `Type::may_have_tags`). False only for a named entity type union none of whose types declares
/// tags; an undeclared type counts as declaring none, so a caller that must not rely on that also
/// checks HasDeclaredEntityTypes. True for every other type, any entity included.
[[nodiscard]] auto MayHaveTags(const ValidatorSchema& schema, const Type& type) noexcept -> bool;

/// The attributes an entity of one of `lub`'s types has: a single type's from the schema (none
/// when the schema does not declare it, as for action types), several types' as their permissive
/// least upper bound, keeping the attributes all of them have whose types have one.
[[nodiscard]] auto EntityAttributeTypes(const EntityLub& lub, const ValidatorSchema& schema) -> Attributes;

/// The type of attribute `name` of a record or entity type, with its optionality; nothing when it
/// is known not to exist, when it may exist with any type (any entity, an unlisted attribute of an
/// open record), or when `type` is neither (Rust's `Type::lookup_attribute_type`).
[[nodiscard]] auto LookupAttributeType(const ValidatorSchema& schema, const Type& type, std::string_view name)
    -> std::optional<AttributeType>;

/// The attributes a record or entity type is known to list; none for any entity and for other
/// types (Rust's `Type::all_attributes`).
[[nodiscard]] auto AllAttributes(const ValidatorSchema& schema, const Type& type) -> Attributes;

/// Whether a value of `type` could have attribute `name` (Rust's `Type::may_have_attr`): Never,
/// any entity, an open record and an entity type the schema does not declare could have any; a
/// closed record and a declared entity type only the ones listed; an action type and other types
/// none.
[[nodiscard]] auto MayHaveAttribute(const ValidatorSchema& schema, const Type& type, std::string_view name) -> bool;

// ---- Printing ----

/// `type` as a schema writes it: `Bool`, `Long`, `String`, `Set<T>`, `{a: T, "b c"?: U}`, an entity
/// type's name, an extension type's name. The typechecker's own types print as `Never`, `True`,
/// `False`, `Entity` (any entity), `A | B` (an entity type union), `Set<_>` (any set) and `{a: T, ..}`
/// (an open record). For messages.
[[nodiscard]] auto ToString(const Type& type) -> std::string;

/// Appends ToString(type) to `out`.
void AppendTypeText(std::string& out, const Type& type);

} // namespace cedar::detail
