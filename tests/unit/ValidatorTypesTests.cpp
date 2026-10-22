#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Subtyping and least upper bounds in both modes, read off cedar-policy-core/src/validator/types.rs
// (`is_subtype`, `least_upper_bound` and their test module), and the type and schema predicates of
// partial evaluation, read off `has_declared_entity_types`, `may_have_tags`,
// `as_set_or_entity_lub` (types.rs) and `any_descendent_of` (schema.rs).

namespace
{

using cedar::ValidationMode;
using cedar::detail::Attribute;
using cedar::detail::Attributes;
using cedar::detail::AttributeType;
using cedar::detail::EntityLub;
using cedar::detail::LubHelp;
using cedar::detail::MakeShared;
using cedar::detail::OpenTag;
using cedar::detail::Type;
namespace types = cedar::detail::types;

constexpr ValidationMode Strict = ValidationMode::Strict;
constexpr ValidationMode Permissive = ValidationMode::Permissive;

auto Name(const std::string_view text) -> cedar::EntityType { return cedar::EntityType::Parse(text).value(); }

auto Entity(const std::string_view text) -> Type { return cedar::detail::NamedEntityType(Name(text)); }

auto Union(const std::string_view first, const std::string_view second) -> Type
{
    return Type{types::Entity{EntityLub{Name(first)}.Union(EntityLub{Name(second)})}};
}

struct Field
{
    std::string name;
    Type type;
    bool required = true;
};

auto Record(std::vector<Field> fields, const OpenTag open = OpenTag::ClosedAttributes) -> Type
{
    std::vector<Attribute> attributes;
    for (Field& field : fields)
    {
        attributes.push_back(Attribute{std::move(field.name), AttributeType{MakeShared(std::move(field.type)), field.required}});
    }
    return Type{types::Record{Attributes{std::move(attributes)}, open}};
}

auto Set(Type element) -> Type { return cedar::detail::SetType(MakeShared(std::move(element))); }

auto True() -> Type { return cedar::detail::SingletonBooleanType(true); }
auto False() -> Type { return cedar::detail::SingletonBooleanType(false); }
auto Bool() -> Type { return cedar::detail::AnyBooleanType(); }
auto Long() -> Type { return cedar::detail::LongType(); }
auto String() -> Type { return cedar::detail::StringType(); }

struct Row
{
    const char* label;
    Type left;
    Type right;
    ValidationMode mode;
    bool leftIsSubtype;
    std::variant<std::string, LubHelp> leastUpperBound;
};

auto Rows() -> std::vector<Row>
{
    constexpr OpenTag Open = OpenTag::OpenAttributes;
    const Type ipaddr = cedar::detail::ExtensionType(cedar::ValueKind::IpAddress);
    const Type decimal = cedar::detail::ExtensionType(cedar::ValueKind::Decimal);
    return {
        {"never below long", cedar::detail::NeverType(), Long(), Strict, true, "Long"},
        {"true below bool", True(), Bool(), Strict, true, "Bool"},
        {"true and false", True(), False(), Strict, false, "Bool"},
        {"long and string", Long(), String(), Strict, false, LubHelp::Mismatch},
        {"sets of true and false", Set(True()), Set(False()), Strict, false, "Set<Bool>"},
        {"any set above a set", Set(Long()), cedar::detail::AnySetType(), Strict, true, "Set<_>"},
        {"sets of long and string", Set(Long()), Set(String()), Permissive, false, LubHelp::Mismatch},
        {"same extension", ipaddr, ipaddr, Strict, true, "ipaddr"},
        {"different extensions", ipaddr, decimal, Permissive, false, LubHelp::Mismatch},
        {"same entity type", Entity("A"), Entity("A"), Strict, true, "A"},
        {"entity types strict", Entity("A"), Entity("B"), Strict, false, LubHelp::EntityType},
        {"entity types permissive", Entity("B"), Entity("A"), Permissive, false, "A | B"},
        {"entity below union permissive", Entity("A"), Union("A", "B"), Permissive, true, "A | B"},
        {"entity and union strict", Entity("A"), Union("A", "B"), Strict, false, LubHelp::EntityType},
        {"entity and any entity strict", Entity("A"), cedar::detail::AnyEntityType(), Strict, false,
         LubHelp::EntityType},
        {"entity below any entity permissive", Entity("A"), cedar::detail::AnyEntityType(), Permissive, true,
         "Entity"},
        {"any entity above entity permissive", cedar::detail::AnyEntityType(), Entity("A"), Permissive, false,
         "Entity"},
        {"entity and record", Entity("A"), Record({}), Permissive, false, LubHelp::EntityRecord},
        {"closed below open", Record({}), Record({}, Open), Strict, true, "{..}"},
        {"open not below closed", Record({}, Open), Record({}), Strict, false, "{..}"},
        {"no width subtyping strict", Record({{"a", Long()}}), cedar::detail::AnyRecordType(), Strict, false,
         LubHelp::RecordWidth},
        {"width subtyping into open permissive", Record({{"a", Long()}}), cedar::detail::AnyRecordType(), Permissive,
         true, "{..}"},
        {"extra attribute strict", Record({{"a", Long()}}), Record({{"a", Long()}, {"b", String()}}), Strict, false,
         LubHelp::RecordWidth},
        {"extra attribute permissive", Record({{"a", Long()}}), Record({{"a", Long()}, {"b", String()}}), Permissive,
         false, "{a: Long, ..}"},
        {"optionality strict", Record({{"a", Long()}}), Record({{"a", Long(), false}}), Strict, false,
         LubHelp::AttributeQualifier},
        {"required below optional permissive", Record({{"a", Long()}}), Record({{"a", Long(), false}}), Permissive,
         true, "{a?: Long}"},
        {"optional not below required permissive", Record({{"a", Long(), false}}), Record({{"a", Long()}}), Permissive,
         false, "{a?: Long}"},
        {"attribute types strict", Record({{"bar", Long()}, {"foo", False()}}),
         Record({{"bar", Long()}, {"foo", String()}}), Strict, false, LubHelp::Mismatch},
        {"attribute types permissive", Record({{"bar", Long()}, {"foo", False()}}),
         Record({{"bar", Long()}, {"foo", String()}}), Permissive, false, "{bar: Long, ..}"},
        {"attribute depth stays closed", Record({{"a", True()}}), Record({{"a", False()}}), Strict, false,
         "{a: Bool}"},
        {"entity attribute permissive", Record({{"a", Entity("A")}}), Record({{"a", Entity("B")}}), Permissive, false,
         "{a: A | B}"},
    };
}

TEST(ValidatorTypes, SubtypeAndLeastUpperBoundTable)
{
    for (const Row& row : Rows())
    {
        SCOPED_TRACE(row.label);
        EXPECT_EQ(cedar::detail::IsSubtype(row.left, row.right, row.mode), row.leftIsSubtype);
        const std::expected<Type, LubHelp> forward = cedar::detail::LeastUpperBound(row.left, row.right, row.mode);
        const std::expected<Type, LubHelp> backward = cedar::detail::LeastUpperBound(row.right, row.left, row.mode);
        if (const auto* expected = std::get_if<std::string>(&row.leastUpperBound))
        {
            ASSERT_TRUE(forward.has_value());
            ASSERT_TRUE(backward.has_value());
            EXPECT_EQ(cedar::detail::ToString(*forward), *expected);
            EXPECT_EQ(*forward, *backward);
            EXPECT_TRUE(cedar::detail::IsSubtype(row.left, *forward, row.mode));
            EXPECT_TRUE(cedar::detail::IsSubtype(row.right, *forward, row.mode));
        }
        else
        {
            ASSERT_FALSE(forward.has_value());
            ASSERT_FALSE(backward.has_value());
            EXPECT_EQ(forward.error(), std::get<LubHelp>(row.leastUpperBound));
            EXPECT_EQ(backward.error(), std::get<LubHelp>(row.leastUpperBound));
        }
    }
}

TEST(ValidatorTypes, ReducesAnEmptyListToNever)
{
    const std::expected<Type, LubHelp> empty = cedar::detail::ReduceToLeastUpperBound({}, Strict);
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(cedar::detail::ToString(*empty), "Never");
    const std::vector<Type> three{True(), False(), Long()};
    const std::expected<Type, LubHelp> mixed = cedar::detail::ReduceToLeastUpperBound(three, Permissive);
    ASSERT_FALSE(mixed.has_value());
    EXPECT_EQ(mixed.error(), LubHelp::Mismatch);
}

TEST(ValidatorTypes, OnlyEntityTypesWithNothingInCommonAreDisjoint)
{
    EXPECT_TRUE(cedar::detail::AreTypesDisjoint(Entity("A"), Entity("B")));
    EXPECT_FALSE(cedar::detail::AreTypesDisjoint(Entity("A"), Union("A", "B")));
    EXPECT_FALSE(cedar::detail::AreTypesDisjoint(Entity("A"), cedar::detail::AnyEntityType()));
    EXPECT_FALSE(cedar::detail::AreTypesDisjoint(Long(), String()));
}

TEST(ValidatorTypes, LooksUpEntityAttributesInTheSchema)
{
    auto parsed = cedar::ParseSchema(R"(
        entity foo;
        entity bar;
        entity baz { a: Long, b: String, c: foo };
        entity buz { a: Long, b: Long, c: bar };
        action view;
    )");
    ASSERT_TRUE(parsed.has_value());
    const cedar::detail::ValidatorSchema& schema = cedar::detail::SchemaAccess::Validator(parsed->value);

    const Type both = Union("baz", "buz");
    EXPECT_EQ(cedar::detail::ToString(Record({})), "{}");
    const Attributes common = cedar::detail::AllAttributes(schema, both);
    EXPECT_EQ(cedar::detail::ToString(Type{types::Record{common}}), "{a: Long, c: bar | foo}");
    EXPECT_FALSE(cedar::detail::LookupAttributeType(schema, both, "b").has_value());
    EXPECT_TRUE(cedar::detail::MayHaveAttribute(schema, both, "b"));
    EXPECT_FALSE(cedar::detail::MayHaveAttribute(schema, both, "d"));

    const auto single = cedar::detail::LookupAttributeType(schema, Entity("baz"), "b");
    ASSERT_TRUE(single.has_value());
    EXPECT_TRUE(single->required);
    EXPECT_EQ(cedar::detail::ToString(*single->type), "String");

    EXPECT_FALSE(cedar::detail::MayHaveAttribute(schema, Entity("foo"), "a"));
    EXPECT_TRUE(cedar::detail::MayHaveAttribute(schema, Entity("undeclared"), "a"));
    EXPECT_FALSE(cedar::detail::MayHaveAttribute(schema, Entity("Action"), "a"));
    EXPECT_TRUE(cedar::detail::MayHaveAttribute(schema, cedar::detail::AnyEntityType(), "a"));
    EXPECT_TRUE(cedar::detail::MayHaveAttribute(schema, cedar::detail::NeverType(), "a"));
    EXPECT_TRUE(cedar::detail::MayHaveAttribute(schema, cedar::detail::AnyRecordType(), "a"));
    EXPECT_FALSE(cedar::detail::MayHaveAttribute(schema, Long(), "a"));

    const auto view = cedar::detail::EntityUidLiteralType(cedar::EntityUid::Create("Action", "view").value(), schema);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(cedar::detail::ToString(*view), "Action");
    EXPECT_FALSE(
        cedar::detail::EntityUidLiteralType(cedar::EntityUid::Create("Action", "edit").value(), schema).has_value());
    EXPECT_FALSE(
        cedar::detail::EntityUidLiteralType(cedar::EntityUid::Create("undeclared", "x").value(), schema).has_value());
}

// Entity type hierarchy User < Group < Org, Doc and Tagged with tags, an enumerated type, and an
// action hierarchy in which Action::"write" is in Other::Action::"act", so Action is below
// Other::Action but not the reverse.
constexpr std::string_view PredicateSchema = R"(
    entity Org;
    entity Group in [Org];
    entity User in [Group];
    entity Folder;
    entity Doc tags String;
    entity Tagged tags Long;
    entity Color enum ["red"];
    namespace Other { action act; }
    action read;
    action write in [read, Other::Action::"act"];
)";

