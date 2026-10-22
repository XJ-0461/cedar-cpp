#pragma once

#include <cedar/Entities.hpp>

#include <expected>
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
