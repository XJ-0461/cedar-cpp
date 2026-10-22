#include "validation/Types.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "schema/ValidatorSchema.hpp"

#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Validator.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <functional>
#include <iterator>
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

namespace
{

[[nodiscard]] auto SameType(const SharedType& left, const SharedType& right) -> bool
{
    if (left == right) { return true; }
    if (!left || !right) { return false; }
    return *left == *right;
}

} // namespace

auto operator==(const AttributeType& left, const AttributeType& right) -> bool
{
    return left.required == right.required && SameType(left.type, right.type);
}

Attributes::Attributes(std::vector<Attribute> attributes)
{
    assert(std::ranges::adjacent_find(attributes, std::greater_equal<>{}, &Attribute::name) == attributes.end());
    if (!attributes.empty()) { entries_ = std::make_shared<const std::vector<Attribute>>(std::move(attributes)); }
}

auto Attributes::Find(const std::string_view name) const noexcept -> const AttributeType*
{
    const std::span<const Attribute> entries = Entries();
    const auto position = std::ranges::lower_bound(entries, name, std::less<>{}, &Attribute::name);
    return position != entries.end() && position->name == name ? &position->type : nullptr;
}

auto operator==(const Attributes& left, const Attributes& right) -> bool
{
    return left.entries_ == right.entries_ || std::ranges::equal(left.Entries(), right.Entries());
}

EntityLub::EntityLub(std::vector<EntityType> types) : first_{types.front()}
{
    assert(!types.empty());
    assert(std::ranges::adjacent_find(types, std::greater_equal<>{}) == types.end());
    if (types.size() > 1) { several_ = std::make_shared<const std::vector<EntityType>>(std::move(types)); }
}

auto EntityLub::Contains(const EntityType& type) const noexcept -> bool
{
    return std::ranges::binary_search(Types(), type);
}

auto EntityLub::IsSubsetOf(const EntityLub& other) const noexcept -> bool
{
    return std::ranges::includes(other.Types(), Types());
}

auto EntityLub::IsDisjointFrom(const EntityLub& other) const noexcept -> bool
{
    return std::ranges::none_of(Types(), [&](const EntityType& type) { return other.Contains(type); });
}

auto EntityLub::Union(const EntityLub& other) const -> EntityLub
{
    if (*this == other) { return *this; }
    std::vector<EntityType> types;
    types.reserve(Types().size() + other.Types().size());
    std::ranges::set_union(Types(), other.Types(), std::back_inserter(types));
    return EntityLub{std::move(types)};
}

auto operator==(const EntityLub& left, const EntityLub& right) noexcept -> bool
{
    return left.several_ == right.several_ ? left.first_ == right.first_
                                           : std::ranges::equal(left.Types(), right.Types());
}

namespace types
{

auto operator==(const Set& left, const Set& right) -> bool { return SameType(left.element, right.element); }

} // namespace types

auto MakeShared(Type type) -> SharedType { return std::make_shared<const Type>(std::move(type)); }

auto EmptyRecord() -> Type { return Type{types::Record{}}; }

auto EntityUidLiteralType(const EntityUid& uid, const ValidatorSchema& schema) -> std::optional<Type>
{
    const bool declared = uid.IsAction() ? schema.FindAction(uid) != nullptr
                                         : schema.FindEntityType(uid.Type()) != nullptr;
    if (!declared) { return std::nullopt; }
    return NamedEntityType(uid.Type());
}

auto LubHelpText(const LubHelp help) noexcept -> std::string_view
{
    switch (help)
    {
    case LubHelp::AttributeQualifier:
        return "corresponding attributes of compatible record types must both be required or both be optional";
    case LubHelp::RecordWidth: return "compatible record types must have exactly the same attributes";
    case LubHelp::EntityType:
        return "different entity types are never compatible, even when their attributes would be";
    case LubHelp::EntityRecord:
        return "entity and record types are never compatible, even when their attributes would be";
    case LubHelp::Mismatch: break;
    }
    return "types must be exactly equal to be compatible";
}