auto PredicateValidatorSchema() -> const cedar::detail::ValidatorSchema&
{
    static const cedar::Schema schema = cedar::ParseSchema(PredicateSchema).value().value;
    return cedar::detail::SchemaAccess::Validator(schema);
}

auto PermissiveLub(const Type& left, const Type& right) -> Type
{
    return cedar::detail::LeastUpperBound(left, right, Permissive).value();
}

auto OpenRecord() -> Type { return Record({{"a", Long()}}, OpenTag::OpenAttributes); }

struct PredicateRow
{
    const char* label;
    Type type;
    bool expected;
};

TEST(ValidatorTypes, HasDeclaredEntityTypes)
{
    const std::vector<PredicateRow> rows{
        {"Long is not an entity", Long(), true},
        {"Never is not an entity", cedar::detail::NeverType(), true},
        {"any entity is not a named union", cedar::detail::AnyEntityType(), true},
        {"a set of an undeclared type is a set", Set(Entity("Undeclared")), true},
        {"closed record", Record({{"a", Long()}}), true},
        {"open record", OpenRecord(), true},
        {"declared type", Entity("User"), true},
        {"enumerated type", Entity("Color"), true},
        {"union of declared types", Union("User", "Doc"), true},
        {"permissive lub of declared types", PermissiveLub(Entity("Group"), Entity("Folder")), true},
        {"undeclared type", Entity("Undeclared"), false},
        {"union with an undeclared type", Union("User", "Undeclared"), false},
        {"permissive lub with an undeclared type", PermissiveLub(Entity("Undeclared"), Entity("Doc")), false},
        {"action type", Entity("Action"), false},
        {"union with an action type", Union("User", "Action"), false},
    };
    for (const PredicateRow& row : rows)
    {
        EXPECT_EQ(cedar::detail::HasDeclaredEntityTypes(PredicateValidatorSchema(), row.type), row.expected)
            << row.label;
    }
}

