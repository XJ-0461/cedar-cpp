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
    enum class State : std::uint8_t
    {
        Unvisited,
        InProgress,
        Done,
    };
    std::unordered_map<const Entity*, std::size_t> nodeIndex;
    nodeIndex.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) { nodeIndex.emplace(nodes[index], index); }
    std::vector<State> states(nodes.size(), State::Unvisited);

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
    for (std::size_t root = 0; root < nodes.size(); ++root)
    {
        if (states[root] != State::Unvisited) { continue; }
        states[root] = State::InProgress;
        stack.push_back({root, 0});
        while (!stack.empty())
        {
            Frame& frame = stack.back();
            Entity& entity = *nodes[frame.node];
            const std::vector<EntityUid>& ancestors = EntityAccess::Ancestors(entity);
            if (frame.nextAncestor < ancestors.size())
            {
                const Entity* const ancestor = FindInTable(table, ancestors[frame.nextAncestor++]);
                if (ancestor == nullptr) { continue; }
                const auto found = nodeIndex.find(ancestor);
                if (found != nodeIndex.end() && states[found->second] == State::Unvisited)
                {
                    states[found->second] = State::InProgress;
                    stack.push_back({found->second, 0});
                }
                continue;
            }
            merged.assign(ancestors.begin(), ancestors.end());
            for (const EntityUid& uid : ancestors)
            {
                if (const Entity* const ancestor = FindInTable(table, uid); ancestor != nullptr)
                {
                    const std::vector<EntityUid>& inherited = EntityAccess::Ancestors(*ancestor);
                    merged.insert(merged.end(), inherited.begin(), inherited.end());
                }
            }
            SortUidSet(merged);
            EntityAccess::Ancestors(entity).swap(merged);
            states[frame.node] = State::Done;
            stack.pop_back();
        }
    }

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
