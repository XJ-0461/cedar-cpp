#include <cedar/Entities.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <expected>
#include <initializer_list>
#include <string>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

using cedar::DiagnosticCode;
using cedar::Entities;
using cedar::Entity;
using cedar::EntityUid;
using cedar::TransitiveClosureMode;

constexpr auto Assume = TransitiveClosureMode::AssumeAlreadyComputed;
constexpr auto Enforce = TransitiveClosureMode::EnforceAlreadyComputed;
constexpr auto Compute = TransitiveClosureMode::ComputeNow;

auto Uid(const std::string_view id) -> EntityUid { return EntityUid::Create("E", id).value(); }

auto Node(const std::string_view id, const std::initializer_list<std::string_view> parents) -> Entity
{
    std::vector<EntityUid> parentUids;
    for (const std::string_view parent : parents) { parentUids.push_back(Uid(parent)); }
    return Entity{Uid(id), std::move(parentUids)};
}

// The ids of `id`'s ancestors, sorted.
auto AncestorsOf(const Entities& entities, const std::string_view id) -> std::vector<std::string>
{
    const Entity* const entity = entities.Find(Uid(id));
    EXPECT_NE(entity, nullptr) << id;
    std::vector<std::string> ids;
    if (entity == nullptr) { return ids; }
    for (const EntityUid& uid : entity->Ancestors()) { ids.emplace_back(uid.Id().Text()); }
    std::ranges::sort(ids);
    return ids;
}

auto Ids(const std::initializer_list<std::string_view> ids) -> std::vector<std::string>
{
    std::vector<std::string> result{ids.begin(), ids.end()};
    std::ranges::sort(result);
    return result;
}

auto Build(std::vector<Entity> entities, const TransitiveClosureMode mode) -> Entities
{
    auto built = Entities::Create(std::move(entities), mode);
    EXPECT_TRUE(built.has_value()) << (built ? "" : built.error().Render());
    return built ? std::move(*built) : Entities{};
}

auto FailureCode(const std::expected<Entities, cedar::EntitiesError>& result) -> DiagnosticCode
{
    EXPECT_FALSE(result.has_value());
    return result ? DiagnosticCode{} : result.error().Primary().code;
}

TEST(TransitiveClosureTests, ClosesChain)
{
    const Entities entities = Build({Node("a", {"b"}), Node("b", {"c"}), Node("c", {"d"}), Node("d", {})}, Compute);
    EXPECT_EQ(AncestorsOf(entities, "a"), Ids({"b", "c", "d"}));
    EXPECT_EQ(AncestorsOf(entities, "b"), Ids({"c", "d"}));
    EXPECT_EQ(AncestorsOf(entities, "d"), Ids({}));
    const Entity& a = *entities.Find(Uid("a"));
    EXPECT_EQ(a.Parents().Size(), 1U);
    EXPECT_TRUE(a.Parents().Contains(Uid("b")));
    EXPECT_FALSE(a.Parents().Contains(Uid("c")));
    EXPECT_TRUE(a.Ancestors().Contains(Uid("c")));
    EXPECT_TRUE(a.IsDescendantOf(Uid("d")));
    EXPECT_FALSE(a.IsDescendantOf(Uid("a")));
}

TEST(TransitiveClosureTests, ClosesDiamondAndMissingParents)
{
    // `x` is named as a parent but is not in the store; it is still an ancestor.
    const Entities entities = Build(
        {Node("a", {"b", "c"}), Node("b", {"d"}), Node("c", {"d", "x"}), Node("d", {"e"}), Node("e", {})}, Compute);
    EXPECT_EQ(AncestorsOf(entities, "a"), Ids({"b", "c", "d", "e", "x"}));
    EXPECT_EQ(AncestorsOf(entities, "c"), Ids({"d", "e", "x"}));
    EXPECT_FALSE(entities.Contains(Uid("x")));
}

TEST(TransitiveClosureTests, SelfLoopIsACycleUnlessAssumed)
{
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"a"})}, Compute)), DiagnosticCode::TransitiveClosureCycle);
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"a"})}, Enforce)), DiagnosticCode::TransitiveClosureCycle);
    const Entities assumed = Build({Node("a", {"a"})}, Assume);
    EXPECT_TRUE(assumed.Find(Uid("a"))->IsDescendantOf(Uid("a")));
}

TEST(TransitiveClosureTests, CyclesInEachMode)
{
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"b"}), Node("b", {"c"}), Node("c", {"a"})}, Compute)),
              DiagnosticCode::TransitiveClosureCycle);
    // Not closed: the closure check fails before the cycle check runs.
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"b"}), Node("b", {"a"})}, Enforce)),
              DiagnosticCode::TransitiveClosureMissingEdge);
    // Closed but cyclic.
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"a", "b"}), Node("b", {"a", "b"})}, Enforce)),
              DiagnosticCode::TransitiveClosureCycle);
    const Entities assumed = Build({Node("a", {"b"}), Node("b", {"a"})}, Assume);
    EXPECT_EQ(AncestorsOf(assumed, "a"), Ids({"b"}));
}