namespace
{

[[nodiscard]] auto IsStrict(const ValidationMode mode) noexcept -> bool { return mode == ValidationMode::Strict; }

// Rust `AttributeType::is_subtype` (types.rs:1337).
[[nodiscard]] auto IsAttributeSubtype(const AttributeType& subtype, const AttributeType& supertype,
                                      const ValidationMode mode) -> bool
{
    const bool qualifier = IsStrict(mode) ? subtype.required == supertype.required
                                          : subtype.required || !supertype.required;
    return qualifier && IsSubtype(*subtype.type, *supertype.type, mode);
}

// Every attribute of `supertype` is in `subtype` with a subtype (Rust `Attributes::is_subtype`).
[[nodiscard]] auto AreAttributesSubtype(const Attributes& subtype, const Attributes& supertype,
                                        const ValidationMode mode) -> bool
{
    return std::ranges::all_of(supertype,
                               [&](const Attribute& attribute)
                               {
                                   const AttributeType* found = subtype.Find(attribute.name);
                                   return found != nullptr && IsAttributeSubtype(*found, attribute.type, mode);
                               });
}

// Rust `AttributeType::least_upper_bound` (types.rs:1321): the type's failure comes first.
[[nodiscard]] auto AttributeLeastUpperBound(const AttributeType& left, const AttributeType& right,
                                            const ValidationMode mode) -> std::expected<AttributeType, LubHelp>
{
    std::expected<Type, LubHelp> type = LeastUpperBound(*left.type, *right.type, mode);
    if (!type) { return std::unexpected{type.error()}; }
    if (IsStrict(mode) && left.required != right.required) { return std::unexpected{LubHelp::AttributeQualifier}; }
    SharedType shared = *type == *left.type ? left.type : *type == *right.type ? right.type : MakeShared(*std::move(type));
    return AttributeType{std::move(shared), left.required && right.required};
}

// Strict: the same names, each pair with a least upper bound (Rust `strict_least_upper_bound`).
[[nodiscard]] auto StrictAttributesLeastUpperBound(const Attributes& left, const Attributes& right)
    -> std::expected<Attributes, LubHelp>
{
    if (left.Size() != right.Size()) { return std::unexpected{LubHelp::RecordWidth}; }
    std::vector<Attribute> result;
    result.reserve(left.Size());
    for (const Attribute& attribute : left)
    {
        const AttributeType* other = right.Find(attribute.name);
        if (other == nullptr) { return std::unexpected{LubHelp::RecordWidth}; }
        std::expected<AttributeType, LubHelp> lub = AttributeLeastUpperBound(attribute.type, *other, ValidationMode::Strict);
        if (!lub) { return std::unexpected{lub.error()}; }
        result.push_back(Attribute{attribute.name, *std::move(lub)});
    }
    return Attributes{std::move(result)};
}

// Permissive: the names both have whose types have a least upper bound (Rust
// `permissive_least_upper_bound`); never fails.
[[nodiscard]] auto PermissiveAttributesLeastUpperBound(const Attributes& left, const Attributes& right) -> Attributes
{
    if (left == right) { return left; }
    std::vector<Attribute> result;
    for (const Attribute& attribute : left)
    {
        const AttributeType* other = right.Find(attribute.name);
        if (other == nullptr) { continue; }
        std::expected<AttributeType, LubHelp> lub =
            AttributeLeastUpperBound(attribute.type, *other, ValidationMode::Permissive);
        if (lub) { result.push_back(Attribute{attribute.name, *std::move(lub)}); }
    }
    return Attributes{std::move(result)};
}

// The number of names in either of two sorted attribute lists.
[[nodiscard]] auto UnionSize(const Attributes& left, const Attributes& right) noexcept -> std::size_t
{
    std::size_t common = 0;
    auto leftPosition = left.begin();
    auto rightPosition = right.begin();
    while (leftPosition != left.end() && rightPosition != right.end())
    {
        if (leftPosition->name < rightPosition->name) { ++leftPosition; }
        else if (rightPosition->name < leftPosition->name) { ++rightPosition; }
        else
        {
            ++common;
            ++leftPosition;
            ++rightPosition;
        }
    }
    return left.Size() + right.Size() - common;
}

// Rust `Type::is_subtype`, record arm (types.rs:239).
[[nodiscard]] auto IsRecordSubtype(const types::Record& subtype, const types::Record& supertype,
                                   const ValidationMode mode) -> bool
{
    const bool subtypeOpen = subtype.open == OpenTag::OpenAttributes;
    const bool supertypeOpen = supertype.open == OpenTag::OpenAttributes;
    if (subtypeOpen && !supertypeOpen) { return false; }
    if (supertypeOpen && !IsStrict(mode)) { return AreAttributesSubtype(subtype.attributes, supertype.attributes, mode); }
    return subtype.attributes.Size() == supertype.attributes.Size() &&
           AreAttributesSubtype(subtype.attributes, supertype.attributes, mode);
}

// Rust `Type::least_upper_bound`, record arm (types.rs:309).
[[nodiscard]] auto RecordLeastUpperBound(const types::Record& left, const types::Record& right,
                                         const ValidationMode mode) -> std::expected<Type, LubHelp>
{
    Attributes attributes;
    if (IsStrict(mode))
    {
        std::expected<Attributes, LubHelp> strict = StrictAttributesLeastUpperBound(left.attributes, right.attributes);
        if (!strict) { return std::unexpected{strict.error()}; }
        attributes = *std::move(strict);
    }
    else { attributes = PermissiveAttributesLeastUpperBound(left.attributes, right.attributes); }
    // In strict mode a least upper bound keeps every name; in permissive mode one that drops a name
    // describes records that may have it, so it is open.
    const bool open = left.open == OpenTag::OpenAttributes || right.open == OpenTag::OpenAttributes ||
                      (!IsStrict(mode) && attributes.Size() != UnionSize(left.attributes, right.attributes));
    return Type{types::Record{std::move(attributes), open ? OpenTag::OpenAttributes : OpenTag::ClosedAttributes}};
}

// Whether an entity of one of `lub`'s types may have attributes it does not list (Rust
// `EntityKind::has_open_attributes_record`): an undeclared non-action type may.
[[nodiscard]] auto HasOpenAttributes(const EntityLub& lub, const ValidatorSchema& schema) -> bool
{
    return std::ranges::any_of(lub.Types(),
                               [&](const EntityType& type)
                               {
                                   if (type.IsAction()) { return false; }
                                   const ValidatorEntityType* declared = schema.FindEntityType(type);
                                   return declared == nullptr || declared->openAttributes == OpenTag::OpenAttributes;
                               });
}

[[nodiscard]] auto DeclaredAttributes(const EntityType& type, const ValidatorSchema& schema) -> Attributes
{
    const ValidatorEntityType* declared = schema.FindEntityType(type);
    return declared != nullptr ? declared->attributes : Attributes{};
}

} // namespace

