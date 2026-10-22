#pragma once

#include <cedar/Entities.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

namespace cedar::detail
{

/// Write access to an entity's parent and ancestor sets, for the store and the closure code.
struct EntityAccess
{
    [[nodiscard]] static auto Parents(Entity& entity) noexcept -> std::vector<EntityUid>& { return entity.parents_; }
    [[nodiscard]] static auto Ancestors(Entity& entity) noexcept -> std::vector<EntityUid>& { return entity.ancestors_; }
    [[nodiscard]] static auto Ancestors(const Entity& entity) noexcept -> const std::vector<EntityUid>&
    {
        return entity.ancestors_;
    }
};

/// Sorts by EntityUidSetOrder and drops duplicates.
void SortUidSet(std::vector<EntityUid>& uids);

[[nodiscard]] auto UidSetContains(std::span<const EntityUid> uids, const EntityUid& uid) noexcept -> bool;

void UidSetInsert(std::vector<EntityUid>& uids, const EntityUid& uid);

void UidSetErase(std::vector<EntityUid>& uids, const EntityUid& uid);

/// The entity `uid` in `table`, or null.
[[nodiscard]] auto FindInTable(const EntityTable& table, const EntityUid& uid) noexcept -> const Entity*;

/// What CloseAncestorSets knows of an ancestor uid: its ancestor set (null when the entity is absent
/// or its ancestors are unknown) and, when it is one of the nodes being closed, its node index.
struct AncestorLookup
{
    const std::vector<EntityUid>* ancestors = nullptr;
    std::optional<std::size_t> node{};
};

/// Closes the ancestor sets of `nodeCount` nodes, the core of RepairClosure for any entity
/// representation. `ancestorsOf(node)` is a node's writable ancestor set, sorted by
/// EntityUidSetOrder; `lookup(uid)` is the AncestorLookup of an ancestor. Each node keeps its
/// ancestors and gains the ancestors of each of them, after those that are nodes too have been
/// closed first. A node on a cycle ends up its own ancestor.
template <typename AncestorsOf, typename Lookup>
void CloseAncestorSets(const std::size_t nodeCount, AncestorsOf&& ancestorsOf, Lookup&& lookup)
{
    enum class State : std::uint8_t
    {
        Unvisited,
        InProgress,
        Done,
    };
    std::vector<State> states(nodeCount, State::Unvisited);

    // An iterative depth-first walk over the nodes, so a long parent chain cannot exhaust the stack.
    // A node is closed in post-order: once every ancestor that is itself a node is closed, the
    // node's set is its current set plus each ancestor's set. A node met again while in progress is
    // on a cycle; its partial set is used, and the cycle shows up as a node that is its own ancestor.
    struct Frame
    {
        std::size_t node;
        std::size_t nextAncestor;
    };
    std::vector<Frame> stack;
    std::vector<EntityUid> merged;
    for (std::size_t root = 0; root < nodeCount; ++root)
    {
        if (states[root] != State::Unvisited) { continue; }
        states[root] = State::InProgress;
        stack.push_back({root, 0});
        while (!stack.empty())
        {
            Frame& frame = stack.back();
            const std::size_t current = frame.node;
            const std::vector<EntityUid>& ancestors = ancestorsOf(current);
            if (frame.nextAncestor < ancestors.size())
            {
                const AncestorLookup found = lookup(ancestors[frame.nextAncestor++]);
                if (found.node && states[*found.node] == State::Unvisited)
                {
                    states[*found.node] = State::InProgress;
                    stack.push_back({*found.node, 0});
                }
                continue;
            }
            merged.assign(ancestors.begin(), ancestors.end());
            for (const EntityUid& uid : ancestors)
            {
                if (const AncestorLookup found = lookup(uid); found.ancestors != nullptr)
                {
                    merged.insert(merged.end(), found.ancestors->begin(), found.ancestors->end());
                }
            }
            SortUidSet(merged);
            ancestorsOf(current).swap(merged);
            states[current] = State::Done;
            stack.pop_back();
        }
    }
}

/// Closes the ancestor sets of `nodes` (entities in `table` that the caller may write), taking
/// every other entity's set as already closed. Each node keeps its current ancestors and gains the
/// ancestors of each of them, after those that are nodes too have been closed first (Rust's
/// `repair_tc`; with every entity as a node it is a full closure). When `enforceAcyclic`, fails if
/// a node ends up its own ancestor.
[[nodiscard]] auto RepairClosure(const EntityTable& table, std::span<Entity* const> nodes, bool enforceAcyclic)
    -> std::expected<void, EntitiesError>;

/// Checks that every ancestor set in `table` is closed and that no entity is its own ancestor
/// (Rust's `enforce_tc_and_dag`): the closure check over the whole table first, then the cycle
/// check.
[[nodiscard]] auto EnforceClosed(const EntityTable& table) -> std::expected<void, EntitiesError>;

} // namespace cedar::detail