TEST(TransitiveClosureTests, EnforceDetectsMissingEdge)
{
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"b"}), Node("b", {"c"}), Node("c", {})}, Enforce)),
              DiagnosticCode::TransitiveClosureMissingEdge);
    const Entities closed = Build({Node("a", {"b", "c"}), Node("b", {"c"}), Node("c", {})}, Enforce);
    EXPECT_EQ(AncestorsOf(closed, "a"), Ids({"b", "c"}));
    // Assume trusts the input: `a in c` would be false.
    const Entities assumed = Build({Node("a", {"b"}), Node("b", {"c"})}, Assume);
    EXPECT_EQ(AncestorsOf(assumed, "a"), Ids({"b"}));
}

TEST(TransitiveClosureTests, DuplicateEntities)
{
    const Entities same = Build({Node("a", {"b"}), Node("a", {"b"})}, Compute);
    EXPECT_EQ(same.Size(), 1U);
    EXPECT_EQ(FailureCode(Entities::Create({Node("a", {"b"}), Node("a", {"c"})}, Compute)),
              DiagnosticCode::DuplicateEntity);
}

TEST(TransitiveClosureTests, AddRepairsDescendants)
{
    const Entities original = Build({Node("a", {"b"}), Node("b", {})}, Compute);
    // `b` is already there with no parents, so giving it again with a parent is a conflict.
    EXPECT_EQ(FailureCode(original.Add({Node("b", {"c"})}, Compute)), DiagnosticCode::DuplicateEntity);

    const Entities withParent = Build({Node("a", {"b"})}, Compute);
    auto added = withParent.Add({Node("b", {"c"}), Node("c", {"d"})}, Compute);
    ASSERT_TRUE(added.has_value()) << added.error().Render();
    EXPECT_EQ(AncestorsOf(*added, "a"), Ids({"b", "c", "d"}));
    EXPECT_EQ(AncestorsOf(*added, "b"), Ids({"c", "d"}));
    // The original store is unchanged.
    EXPECT_EQ(AncestorsOf(withParent, "a"), Ids({"b"}));
    EXPECT_FALSE(withParent.Contains(Uid("b")));

    EXPECT_EQ(FailureCode(std::move(*added).Add({Node("d", {"a"})}, Compute)), DiagnosticCode::TransitiveClosureCycle);
    EXPECT_EQ(FailureCode(withParent.Add({Node("b", {"c"}), Node("c", {})}, Enforce)),
              DiagnosticCode::TransitiveClosureMissingEdge);
}

TEST(TransitiveClosureTests, RemoveRepairsDescendants)
{
    const Entities diamond =
        Build({Node("a", {"b", "d"}), Node("b", {"c"}), Node("d", {"c"}), Node("c", {"e"}), Node("e", {})}, Compute);
    const EntityUid removeB[] = {Uid("b")};
    auto withoutB = diamond.Remove(removeB, Compute);
    ASSERT_TRUE(withoutB.has_value()) << withoutB.error().Render();
    EXPECT_FALSE(withoutB->Contains(Uid("b")));
    // `c` and `e` stay reachable through `d`; `b` is gone as a parent too.
    EXPECT_EQ(AncestorsOf(*withoutB, "a"), Ids({"c", "d", "e"}));
    EXPECT_EQ(AncestorsOf(diamond, "a"), Ids({"b", "c", "d", "e"}));

    const Entities chain = Build({Node("a", {"b"}), Node("b", {"c"}), Node("c", {})}, Compute);
    auto withoutMiddle = chain.Remove(removeB, Enforce);
    ASSERT_TRUE(withoutMiddle.has_value()) << withoutMiddle.error().Render();
    EXPECT_EQ(AncestorsOf(*withoutMiddle, "a"), Ids({}));

    const EntityUid absent[] = {Uid("z")};
    auto unchanged = chain.Remove(absent, Compute);
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(unchanged->Size(), 3U);
}

TEST(TransitiveClosureTests, UpsertRepairsDescendants)
{
    const Entities chain = Build({Node("a", {"b"}), Node("b", {"c"}), Node("c", {})}, Compute);
    auto moved = chain.Upsert({Node("b", {"d"}), Node("b", {"x"})}, Compute);
    ASSERT_TRUE(moved.has_value()) << moved.error().Render();
    // The last version of `b` wins, and `a` loses `c`, which it had only through the old `b`.
    EXPECT_EQ(AncestorsOf(*moved, "b"), Ids({"x"}));
    EXPECT_EQ(AncestorsOf(*moved, "a"), Ids({"b", "x"}));
    EXPECT_EQ(AncestorsOf(chain, "a"), Ids({"b", "c"}));

    auto inserted = chain.Upsert({Node("c", {"d"}), Node("d", {})}, Compute);
    ASSERT_TRUE(inserted.has_value()) << inserted.error().Render();
    EXPECT_EQ(AncestorsOf(*inserted, "a"), Ids({"b", "c", "d"}));

    EXPECT_EQ(FailureCode(chain.Upsert({Node("c", {"a"})}, Compute)), DiagnosticCode::TransitiveClosureCycle);
}