auto IsSubtype(const Type& subtype, const Type& supertype, const ValidationMode mode) -> bool
{
    if (std::holds_alternative<types::Never>(subtype.kind)) { return true; }
    return std::visit(
        Overloaded{
            [](const types::Boolean& left, const types::Boolean& right)
            { return left.kind == right.kind || right.kind == BooleanType::AnyBoolean; },
            [](const types::Long&, const types::Long&) { return true; },
            [](const types::String&, const types::String&) { return true; },
            [&](const types::Set& left, const types::Set& right)
            {
                if (!right.element) { return true; }
                if (!left.element) { return false; }
                return IsSubtype(*left.element, *right.element, mode);
            },
            [&](const types::Record& left, const types::Record& right) { return IsRecordSubtype(left, right, mode); },
            [&](const types::Entity& left, const types::Entity& right)
            { return IsStrict(mode) ? left.lub == right.lub : left.lub.IsSubsetOf(right.lub); },
            [&](const types::Entity&, const types::AnyEntity&) { return !IsStrict(mode); },
            [](const types::AnyEntity&, const types::AnyEntity&) { return true; },
            [](const types::Extension& left, const types::Extension& right) { return left.kind == right.kind; },
            [](const auto&, const auto&) { return false; },
        },
        subtype.kind, supertype.kind);
}

