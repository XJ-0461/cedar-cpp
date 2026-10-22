#include <cedar/Names.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace
{

using cedar::CheckIdentifier;
using cedar::CheckName;
using cedar::EntityType;
using cedar::EntityUid;
using cedar::Identifier;
using cedar::IsIdentifierSyntax;
using cedar::Name;
using cedar::NameError;
using cedar::Symbol;

static_assert(IsIdentifierSyntax("_"));
static_assert(IsIdentifierSyntax("a_9Z"));
static_assert(IsIdentifierSyntax("if"));
static_assert(!IsIdentifierSyntax(""));
static_assert(!IsIdentifierSyntax("9a"));
static_assert(!IsIdentifierSyntax("a-b"));
static_assert(!IsIdentifierSyntax("a b"));

static_assert(CheckIdentifier("principal").has_value());
static_assert(CheckIdentifier("permit").has_value());
static_assert(CheckIdentifier("__cedar").has_value());
static_assert(CheckIdentifier("has").error() == NameError::ReservedKeyword);
static_assert(CheckIdentifier("then").error() == NameError::ReservedKeyword);
static_assert(CheckIdentifier("é").error() == NameError::InvalidSyntax);

static_assert(CheckName("A::B::C").has_value());
static_assert(CheckName("___cedar::A").has_value());
static_assert(CheckName("__cedar::Long").error() == NameError::ReservedNamespace);
static_assert(CheckName("A::__cedar").error() == NameError::ReservedNamespace);
static_assert(CheckName("__cedar::Long", true).has_value());
static_assert(CheckName("A::in::B", true).error() == NameError::ReservedKeyword);
static_assert(CheckName("A :: B").error() == NameError::InvalidSyntax);
static_assert(CheckName("A::").error() == NameError::InvalidSyntax);
static_assert(CheckName("::A").error() == NameError::InvalidSyntax);
static_assert(CheckName("A:::B").error() == NameError::InvalidSyntax);

TEST(NamesTests, InterningIsIdentity)
{
    const Symbol first = Symbol::Intern("NamesTests_Interning");
    const std::string copy{"NamesTests_Interning"};
    const Symbol second = Symbol::Intern(copy);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.Text().data(), second.Text().data());
    EXPECT_EQ(first.Text(), "NamesTests_Interning");
    EXPECT_EQ(Symbol::Find("NamesTests_Interning"), first);
    EXPECT_EQ(Symbol::Find("NamesTests_NeverInterned"), std::nullopt);
    EXPECT_EQ(Symbol{}.Text(), "");
    EXPECT_EQ(Symbol::Intern(""), Symbol{});
}

TEST(NamesTests, ConcurrentInterningGivesOneSymbolPerString)
{
    // Enough distinct strings to force several index growths and segment allocations while the
    // threads race.
    constexpr std::size_t StringCount = 20000;
    constexpr std::size_t ThreadCount = 8;
    std::vector<std::string> texts;
    texts.reserve(StringCount);
    for (std::size_t i = 0; i < StringCount; ++i) { texts.push_back("NamesTests_Concurrent_" + std::to_string(i)); }

    std::vector<std::vector<Symbol>> results(ThreadCount, std::vector<Symbol>(StringCount));
    std::vector<std::jthread> threads;
    for (std::size_t t = 0; t < ThreadCount; ++t)
    {
        threads.emplace_back([&, t] {
            // Each thread starts at a different offset; odd threads walk backward.
            for (std::size_t k = 0; k < StringCount; ++k)
            {
                const std::size_t step = t % 2 == 0 ? k : StringCount - 1 - k;
                const std::size_t i = (step + t * 2503) % StringCount;
                results[t][i] = Symbol::Intern(texts[i]);
            }
        });
    }
    threads.clear();

    std::unordered_set<std::uint32_t> distinct;
    for (std::size_t i = 0; i < StringCount; ++i)
    {
        for (std::size_t t = 1; t < ThreadCount; ++t) { ASSERT_EQ(results[t][i], results[0][i]) << texts[i]; }
        ASSERT_EQ(results[0][i].Text(), texts[i]);
        distinct.insert(results[0][i].Index());
    }
    EXPECT_EQ(distinct.size(), StringCount);
}

TEST(NamesTests, NameParts)
{
    const Name name = *Name::Parse("A::B::C");
    EXPECT_EQ(name.Basename().Text(), "C");
    EXPECT_EQ(name.NamespaceText(), "A::B");
    EXPECT_FALSE(name.IsUnqualified());
    const auto components = name.NamespaceComponents();
    ASSERT_EQ(components.size(), 2U);
    EXPECT_EQ(components[0].Text(), "A");
    EXPECT_EQ(components[1].Text(), "B");
    EXPECT_EQ(Name::Qualified(components, name.Basename()), name);

    const Name unqualified = *Name::Parse("C");
    EXPECT_TRUE(unqualified.IsUnqualified());
    EXPECT_EQ(unqualified.NamespaceText(), "");
    EXPECT_EQ(unqualified.QualifyWith(*Name::Parse("A::B")), name);
    EXPECT_EQ(name.QualifyWith(*Name::Parse("Z")), name);

    EXPECT_FALSE(Name::Parse("__cedar::Long").has_value());
    EXPECT_TRUE(Name::ParseInternal("__cedar::Long")->IsReserved());
    EXPECT_TRUE(EntityType::Parse("NS::Action")->IsAction());
    EXPECT_FALSE(EntityType::Parse("Actions")->IsAction());
}