TEST(ValidatorTypes, MayHaveTags)
{
    const std::vector<PredicateRow> rows{
        {"Long is assumed to", Long(), true},
        {"String is assumed to", String(), true},
        {"Never is assumed to", cedar::detail::NeverType(), true},
        {"any entity is assumed to", cedar::detail::AnyEntityType(), true},
        {"a set of a tagless type is assumed to", Set(Entity("User")), true},
        {"closed record is assumed to", Record({}), true},
        {"open record is assumed to", OpenRecord(), true},
        {"type declaring tags", Entity("Doc"), true},
        {"type declaring other tags", Entity("Tagged"), true},
        {"type declaring no tags", Entity("User"), false},
        {"enumerated type", Entity("Color"), false},
        {"undeclared type counts as no tags", Entity("Undeclared"), false},
        {"action type", Entity("Action"), false},
        {"union with one tagged type", Union("User", "Doc"), true},
        {"union with an undeclared and a tagged type", Union("Undeclared", "Doc"), true},
        {"union of tagless types", Union("User", "Folder"), false},
        {"permissive lub with a tagged type", PermissiveLub(Entity("Folder"), Entity("Tagged")), true},
        {"permissive lub of tagless types", PermissiveLub(Entity("Org"), Entity("Color")), false},
    };
    for (const PredicateRow& row : rows)
    {
        EXPECT_EQ(cedar::detail::MayHaveTags(PredicateValidatorSchema(), row.type), row.expected) << row.label;
    }
}