auto LeastUpperBound(const Type& left, const Type& right, const ValidationMode mode) -> std::expected<Type, LubHelp>
{
    if (IsSubtype(left, right, mode)) { return right; }
    if (IsSubtype(right, left, mode)) { return left; }
    return std::visit(
        Overloaded{
            [](const types::Boolean&, const types::Boolean&) -> std::expected<Type, LubHelp>
            { return AnyBooleanType(); },
            [&](const types::Set& leftSet, const types::Set& rightSet) -> std::expected<Type, LubHelp>
            {
                // An element-less set is above every set, so the subtype checks above already
                // answered; kept for completeness as Rust does.
                if (!leftSet.element || !rightSet.element) { return AnySetType(); }
                std::expected<Type, LubHelp> element = LeastUpperBound(*leftSet.element, *rightSet.element, mode);
                if (!element) { return std::unexpected{element.error()}; }
                return SetType(MakeShared(*std::move(element)));
            },
            [&](const types::Record& leftRecord, const types::Record& rightRecord) -> std::expected<Type, LubHelp>
            { return RecordLeastUpperBound(leftRecord, rightRecord, mode); },
            [&](const types::Entity& leftEntity, const types::Entity& rightEntity) -> std::expected<Type, LubHelp>
            {
                if (IsStrict(mode)) { return std::unexpected{LubHelp::EntityType}; }
                return Type{types::Entity{leftEntity.lub.Union(rightEntity.lub)}};
            },
            [&](const types::Entity&, const types::AnyEntity&) -> std::expected<Type, LubHelp>
            {
                if (IsStrict(mode)) { return std::unexpected{LubHelp::EntityType}; }
                return AnyEntityType();
            },
            [&](const types::AnyEntity&, const types::Entity&) -> std::expected<Type, LubHelp>
            {
                if (IsStrict(mode)) { return std::unexpected{LubHelp::EntityType}; }
                return AnyEntityType();
            },
            [](const types::Entity&, const types::Record&) -> std::expected<Type, LubHelp>
            { return std::unexpected{LubHelp::EntityRecord}; },
            [](const types::AnyEntity&, const types::Record&) -> std::expected<Type, LubHelp>
            { return std::unexpected{LubHelp::EntityRecord}; },
            [](const types::Record&, const types::Entity&) -> std::expected<Type, LubHelp>
            { return std::unexpected{LubHelp::EntityRecord}; },
            [](const types::Record&, const types::AnyEntity&) -> std::expected<Type, LubHelp>
            { return std::unexpected{LubHelp::EntityRecord}; },
            [](const auto&, const auto&) -> std::expected<Type, LubHelp> { return std::unexpected{LubHelp::Mismatch}; },
        },
        left.kind, right.kind);
}

auto ReduceToLeastUpperBound(const std::span<const Type> types, const ValidationMode mode)
    -> std::expected<Type, LubHelp>
{
    Type result = NeverType();
    for (const Type& type : types)
    {
        std::expected<Type, LubHelp> next = LeastUpperBound(result, type, mode);
        if (!next) { return next; }
        result = *std::move(next);
    }
    return result;
}

auto AreTypesDisjoint(const Type& left, const Type& right) noexcept -> bool
{
    const EntityLub* leftLub = AsEntityLub(left);
    const EntityLub* rightLub = AsEntityLub(right);
    return leftLub != nullptr && rightLub != nullptr && leftLub->IsDisjointFrom(*rightLub);
}

auto AsSetOrEntityLub(const Type& type) noexcept -> const EntityLub*
{
    if (const auto* set = std::get_if<types::Set>(&type.kind); set != nullptr && set->element)
    {
        return AsEntityLub(*set->element);
    }
    return AsEntityLub(type);
}

auto HasDeclaredEntityTypes(const ValidatorSchema& schema, const Type& type) noexcept -> bool
{
    const EntityLub* lub = AsEntityLub(type);
    return lub == nullptr || std::ranges::all_of(lub->Types(), [&](const EntityType& member)
                                                 { return schema.FindEntityType(member) != nullptr; });
}

auto MayHaveTags(const ValidatorSchema& schema, const Type& type) noexcept -> bool
{
    const EntityLub* lub = AsEntityLub(type);
    return lub == nullptr || std::ranges::any_of(lub->Types(),
                                                 [&](const EntityType& member)
                                                 {
                                                     const ValidatorEntityType* declared = schema.FindEntityType(member);
                                                     return declared != nullptr && declared->tags.has_value();
                                                 });
}

