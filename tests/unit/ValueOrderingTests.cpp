#include <cedar/Request.hpp>
#include <cedar/Value.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <random>
#include <span>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using cedar::Decimal;
using cedar::Duration;
using cedar::EntityUid;
using cedar::IpAddress;
using cedar::OwnedValue;
using cedar::RecordEntry;
using cedar::ToCedar;
using cedar::Value;
using cedar::ValueArena;

constexpr auto V6(const std::array<std::uint16_t, 8> groups, const std::uint8_t prefixLength = 128) -> IpAddress
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    for (std::size_t index = 0; index < 4; ++index)
    {
        high = high << 16 | groups[index];
        low = low << 16 | groups[index + 4];
    }
    return IpAddress::V6(high, low, prefixLength);
}

constexpr auto V4(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const std::uint32_t d,
                  const std::uint8_t prefixLength = 32) -> IpAddress
{
    return IpAddress::V4(a << 24 | b << 16 | c << 8 | d, prefixLength);
}

template <typename T>
constexpr auto Canonical(const T& value) -> std::string
{
    std::string output;
    cedar::AppendCanonicalString(output, value);
    return output;
}

// Expected text from Rust's `Display for std::net::IpAddr` (rustc 1.98.1), plus the prefix.
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0, 0, 0}, 0)) == "::/0");
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0, 0, 1})) == "::1/128");
static_assert(Canonical(V6({1, 0, 0, 0, 0, 0, 0, 0})) == "1::/128");
static_assert(Canonical(V6({1, 0, 0, 1, 0, 0, 0, 1})) == "1:0:0:1::1/128");
static_assert(Canonical(V6({1, 0, 0, 0, 1, 0, 0, 1})) == "1::1:0:0:1/128");
static_assert(Canonical(V6({1, 0, 1, 0, 1, 0, 1, 0})) == "1:0:1:0:1:0:1:0/128");
static_assert(Canonical(V6({0, 0, 1, 0, 0, 1, 0, 0})) == "::1:0:0:1:0:0/128");
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0xffff, 0xff00, 1})) == "::ffff:255.0.0.1/128");
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0xffff, 0, 0})) == "::ffff:0.0.0.0/128");
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0, 0x102, 0x304})) == "::102:304/128");
static_assert(Canonical(V6({0xffff, 0xabcd, 0, 0, 0, 0, 0, 1})) == "ffff:abcd::1/128");
static_assert(Canonical(V6({0, 1, 2, 3, 4, 5, 6, 7})) == "0:1:2:3:4:5:6:7/128");
static_assert(Canonical(V6({1, 2, 3, 4, 5, 6, 7, 0})) == "1:2:3:4:5:6:7:0/128");
static_assert(Canonical(V6({0, 0, 0, 0, 0, 0xff, 0xff00, 1})) == "::ff:ff00:1/128");
static_assert(Canonical(V6({0x64, 0xff9b, 0, 0, 0, 0, 0x102, 0x304})) == "64:ff9b::102:304/128");
static_assert(Canonical(V4(1, 2, 3, 4)) == "1.2.3.4/32");
static_assert(Canonical(V4(0, 0, 0, 0, 0)) == "0.0.0.0/0");