auto Uid(const std::string_view type, const std::string_view id) -> EntityUid
{
    return *EntityUid::Create(type, id);
}

TEST(NamesTests, EntityUidOrderMatchesReference)
{
    // The order and printing the reference CLI gives for
    // `[A0::X::"x", A::X::"x", Z::A::"x", X::"x", B::"b", B::"a", A::X::"\u{301}é\""]`:
    // basename first, then the namespace path component by component, then the id byte-wise.
    std::vector<EntityUid> uids{
        Uid("A0::X", "x"), Uid("A::X", "x"), Uid("Z::A", "x"), Uid("X", "x"),
        Uid("B", "b"),     Uid("B", "a"),    Uid("A::X", "́é\""),
    };
    std::ranges::sort(uids);
    std::string printed;
    for (const EntityUid& uid : uids) { printed += uid.ToString() + ", "; }
    EXPECT_EQ(printed, R"(Z::A::"x", B::"a", B::"b", X::"x", A::X::"x", A::X::"\u{301}é\"", A0::X::"x", )");
}

TEST(NamesTests, EntityUidEqualityAndHashAgree)
{
    const EntityUid left = Uid("A::User", "alice");
    const EntityUid right{EntityType{*Name::Parse("A::User")}, cedar::Eid{std::string{"alice"}}};
    EXPECT_EQ(left, right);
    EXPECT_EQ(left.Hash(), right.Hash());
    EXPECT_EQ(std::hash<EntityUid>{}(left), std::hash<EntityUid>{}(right));
    EXPECT_NE(left, Uid("A::User", "bob"));
    EXPECT_NE(left, Uid("B::User", "alice"));
    EXPECT_EQ(left <=> right, std::strong_ordering::equal);
}

TEST(NamesTests, EscapesLikeRust)
{
    // Expected strings are Rust 1.98.1 `str::escape_debug` output.
    EXPECT_EQ(cedar::Eid{"a\"b'c\\"}.Escaped(), R"(a\"b\'c\\)");
    EXPECT_EQ(cedar::Eid{"́x"}.Escaped(), R"(\u{301}x)");
    EXPECT_EQ(cedar::Eid{"x́"}.Escaped(), "x́");
    const std::string controls{"\x7f\0\t\r\n", 5};
    EXPECT_EQ(cedar::Eid{controls}.Escaped(), R"(\u{7f}\0\t\r\n)");
    EXPECT_EQ(cedar::Eid{"é日本"}.Escaped(), "é日本");
    EXPECT_EQ(cedar::Eid{"​"}.Escaped(), R"(\u{200b})");
    EXPECT_EQ(cedar::Eid{"\U0010FFFF"}.Escaped(), R"(\u{10ffff})");
    EXPECT_EQ(cedar::Eid{"\U0001F600"}.Escaped(), "\U0001F600");
}

TEST(NamesTests, FindNeverInterns)
{
    EXPECT_EQ(Name::Find("NamesTests_FindNs::Missing"), std::nullopt);
    EXPECT_EQ(Symbol::Find("NamesTests_FindNs::Missing"), std::nullopt);
    EXPECT_EQ(Symbol::Find("NamesTests_FindNs"), std::nullopt);
    EXPECT_EQ(EntityType::Find("NamesTests_FindAlone"), std::nullopt);
    EXPECT_EQ(Symbol::Find("NamesTests_FindAlone"), std::nullopt);

    const Name made = Name::Parse("NamesTests_FindNs::Present").value();
    EXPECT_EQ(Name::Find("NamesTests_FindNs::Present"), made);
    EXPECT_EQ(Name::Find("NamesTests_FindNs::Present")->Basename().Text(), "Present");
    EXPECT_EQ(EntityType::Find("NamesTests_FindNs::Present"), EntityType{made});
    EXPECT_EQ(Name::Find("Present"), Name::Unqualified(made.Basename()));

    // Invalid or reserved text is never found, whatever is interned.
    EXPECT_EQ(Name::Find("NamesTests_FindNs::"), std::nullopt);
    EXPECT_EQ(Name::Find("if"), std::nullopt);
    ASSERT_TRUE(Name::ParseInternal("__cedar::Long").has_value());
    EXPECT_EQ(Name::Find("__cedar::Long"), std::nullopt);
    // A symbol interned as plain text is not a name unless its basename is a symbol too.
    const Symbol plain = Symbol::Intern("NamesTests_PlainNs::NamesTests_PlainBase");
    EXPECT_EQ(Name::Find(plain.Text()), std::nullopt);
}

TEST(NamesTests, SlotIds)
{
    EXPECT_EQ(cedar::ToString(cedar::SlotId::Principal), "?principal");
    EXPECT_EQ(cedar::ToString(cedar::SlotId::Resource), "?resource");
    EXPECT_LT(cedar::SlotId::Principal, cedar::SlotId::Resource);
}

} // namespace