auto EntityAttributeTypes(const EntityLub& lub, const ValidatorSchema& schema) -> Attributes
{
    const std::span<const EntityType> members = lub.Types();
    Attributes result = DeclaredAttributes(members.front(), schema);
    for (const EntityType& member : members.subspan(1))
    {
        result = PermissiveAttributesLeastUpperBound(result, DeclaredAttributes(member, schema));
    }
    return result;
}

auto LookupAttributeType(const ValidatorSchema& schema, const Type& type, const std::string_view name)
    -> std::optional<AttributeType>
{
    if (const types::Record* record = AsRecord(type))
    {
        const AttributeType* found = record->attributes.Find(name);
        return found != nullptr ? std::optional<AttributeType>{*found} : std::nullopt;
    }
    if (const EntityLub* lub = AsEntityLub(type))
    {
        if (const EntityType* single = lub->Single())
        {
            const ValidatorEntityType* declared = schema.FindEntityType(*single);
            const AttributeType* found = declared != nullptr ? declared->attributes.Find(name) : nullptr;
            return found != nullptr ? std::optional<AttributeType>{*found} : std::nullopt;
        }
        const Attributes attributes = EntityAttributeTypes(*lub, schema);
        const AttributeType* found = attributes.Find(name);
        return found != nullptr ? std::optional<AttributeType>{*found} : std::nullopt;
    }
    return std::nullopt;
}

auto AllAttributes(const ValidatorSchema& schema, const Type& type) -> Attributes
{
    if (const types::Record* record = AsRecord(type)) { return record->attributes; }
    if (const EntityLub* lub = AsEntityLub(type)) { return EntityAttributeTypes(*lub, schema); }
    return Attributes{};
}

auto MayHaveAttribute(const ValidatorSchema& schema, const Type& type, const std::string_view name) -> bool
{
    return std::visit(
        Overloaded{
            [](const types::Never&) { return true; },
            [](const types::AnyEntity&) { return true; },
            [&](const types::Entity& entity)
            {
                if (HasOpenAttributes(entity.lub, schema)) { return true; }
                return std::ranges::any_of(entity.lub.Types(),
                                           [&](const EntityType& member)
                                           {
                                               const ValidatorEntityType* declared = schema.FindEntityType(member);
                                               return declared != nullptr && declared->attributes.Find(name) != nullptr;
                                           });
            },
            [&](const types::Record& record)
            { return record.open == OpenTag::OpenAttributes || record.attributes.Find(name) != nullptr; },
            [](const auto&) { return false; },
        },
        type.kind);
}

void AppendTypeText(std::string& out, const Type& type)
{
    std::visit(
        Overloaded{
            [&](const types::Never&) { out += "Never"; },
            [&](const types::Boolean& boolean)
            {
                out += boolean.kind == BooleanType::True    ? "True"
                       : boolean.kind == BooleanType::False ? "False"
                                                            : "Bool";
            },
            [&](const types::Long&) { out += "Long"; },
            [&](const types::String&) { out += "String"; },
            [&](const types::AnyEntity&) { out += "Entity"; },
            [&](const types::Entity& entity)
            {
                bool first = true;
                for (const EntityType& member : entity.lub.Types())
                {
                    if (!first) { out += " | "; }
                    first = false;
                    out += member.GetName().QualifiedText();
                }
            },
            [&](const types::Set& set)
            {
                out += "Set<";
                if (set.element) { AppendTypeText(out, *set.element); }
                else { out += "_"; }
                out += '>';
            },
            [&](const types::Record& record)
            {
                out += '{';
                bool first = true;
                for (const Attribute& attribute : record.attributes)
                {
                    if (!first) { out += ", "; }
                    first = false;
                    if (IsIdentifierSyntax(attribute.name)) { out += attribute.name; }
                    else
                    {
                        out += '"';
                        out += attribute.name;
                        out += '"';
                    }
                    if (!attribute.type.required) { out += '?'; }
                    out += ": ";
                    AppendTypeText(out, *attribute.type.type);
                }
                if (record.open == OpenTag::OpenAttributes) { out += first ? ".." : ", .."; }
                out += '}';
            },
            [&](const types::Extension& extension) { out += ExtensionTypeName(extension.kind); },
        },
        type.kind);
}

auto ToString(const Type& type) -> std::string
{
    std::string out;
    AppendTypeText(out, type);
    return out;
}

} // namespace cedar::detail