TEST(ValidatorTypes, AsSetOrEntityLub)
{
    struct LubRow
    {
        const char* label;
        Type type;
        /// The entity union as ToString prints it; empty for none.
        std::string expected;
    };
    const std::vector<LubRow> rows{
        {"entity type", Entity("User"), "User"},
        {"entity union", Union("User", "Doc"), "Doc | User"},
        {"set of an entity type", Set(Entity("User")), "User"},
        {"set of an entity union", Set(Union("Group", "Doc")), "Doc | Group"},
        {"permissive lub of sets of entities", PermissiveLub(Set(Entity("User")), Set(Entity("Org"))), "Org | User"},
        {"any entity", cedar::detail::AnyEntityType(), ""},
        {"set of any entity", Set(cedar::detail::AnyEntityType()), ""},
        {"any set", cedar::detail::AnySetType(), ""},
        {"set of a set of entities", Set(Set(Entity("User"))), ""},
        {"set of Long", Set(Long()), ""},
        {"set of Never", Set(cedar::detail::NeverType()), ""},
        {"Long", Long(), ""},
        {"Never", cedar::detail::NeverType(), ""},
        {"closed record", Record({{"a", Entity("User")}}), ""},
        {"open record", OpenRecord(), ""},
    };
    for (const LubRow& row : rows)
    {
        const EntityLub* lub = cedar::detail::AsSetOrEntityLub(row.type);
        const std::string actual = lub != nullptr ? cedar::detail::ToString(Type{types::Entity{*lub}}) : "";
        EXPECT_EQ(actual, row.expected) << row.label;
    }
}

