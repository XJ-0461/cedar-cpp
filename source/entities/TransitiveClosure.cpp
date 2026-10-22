#include "entities/TransitiveClosure.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <unordered_map>
#include <utility>

namespace cedar::detail
{

namespace
{

auto CycleError(const EntityUid& uid) -> EntitiesError
{
    return EntitiesError{Diagnostic{
        .code = DiagnosticCode::TransitiveClosureCycle,
        .message = std::format("the entity hierarchy has a cycle through `{}`", uid.ToString()),
    }};
}

auto MissingEdgeError(const EntityUid& child, const EntityUid& parent, const EntityUid& grandparent) -> EntitiesError
{
    return EntitiesError{Diagnostic{
        .code = DiagnosticCode::TransitiveClosureMissingEdge,
        .message = std::format("the entity hierarchy is not transitively closed: `{}` has ancestor `{}`, which has "
                               "ancestor `{}`, but `{}` does not",
                               child.ToString(), parent.ToString(), grandparent.ToString(), child.ToString()),
        .help = "load the entities with the closure computed, or list every ancestor in each entity's parents",
    }};
}

} // namespace

void SortUidSet(std::vector<EntityUid>& uids)
{
    std::ranges::sort(uids, EntityUidSetOrder{});
    const auto duplicates = std::ranges::unique(uids);
    uids.erase(duplicates.begin(), duplicates.end());
}

auto UidSetContains(const std::span<const EntityUid> uids, const EntityUid& uid) noexcept -> bool
{
    const auto found = std::ranges::lower_bound(uids, uid, EntityUidSetOrder{});
    return found != uids.end() && *found == uid;
}

void UidSetInsert(std::vector<EntityUid>& uids, const EntityUid& uid)
{
    const auto found = std::ranges::lower_bound(uids, uid, EntityUidSetOrder{});
    if (found == uids.end() || *found != uid) { uids.insert(found, uid); }
}

void UidSetErase(std::vector<EntityUid>& uids, const EntityUid& uid)
{
    const auto found = std::ranges::lower_bound(uids, uid, EntityUidSetOrder{});
    if (found != uids.end() && *found == uid) { uids.erase(found); }
}

auto FindInTable(const EntityTable& table, const EntityUid& uid) noexcept -> const Entity*
{
    const auto found = table.find(uid);
    return found == table.end() ? nullptr : found->entity.get();
}

auto RepairClosure(const EntityTable& table, const std::span<Entity* const> nodes, const bool enforceAcyclic)
    -> std::expected<void, EntitiesError>
{
    std::unordered_map<const Entity*, std::size_t> nodeIndex;
    nodeIndex.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) { nodeIndex.emplace(nodes[index], index); }
    CloseAncestorSets(
        nodes.size(), [&](const std::size_t node) -> std::vector<EntityUid>& { return EntityAccess::Ancestors(*nodes[node]); },
        [&](const EntityUid& uid) -> AncestorLookup {
            const Entity* const ancestor = FindInTable(table, uid);
            if (ancestor == nullptr) { return {}; }
            const auto found = nodeIndex.find(ancestor);
            return AncestorLookup{.ancestors = &EntityAccess::Ancestors(*ancestor),
                                  .node = found == nodeIndex.end() ? std::nullopt : std::optional{found->second}};
        });

    if (enforceAcyclic)
    {
        for (const Entity* const node : nodes)
        {
            if (node->IsDescendantOf(node->Uid())) { return std::unexpected{CycleError(node->Uid())}; }
        }
    }
    return {};
}

auto EnforceClosed(const EntityTable& table) -> std::expected<void, EntitiesError>
{
    for (const EntitySlot& slot : table)
    {
        const Entity& entity = *slot.entity;
        for (const EntityUid& uid : entity.Ancestors())
        {
            const Entity* const ancestor = FindInTable(table, uid);
            if (ancestor == nullptr) { continue; }
            for (const EntityUid& inherited : ancestor->Ancestors())
            {
                if (!entity.IsDescendantOf(inherited))
                {
                    return std::unexpected{MissingEdgeError(entity.Uid(), uid, inherited)};
                }
            }
        }
    }
    for (const EntitySlot& slot : table)
    {
        if (slot.entity->IsDescendantOf(slot.entity->Uid())) { return std::unexpected{CycleError(slot.entity->Uid())}; }
    }
    return {};
}

} // namespace cedar::detail