// Rust's `Display for Decimal` and `Display for Duration`.
static_assert(Canonical(Decimal{-5'000}) == "-0.5000");
static_assert(Canonical(Decimal{12'300}) == "1.2300");
static_assert(Canonical(Decimal{0}) == "0.0000");
static_assert(Canonical(Decimal{INT64_MIN}) == "-922337203685477.5808");
static_assert(Canonical(Duration{-1}) == "-1ms");

// Builds the table's values. Everything lives in one arena that outlives the test.
class Builder
{
public:
    auto Long(const std::int64_t value) -> Value { return Value::FromLong(value); }
    auto String(const std::string_view text) -> Value { return arena_.MakeString(text); }
    auto Entity(const std::string_view type, const std::string_view id) -> Value
    {
        return arena_.MakeEntity(EntityUid::Create(type, id).value());
    }
    auto Set(const std::initializer_list<Value> elements) -> Value { return arena_.MakeSet(std::span<const Value>{elements.begin(), elements.end()}); }
    auto Record(const std::initializer_list<RecordEntry> entries) -> Value
    {
        std::vector<RecordEntry> copied;
        for (const RecordEntry& entry : entries) { copied.push_back({arena_.CopyText(entry.key), entry.value}); }
        return arena_.MakeRecord(copied).value();
    }
    auto Arena() -> ValueArena& { return arena_; }

private:
    ValueArena arena_;
};

auto MakeDecimal(const std::int64_t scaled) -> Value { return Value::FromDecimal(Decimal{scaled}); }
auto MakeDatetime(const std::int64_t milliseconds) -> Value { return Value::FromDatetime(cedar::Datetime{milliseconds}); }
auto MakeDuration(const std::int64_t milliseconds) -> Value { return Value::FromDuration(Duration{milliseconds}); }
auto MakeIp(const IpAddress& address) -> Value { return Value::FromIpAddress(address); }

// The values of the oracle's set, in the oracle's order. The oracle is cedar-policy-core 4.14.0's
// own `Display for Value`, which iterates a set in its `BTreeSet<Value>` order; see
// /var/tmp/cedar-value-order-probe (the `cedar evaluate` CLI is not usable here, because it re-sorts
// through `EvalResult`, which orders extension values by their printed text).
auto OracleOrderedTable(Builder& b) -> std::vector<Value>
{
    return {
        Value::FromBool(false),
        Value::FromBool(true),
        b.Long(INT64_MIN),
        b.Long(-1),
        b.Long(0),
        b.Long(1),
        b.Long(INT64_MAX),
        b.String(""),
        b.String("\n"),
        b.String("Z"),
        b.String("a"),
        b.String("ab"),
        b.String("b"),
        b.String("é"),
        b.String("\U0001F600"),
        b.Entity("A", ""),
        b.Entity("A", "x"),
        b.Entity("A", "y"),
        b.Entity("A", "é"),
        b.Entity("Action", "view"),
        b.Entity("B", "a"),
        b.Entity("A::B", "a"),
        b.Set({}),
        b.Set({Value::FromBool(false)}),
        b.Set({b.Long(1)}),
        b.Set({b.Long(1), b.Long(2)}),
        b.Set({b.Long(2)}),
        b.Set({b.String("a")}),
        b.Set({b.Entity("A", "x")}),
        b.Set({b.Set({b.Long(1)})}),
        b.Set({b.Record({})}),
        b.Set({MakeDecimal(10'000), MakeIp(V6({0, 0, 0, 0, 0, 0, 0, 1}))}),
        b.Record({}),
        b.Record({{"Z", b.Long(1)}}),
        b.Record({{"__cedar", b.Long(1)}}),
        b.Record({{"a", Value::FromBool(true)}}),
        b.Record({{"a", b.Long(1)}}),
        b.Record({{"a", b.Long(1)}, {"b", b.Long(2)}}),
        b.Record({{"a", b.Entity("A", "x")}}),
        b.Record({{"a", b.Set({b.Long(1)})}}),
        b.Record({{"b", b.Long(0)}}),
        b.Record({{"b c", b.Long(1)}}),
        b.Record({{"if", b.Long(1)}}),
        b.Record({{"é", b.Long(1)}}),
        MakeDatetime(-86'400'000),
        MakeDatetime(0),
        MakeDatetime(1'704'067'200'000),
        MakeDecimal(INT64_MIN),
        MakeDecimal(-5'000),
        MakeDecimal(0),
        MakeDecimal(1),
        MakeDecimal(90'000),
        MakeDecimal(100'000),
        MakeDecimal(INT64_MAX),
        MakeDuration(-1),
        MakeDuration(0),
        MakeDuration(3'600'000),
        MakeIp(V4(0, 0, 0, 0, 0)),
        MakeIp(V4(9, 0, 0, 0)),
        MakeIp(V4(10, 0, 0, 0, 24)),
        MakeIp(V4(10, 0, 0, 1, 24)),
        MakeIp(V4(10, 0, 0, 1)),
        MakeIp(V4(255, 255, 255, 255)),
        MakeIp(V6({0, 0, 0, 0, 0, 0, 0, 0}, 0)),
        MakeIp(V6({0, 0, 0, 0, 0, 0, 0, 0})),
        MakeIp(V6({0, 0, 0, 0, 0, 0, 0, 1})),
        MakeIp(V6({1, 0, 0, 0, 0, 0, 0, 0}, 16)),
        MakeIp(V6({1, 0, 0, 1, 0, 0, 0, 1})),
        MakeIp(V6({0xfe80, 0, 0, 0, 0, 0, 0, 1}, 64)),
        MakeIp(V6({0xffff, 0, 0, 0, 0, 0, 0, 1})),
    };
}

// The oracle's printing of the same values written as one Cedar set literal (canonical constructor
// arguments, so Rust's echo of the written argument coincides with the canonical form).
constexpr std::string_view OraclePrinted =
    R"([false, true, -9223372036854775808, -1, 0, 1, 9223372036854775807, "", "\n", "Z", "a", "ab", "b", "é", )"
    R"(")" "\U0001F600" R"(", A::"", A::"x", A::"y", A::"é", Action::"view", B::"a", A::B::"a", [], [false], [1], [1, 2], [2], )"
    R"(["a"], [A::"x"], [[1]], [{}], [decimal("1.0000"), ip("::1/128")], {}, {Z: 1}, {"__cedar": 1}, )"
    R"({a: true}, {a: 1}, {a: 1, b: 2}, {a: A::"x"}, {a: [1]}, {b: 0}, {"b c": 1}, {"if": 1}, {"é": 1}, )"
    R"(datetime("1970-01-01").offset(duration("-86400000ms")), datetime("1970-01-01").offset(duration("0ms")), )"
    R"(datetime("1970-01-01").offset(duration("1704067200000ms")), decimal("-922337203685477.5808"), )"
    R"(decimal("-0.5000"), decimal("0.0000"), decimal("0.0001"), decimal("9.0000"), decimal("10.0000"), )"
    R"(decimal("922337203685477.5807"), duration("-1ms"), duration("0ms"), duration("3600000ms"), )"
    R"(ip("0.0.0.0/0"), ip("9.0.0.0/32"), ip("10.0.0.0/24"), ip("10.0.0.1/24"), ip("10.0.0.1/32"), )"
    R"(ip("255.255.255.255/32"), ip("::/0"), ip("::/128"), ip("::1/128"), ip("1::/16"), ip("1:0:0:1::1/128"), )"
    R"(ip("fe80::1/64"), ip("ffff::1/128")])";

TEST(ValueOrderingTests, SortReproducesTheOracleOrderAndPrinting)
{
    Builder builder;
    const std::vector<Value> expected = OracleOrderedTable(builder);

    for (const std::size_t i : std::views::iota(std::size_t{0}, expected.size()))
    {
        for (const std::size_t j : std::views::iota(std::size_t{0}, expected.size()))
        {
            EXPECT_EQ(Value::Compare(expected[i], expected[j]), i <=> j) << i << " vs " << j;
            EXPECT_EQ(expected[i] == expected[j], i == j) << i << " vs " << j;
        }
    }

    std::vector<Value> shuffled = expected;
    std::ranges::reverse(shuffled);
    std::mt19937 generator{20261022};
    std::ranges::shuffle(shuffled, generator);
    shuffled.insert(shuffled.end(), expected.begin(), expected.begin() + 10);

    const Value set = builder.Arena().MakeSet(shuffled);
    ASSERT_EQ(set.AsSet().Size(), expected.size());
    EXPECT_TRUE(std::ranges::equal(set.AsSet(), expected));
    EXPECT_EQ(ToCedar(set), OraclePrinted);
}

TEST(ValueOrderingTests, EqualityIsStructuralAndTotal)
{
    Builder b;
    // Same set regardless of insertion order and duplicates; entity uids in different storage.
    EXPECT_EQ(b.Set({b.Long(2), b.Long(1), b.Long(2)}), b.Set({b.Long(1), b.Long(2)}));
    EXPECT_EQ(b.Entity("A", "x"), b.Entity("A", "x"));
    EXPECT_EQ(b.Record({{"b", b.Long(1)}, {"a", b.String("x")}}), b.Record({{"a", b.String("x")}, {"b", b.Long(1)}}));
    EXPECT_NE(b.Record({{"a", b.Long(1)}}), b.Record({{"a", b.Long(1)}, {"b", b.Long(1)}}));
    // Different kinds are unequal, never an error.
    EXPECT_NE(b.Long(1), b.String("1"));
    EXPECT_NE(b.Long(1), MakeDecimal(10'000));
    EXPECT_NE(MakeDatetime(5), MakeDuration(5));
    EXPECT_NE(b.Set({}), b.Record({}));
    // The address is not masked by the prefix (Rust's derived `Eq` on `IPAddr`).
    EXPECT_NE(MakeIp(V4(10, 0, 0, 1, 24)), MakeIp(V4(10, 0, 0, 0, 24)));
    EXPECT_NE(MakeIp(V4(10, 0, 0, 1, 24)), MakeIp(V4(10, 0, 0, 1)));
    EXPECT_NE(MakeIp(V4(0, 0, 0, 1, 32)), MakeIp(V6({0, 0, 0, 0, 0, 0, 0, 1}, 32)));
}

TEST(ValueOrderingTests, SetMembershipAndRecordConstruction)
{
    Builder b;
    const Value set = b.Set({b.Long(3), b.String("a"), b.Long(1), b.Entity("A", "x")});
    EXPECT_TRUE(set.AsSet().Contains(b.Entity("A", "x")));
    EXPECT_FALSE(set.AsSet().Contains(b.Long(2)));
    EXPECT_TRUE(set.AsSet().ContainsAll(b.Set({b.Long(3), b.String("a")}).AsSet()));
    EXPECT_TRUE(set.AsSet().ContainsAll(b.Set({}).AsSet()));
    EXPECT_FALSE(set.AsSet().ContainsAll(b.Set({b.Long(3), b.Long(4)}).AsSet()));
    EXPECT_TRUE(set.AsSet().ContainsAny(b.Set({b.Long(0), b.Long(1)}).AsSet()));
    EXPECT_FALSE(set.AsSet().ContainsAny(b.Set({b.Long(0), b.Long(2)}).AsSet()));
    EXPECT_FALSE(set.AsSet().ContainsAny(b.Set({}).AsSet()));

    const std::array<RecordEntry, 3> duplicated{{{"k", b.Long(1)}, {"j", b.Long(2)}, {"k", b.Long(3)}}};
    const auto rejected = b.Arena().MakeRecord(duplicated);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().key, "k");

    const Value record = b.Record({{"z", b.Long(1)}, {"a", b.Long(2)}});
    ASSERT_NE(record.AsRecord().Find("a"), nullptr);
    EXPECT_EQ(*record.AsRecord().Find("a"), b.Long(2));
    EXPECT_FALSE(record.AsRecord().Contains("m"));
}

TEST(ValueOrderingTests, OwnedValueOutlivesItsSource)
{
    std::unique_ptr<Builder> source = std::make_unique<Builder>();
    const Value original = source->Record({{"set", source->Set({source->Entity("A", "x"), source->String("text")})}});
    const OwnedValue owned = OwnedValue::Copy(original);
    const std::string printed = ToCedar(original);
    source.reset();
    EXPECT_EQ(ToCedar(owned.Get()), printed);
    const OwnedValue empty = OwnedValue::Copy(Value::FromString(""));
    EXPECT_EQ(ToCedar(empty.Get()), R"("")");
}

// Views into a temporary owner would dangle, so those calls do not compile.
template <typename Owner>
concept GetOnTemporary = requires(const Owner&& owner) { std::move(owner).Get(); };
template <typename Owner>
concept RecordOnTemporary = requires(const Owner&& owner) { std::move(owner).GetRecord(); };
template <typename Owner>
concept ValueOnTemporary = requires(const Owner&& owner) { std::move(owner).AsValue(); };
static_assert(!GetOnTemporary<OwnedValue>);
static_assert(!RecordOnTemporary<cedar::Context>);
static_assert(!ValueOnTemporary<cedar::Context>);

TEST(ValueOrderingTests, AdoptSortsInPlace)
{
    Builder b;
    ValueArena& arena = b.Arena();
    const std::span<Value> elements = arena.AllocateValues(4);
    ASSERT_EQ(elements.size(), 4U);
    elements[0] = b.Long(3);
    elements[1] = b.String("a");
    elements[2] = b.Long(1);
    elements[3] = b.Long(3);
    const Value set = arena.AdoptSet(elements);
    EXPECT_EQ(set.AsSet().Elements().data(), elements.data());
    EXPECT_EQ(ToCedar(set), R"([1, 3, "a"])");
    EXPECT_EQ(set, b.Set({b.Long(1), b.Long(3), b.String("a")}));
    EXPECT_TRUE(arena.AdoptSet({}).AsSet().IsEmpty());

    const std::span<RecordEntry> entries = arena.AllocateRecordEntries(3);
    entries[0] = {arena.CopyText("z"), b.Long(1)};
    entries[1] = {arena.CopyText("a"), b.Long(2)};
    entries[2] = {arena.CopyText("m"), b.Long(3)};
    const auto record = arena.AdoptRecord(entries);
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->AsRecord().Entries().data(), entries.data());
    EXPECT_EQ(ToCedar(*record), "{a: 2, m: 3, z: 1}");

    const std::span<RecordEntry> duplicated = arena.AllocateRecordEntries(2);
    duplicated[0] = {"k", b.Long(1)};
    duplicated[1] = {"k", b.Long(2)};
    const auto rejected = arena.AdoptRecord(duplicated);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().key, "k");
}

} // namespace