TEST(ValidatorTypes, AnyDescendantOf)
{
    struct DescendantRow
    {
        const char* label;
        Type left;
        Type right;
        bool expected;
    };
    const std::vector<DescendantRow> rows{
        {"a type is below itself", Entity("User"), Entity("User"), true},
        {"a type without parents is below itself", Entity("Folder"), Entity("Folder"), true},
        {"an undeclared type is below itself", Entity("Undeclared"), Entity("Undeclared"), true},
        {"declared parent", Entity("User"), Entity("Group"), true},
        {"transitive ancestor", Entity("User"), Entity("Org"), true},
        {"child is not above its parent", Entity("Group"), Entity("User"), false},
        {"unrelated types", Entity("Doc"), Entity("Folder"), false},
        {"undeclared below declared", Entity("Undeclared"), Entity("Group"), false},
        {"declared below undeclared", Entity("Group"), Entity("Undeclared"), false},
        {"action type below itself", Entity("Action"), Entity("Action"), true},
        {"action type below the type of an ancestor action", Entity("Action"), Entity("Other::Action"), true},
        {"action type above the type of a descendant action", Entity("Other::Action"), Entity("Action"), false},
        {"entity type below an action type", Entity("User"), Entity("Action"), false},
        {"action type below an entity type", Entity("Action"), Entity("Group"), false},
        {"left union, one member below", Union("Doc", "User"), Entity("Org"), true},
        {"right union, one member above", Entity("Group"), Union("Doc", "Org"), true},
        {"unions, some pair related", Union("Doc", "User"), Union("Folder", "Org"), true},
        {"unions, no pair related", Union("Doc", "Folder"), Union("Group", "User"), false},
        {"right is a set of the parent", Entity("User"), Set(Entity("Group")), true},
        {"right is a set of a child", Entity("Org"), Set(Entity("Group")), false},
        {"right is a permissive lub of sets", Entity("Group"),
         PermissiveLub(Set(Entity("Doc")), Set(Entity("Org"))), true},
        {"left is a permissive lub of entities", PermissiveLub(Entity("Doc"), Entity("Folder")), Entity("Org"), false},
    };
    for (const DescendantRow& row : rows)
    {
        const EntityLub* left = cedar::detail::AsEntityLub(row.left);
        const EntityLub* right = cedar::detail::AsSetOrEntityLub(row.right);
        ASSERT_NE(left, nullptr) << row.label;
        ASSERT_NE(right, nullptr) << row.label;
        EXPECT_EQ(PredicateValidatorSchema().AnyDescendantOf(*left, *right), row.expected) << row.label;
    }
}

} // namespace
