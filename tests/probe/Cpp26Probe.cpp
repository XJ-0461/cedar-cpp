// The C++26 language and library features cedar-cpp relies on. Every feature here compiles cleanly,
// with cedar-cpp's warning flags, on both GCC 16 and Clang 22 (against libstdc++). Constant-evaluated
// checks are static_asserts; the rest run and return a distinct non-zero code on failure.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <flat_map>
#include <flat_set>
#include <format>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

// Deducing this.
struct ExplicitObject
{
    int Value = 0;

    template <typename Self>
    [[nodiscard]] auto Get(this Self&& self) -> decltype(auto)
    {
        return std::forward<Self>(self).Value;
    }
};

// Pack indexing, on a type pack and on a value pack.
template <typename... Types>
using FirstType = Types...[0];

template <auto... Values>
constexpr auto SecondValue = Values...[1];

static_assert(std::is_same_v<FirstType<int, double>, int>);
static_assert(SecondValue<1, 2, 3> == 2);

// std::string in constant evaluation.
constexpr auto ConstexprStringLength() -> std::size_t
{
    std::string text = "hello";
    text += ", world";
    return text.size();
}
static_assert(ConstexprStringLength() == 12);

// std::expected, in constant evaluation.
constexpr auto DoubleIfNonNegative(int input) -> std::expected<int, int>
{
    if (input < 0) { return std::unexpected(input); }
    return input * 2;
}
static_assert(DoubleIfNonNegative(2).value() == 4);
static_assert(DoubleIfNonNegative(-1).error() == -1);

// std::flat_map in constant evaluation.
constexpr auto ConstexprFlatMap() -> bool
{
    std::flat_map<int, int> map;
    map.emplace(2, 3);
    map.emplace(1, 5);
    return map.begin()->first == 1 && map.at(2) == 3;
}
static_assert(ConstexprFlatMap());

// std::chrono::year_month_day arithmetic in constant evaluation.
constexpr auto LeapDayPlusOne() -> bool
{
    constexpr std::chrono::year_month_day leapDay{std::chrono::year{2024}, std::chrono::month{2}, std::chrono::day{29}};
    constexpr std::chrono::year_month_day next{std::chrono::sys_days{leapDay} + std::chrono::days{1}};
    return leapDay.ok() && next == std::chrono::year_month_day{std::chrono::year{2024}, std::chrono::month{3}, std::chrono::day{1}};
}
static_assert(LeapDayPlusOne());

// std::ranges algorithms and views in constant evaluation.
constexpr auto SumOfScaledValuesAboveOne() -> int
{
    std::vector<int> values{5, 3, 1, 4};
    std::ranges::sort(values);
    int sum = 0;
    for (int value : values | std::views::filter([](int x) { return x > 1; }) | std::views::transform([](int x) { return x * 10; }))
    {
        sum += value;
    }
    return sum;
}
static_assert(SumOfScaledValuesAboveOne() == 120);

struct TriviallyCopyable
{
    std::int32_t First;
    std::int32_t Second;
};

} // namespace

auto main() -> int
{
    ExplicitObject object{3};
    if (object.Get() != 3) { return 1; }

    std::flat_map<std::string, int> map;
    map.emplace("b", 2);
    map.emplace("a", 1);
    std::flat_set<int> set{3, 1, 2, 1};
    if (map.begin()->first != "a" || set.size() != 3) { return 2; }

    std::array<std::byte, 256> buffer{};
    std::pmr::monotonic_buffer_resource resource{buffer.data(), buffer.size()};
    std::pmr::vector<int> arenaValues{&resource};
    arenaValues.assign({1, 2, 3});
    if (arenaValues.size() != 3) { return 3; }

    if (std::format("{}-{:>3}", "x", 7) != "x-  7") { return 4; }

    alignas(TriviallyCopyable) std::array<std::byte, sizeof(TriviallyCopyable)> storage{};
    const TriviallyCopyable* reinterpreted = std::start_lifetime_as<TriviallyCopyable>(storage.data());
    if (reinterpreted->First != 0 || reinterpreted->Second != 0) { return 5; }

    auto evens = std::views::iota(0, 10) | std::views::filter([](int x) { return x % 2 == 0; });
    if (std::ranges::distance(evens) != 5) { return 6; }

    return 0;
}