TEST(TransitiveClosureTests, AncestorQueries)
{
    const Entities chain = Build({Node("a", {"b"}), Node("b", {"c"}), Node("c", {})}, Compute);
    EXPECT_TRUE(chain.IsAncestorOf(Uid("c"), Uid("a")));
    EXPECT_FALSE(chain.IsAncestorOf(Uid("a"), Uid("c")));
    // As in Rust: a present entity is not its own ancestor, an absent one is `in` itself.
    EXPECT_FALSE(chain.IsAncestorOf(Uid("a"), Uid("a")));
    EXPECT_TRUE(chain.IsAncestorOf(Uid("z"), Uid("z")));
    EXPECT_FALSE(chain.IsAncestorOf(Uid("a"), Uid("z")));

    const auto ancestors = chain.Ancestors(Uid("a"));
    ASSERT_TRUE(ancestors.has_value());
    EXPECT_EQ(ancestors->Size(), 2U);
    EXPECT_TRUE(ancestors->Contains(Uid("b")));
    EXPECT_TRUE(ancestors->Contains(Uid("c")));
    EXPECT_FALSE(chain.Ancestors(Uid("z")).has_value());
}

TEST(TransitiveClosureTests, AddSharesGivenEntities)
{
    const std::shared_ptr<const Entity> action = std::make_shared<const Entity>(Node("view", {"group"}));
    const std::shared_ptr<const Entity> group = std::make_shared<const Entity>(Node("group", {}));
    const std::shared_ptr<const Entity> shared[] = {action, group};
    const Entities users = Build({Node("u", {})}, Compute);

    auto assumed = users.Add(shared, Assume);
    ASSERT_TRUE(assumed.has_value()) << assumed.error().Render();
    EXPECT_EQ(assumed->Find(Uid("view")), action.get());
    EXPECT_EQ(assumed->Find(Uid("group")), group.get());
    EXPECT_EQ(users.Size(), 1U);

    // ComputeNow re-closes a given entity whose ancestors change, on a copy.
    const std::shared_ptr<const Entity> onlyAction[] = {action};
    auto computed = Build({Node("group", {"all"}), Node("all", {})}, Compute).Add(onlyAction, Compute);
    ASSERT_TRUE(computed.has_value()) << computed.error().Render();
    EXPECT_EQ(AncestorsOf(*computed, "view"), Ids({"all", "group"}));
    EXPECT_NE(computed->Find(Uid("view")), action.get());
    EXPECT_EQ(action->Ancestors().Size(), 1U);
}

// An operation on a store never writes an entity it shares with another store, even once the other
// store is gone: the shared entity is copied, so its address changes.
TEST(TransitiveClosureTests, ChangesCopySharedEntities)
{
    auto original =
        std::make_unique<Entities>(Build({Node("child", {"p"}), Node("p", {"q"}), Node("q", {})}, Compute));
    Entities derived = *original;
    const Entity* const before = original->Find(Uid("child"));
    original.reset();
    const EntityUid removeP[] = {Uid("p")};
    auto removed = std::move(derived).Remove(removeP, Compute);
    ASSERT_TRUE(removed.has_value()) << removed.error().Render();
    EXPECT_NE(removed->Find(Uid("child")), before);
    EXPECT_EQ(AncestorsOf(*removed, "child"), Ids({}));
}

// A reader on one thread and a store derived from the same entities on another. Run under
// ThreadSanitizer this reports a race if a derived store writes a shared entity in place.
TEST(TransitiveClosureTests, DerivedStoreDoesNotRaceReaders)
{
    std::size_t total = 0;
    for (int trial = 0; trial < 50; ++trial)
    {
        auto original = std::make_unique<Entities>(Build({Node("child", {"p"}), Node("p", {"q"})}, Compute));
        Entities derived = *original;
        std::size_t seen = 0;
        std::thread reader{[&] {
            for (const EntityUid& ancestor : original->Find(Uid("child"))->Ancestors())
            {
                seen += ancestor.Id().Text().size();
            }
            original.reset();
        }};
        const EntityUid removeP[] = {Uid("p")};
        auto removed = std::move(derived).Remove(removeP, Compute);
        reader.join();
        ASSERT_TRUE(removed.has_value());
        EXPECT_EQ(seen, 2U);
        total += removed->Size();
    }
    EXPECT_EQ(total, 50U);
}

// Views into a temporary store would dangle, so those calls do not compile.
template <typename Store>
concept FindOnTemporary = requires(const Store&& store, const EntityUid& uid) { std::move(store).Find(uid); };
template <typename Store>
concept AncestorsOnTemporary =
    requires(const Store&& store, const EntityUid& uid) { std::move(store).Ancestors(uid); };
static_assert(!FindOnTemporary<Entities>);
static_assert(!AncestorsOnTemporary<Entities>);
static_assert(std::is_convertible_v<const Entities&, cedar::EntityStoreView>);
static_assert(!std::is_convertible_v<Entities&&, cedar::EntityStoreView>);
static_assert(!std::is_constructible_v<cedar::EntityStoreView, Entities&&>);

} // namespace
