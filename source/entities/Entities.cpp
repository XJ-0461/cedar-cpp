#include <cedar/Entities.hpp>

#include "entities/TransitiveClosure.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace cedar
{

using detail::EntityAccess;
using detail::EntitySlot;
using detail::EntityTable;

namespace
{

auto DuplicateEntityError(const EntityUid& uid) -> EntitiesError
{
    return EntitiesError{Diagnostic{
        .code = DiagnosticCode::DuplicateEntity,
        .message = std::format("entity `{}` is given more than once with different attributes, tags or ancestors",
                               uid.ToString()),
    }};
}

// Rust's `Entity::deep_eq`: same uid, attributes, tags and ancestor set (direct parents are not
// compared).
auto StructurallyEqual(const Entity& left, const Entity& right) noexcept -> bool
{
    return left.Uid() == right.Uid() && Value::FromRecord(left.Attributes()) == Value::FromRecord(right.Attributes())
           && Value::FromRecord(left.Tags()) == Value::FromRecord(right.Tags())
           && std::ranges::equal(left.Ancestors(), right.Ancestors());
}

// The entities one store operation may write: the ones it created and the copies it made.
//
// An operation never writes an entity it did not make itself, even when no other store seems to
// hold it. A use count cannot tell: it is read without synchronization, so another thread's last
// read of a store it has just released need not happen before this thread's write.
class WritableEntities final
{
public:
    // Makes a new entity owned by this operation.
    auto Create(Entity entity) -> std::shared_ptr<const Entity>
    {
        auto created = std::make_shared<Entity>(std::move(entity));
        owned_.insert(created.get());
        return created;
    }

    // The entity in `slot`, first replaced by a copy unless this operation made it.
    auto Get(const EntitySlot& slot) -> Entity&
    {
        if (owned_.contains(slot.entity.get()))
        {
            // Every owned entity was created non-const by this operation.
            return const_cast<Entity&>(*slot.entity);
        }
        auto copy = std::make_shared<Entity>(*slot.entity);
        Entity& writable = *copy;
        owned_.insert(copy.get());
        slot.entity = std::move(copy);
        return writable;
    }

private:
    std::unordered_set<const Entity*> owned_;
};

// Inserts `entity`, or accepts it when an identical one is already there.
auto InsertWithoutOverride(EntityTable& table, std::shared_ptr<const Entity> entity) -> std::expected<void, EntitiesError>
{
    if (const Entity* const existing = detail::FindInTable(table, entity->Uid()); existing != nullptr)
    {
        if (!StructurallyEqual(*existing, *entity)) { return std::unexpected{DuplicateEntityError(entity->Uid())}; }
        return {};
    }
    table.insert(EntitySlot{std::move(entity)});
    return {};
}

auto WritableNodes(EntityTable& table, const std::unordered_set<EntityUid>& uids, WritableEntities& writable)
    -> std::vector<Entity*>
{
    std::vector<Entity*> nodes;
    nodes.reserve(uids.size());
    for (const EntityUid& uid : uids)
    {
        if (const auto found = table.find(uid); found != table.end()) { nodes.push_back(&writable.Get(*found)); }
    }
    return nodes;
}

// Adds to `touched` every entity with an ancestor in it, so the entities below a changed one are
// re-closed too (the touched-set expansion in Rust's `add_entities` and `upsert_entities`).
void AddDescendantsOfTouched(const EntityTable& table, std::unordered_set<EntityUid>& touched)
{
    std::vector<EntityUid> descendants;
    for (const EntitySlot& slot : table)
    {
        const Entity& entity = *slot.entity;
        if (std::ranges::any_of(entity.Ancestors(), [&](const EntityUid& uid) { return touched.contains(uid); }))
        {
            descendants.push_back(entity.Uid());
        }
    }
    touched.insert(descendants.begin(), descendants.end());
}

// What Add and Upsert do after changing the table.
auto FinishChange(EntityTable& table, std::unordered_set<EntityUid>& touched, const TransitiveClosureMode mode,
                  WritableEntities& writable) -> std::expected<void, EntitiesError>
{
    switch (mode)
    {
    case TransitiveClosureMode::AssumeAlreadyComputed: return {};
    case TransitiveClosureMode::EnforceAlreadyComputed: return detail::EnforceClosed(table);
    case TransitiveClosureMode::ComputeNow:
    {
        AddDescendantsOfTouched(table, touched);
        const std::vector<Entity*> nodes = WritableNodes(table, touched, writable);
        return detail::RepairClosure(table, nodes, true);
    }
    }
    return {};
}

// Add over entities given either by value or as shared pointers. `Share(entity)` yields the
// pointer to insert: a new owned entity, or the caller's pointer as it is.
template <typename Range, typename Share>
auto AddEntities(EntityTable& table, Range& entities, const TransitiveClosureMode mode,
                 WritableEntities& writable, Share share) -> std::expected<void, EntitiesError>
{
    std::unordered_set<EntityUid> touched;
    for (auto& entity : entities)
    {
        std::shared_ptr<const Entity> shared = share(entity);
        touched.insert(shared->Uid());
        if (auto inserted = InsertWithoutOverride(table, std::move(shared)); !inserted)
        {
            return std::unexpected{std::move(inserted).error()};
        }
    }
    return FinishChange(table, touched, mode, writable);
}

} // namespace

// ---- EntityUidSetView ----

auto EntityUidSetView::Contains(const EntityUid& uid) const noexcept -> bool
{
    return detail::UidSetContains(uids_, uid);
}

// ---- Entity ----

Entity::Entity(EntityUid uid, std::vector<EntityUid> parents)
    : Entity{std::move(uid), std::move(parents), Record{}, Record{}, nullptr}
{
}

Entity::Entity(EntityUid uid, std::vector<EntityUid> parents, const Record attributes, const Record tags,
               std::shared_ptr<const ValueArena> storage)
    : uid_{std::move(uid)}, attributes_{attributes}, tags_{tags}, parents_{std::move(parents)},
      storage_{std::move(storage)}
{
    detail::SortUidSet(parents_);
    ancestors_ = parents_;
}

auto Entity::IsDescendantOf(const EntityUid& ancestor) const noexcept -> bool
{
    return detail::UidSetContains(ancestors_, ancestor);
}

void Entity::AddIndirectAncestor(const EntityUid& ancestor) { detail::UidSetInsert(ancestors_, ancestor); }

// ---- Entities ----

auto Entities::Create(std::vector<Entity> entities, const TransitiveClosureMode mode)
    -> std::expected<Entities, EntitiesError>
{
    Entities result;
    WritableEntities writable;
    result.table_.reserve(entities.size());
    for (Entity& entity : entities)
    {
        if (auto inserted = InsertWithoutOverride(result.table_, writable.Create(std::move(entity))); !inserted)
        {
            return std::unexpected{std::move(inserted).error()};
        }
    }
    switch (mode)
    {
    case TransitiveClosureMode::AssumeAlreadyComputed: break;
    case TransitiveClosureMode::EnforceAlreadyComputed:
        if (auto checked = detail::EnforceClosed(result.table_); !checked)
        {
            return std::unexpected{std::move(checked).error()};
        }
        break;
    case TransitiveClosureMode::ComputeNow:
    {
        std::vector<Entity*> nodes;
        nodes.reserve(result.table_.size());
        for (const EntitySlot& slot : result.table_) { nodes.push_back(&writable.Get(slot)); }
        if (auto closed = detail::RepairClosure(result.table_, nodes, true); !closed)
        {
            return std::unexpected{std::move(closed).error()};
        }
        break;
    }
    }
    return result;
}

auto Entities::Add(std::vector<Entity> entities, const TransitiveClosureMode mode) &&
    -> std::expected<Entities, EntitiesError>
{
    WritableEntities writable;
    if (auto added = AddEntities(table_, entities, mode, writable,
                                 [&](Entity& entity) { return writable.Create(std::move(entity)); });
        !added)
    {
        return std::unexpected{std::move(added).error()};
    }
    return std::move(*this);
}

auto Entities::Add(std::vector<Entity> entities, const TransitiveClosureMode mode) const&
    -> std::expected<Entities, EntitiesError>
{
    Entities copy{*this};
    return std::move(copy).Add(std::move(entities), mode);
}

auto Entities::Add(const std::span<const std::shared_ptr<const Entity>> entities, const TransitiveClosureMode mode) &&
    -> std::expected<Entities, EntitiesError>
{
    WritableEntities writable;
    if (auto added = AddEntities(table_, entities, mode, writable,
                                 [](const std::shared_ptr<const Entity>& entity) { return entity; });
        !added)
    {
        return std::unexpected{std::move(added).error()};
    }
    return std::move(*this);
}

auto Entities::Add(const std::span<const std::shared_ptr<const Entity>> entities, const TransitiveClosureMode mode) const&
    -> std::expected<Entities, EntitiesError>
{
    Entities copy{*this};
    return std::move(copy).Add(entities, mode);
}

auto Entities::Remove(const std::span<const EntityUid> uids, const TransitiveClosureMode mode) &&
    -> std::expected<Entities, EntitiesError>
{
    WritableEntities writable;
    std::unordered_set<EntityUid> touched;
    for (const EntityUid& uid : uids)
    {
        auto removedNode = table_.extract(uid);
        if (removedNode.empty()) { continue; }
        const Entity& removed = *removedNode.value().entity;
        for (const EntitySlot& slot : table_)
        {
            if (!slot.entity->IsDescendantOf(uid)) { continue; }
            touched.insert(slot.entity->Uid());
            Entity& descendant = writable.Get(slot);
            // Drop every link to the removed entity, and the indirect ancestors that may have come
            // through it; the repair below restores the ones reachable another way.
            std::vector<EntityUid>& parents = EntityAccess::Parents(descendant);
            std::vector<EntityUid>& ancestors = EntityAccess::Ancestors(descendant);
            detail::UidSetErase(parents, uid);
            detail::UidSetErase(ancestors, uid);
            for (const EntityUid& inherited : removed.Ancestors())
            {
                if (!detail::UidSetContains(parents, inherited)) { detail::UidSetErase(ancestors, inherited); }
            }
        }
    }
    const std::vector<Entity*> nodes = WritableNodes(table_, touched, writable);
    if (auto repaired = detail::RepairClosure(table_, nodes, mode == TransitiveClosureMode::ComputeNow); !repaired)
    {
        return std::unexpected{std::move(repaired).error()};
    }
    if (mode == TransitiveClosureMode::EnforceAlreadyComputed)
    {
        if (auto checked = detail::EnforceClosed(table_); !checked) { return std::unexpected{std::move(checked).error()}; }
    }
    return std::move(*this);
}

auto Entities::Remove(const std::span<const EntityUid> uids, const TransitiveClosureMode mode) const&
    -> std::expected<Entities, EntitiesError>
{
    Entities copy{*this};
    return std::move(copy).Remove(uids, mode);
}

auto Entities::Upsert(std::vector<Entity> entities, const TransitiveClosureMode mode) &&
    -> std::expected<Entities, EntitiesError>
{
    std::unordered_map<EntityUid, std::size_t> lastVersion;
    for (std::size_t index = 0; index < entities.size(); ++index) { lastVersion.insert_or_assign(entities[index].Uid(), index); }

    WritableEntities writable;
    std::unordered_set<EntityUid> touched;
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        Entity& entity = entities[index];
        if (lastVersion.at(entity.Uid()) != index) { continue; }
        const EntityUid& uid = entity.Uid();
        if (const auto existing = table_.find(uid); existing != table_.end())
        {
            // Strip from the old entity's descendants the indirect ancestors they had through it.
            const std::shared_ptr<const Entity> old = existing->entity;
            for (const EntitySlot& slot : table_)
            {
                if (slot.entity == old || !slot.entity->IsDescendantOf(uid)) { continue; }
                touched.insert(slot.entity->Uid());
                Entity& descendant = writable.Get(slot);
                const std::vector<EntityUid>& parents = EntityAccess::Parents(descendant);
                std::vector<EntityUid>& ancestors = EntityAccess::Ancestors(descendant);
                if (!detail::UidSetContains(parents, uid)) { detail::UidSetErase(ancestors, uid); }
                for (const EntityUid& inherited : old->Ancestors())
                {
                    if (!detail::UidSetContains(parents, inherited)) { detail::UidSetErase(ancestors, inherited); }
                }
            }
            existing->entity = writable.Create(std::move(entity));
            touched.insert(existing->entity->Uid());
        }
        else
        {
            touched.insert(uid);
            table_.insert(EntitySlot{writable.Create(std::move(entity))});
        }
    }
    if (auto finished = FinishChange(table_, touched, mode, writable); !finished)
    {
        return std::unexpected{std::move(finished).error()};
    }
    return std::move(*this);
}

auto Entities::Upsert(std::vector<Entity> entities, const TransitiveClosureMode mode) const&
    -> std::expected<Entities, EntitiesError>
{
    Entities copy{*this};
    return std::move(copy).Upsert(std::move(entities), mode);
}

auto Entities::Find(const EntityUid& uid) const& noexcept -> const Entity*
{
    return detail::FindInTable(table_, uid);
}

auto Entities::IsAncestorOf(const EntityUid& ancestor, const EntityUid& descendant) const noexcept -> bool
{
    if (const Entity* const entity = Find(descendant); entity != nullptr) { return entity->IsDescendantOf(ancestor); }
    return ancestor == descendant;
}

auto Entities::Ancestors(const EntityUid& uid) const& noexcept -> std::optional<EntityUidSetView>
{
    if (const Entity* const entity = Find(uid); entity != nullptr) { return entity->Ancestors(); }
    return std::nullopt;
}

} // namespace cedar
