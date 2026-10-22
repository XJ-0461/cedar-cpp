#pragma once

#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>
#include <cedar/Value.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace cedar
{

class Entity;
class Entities;
class PartialEntity;
class Schema;

namespace detail
{

struct EntityAccess;

/// The order of the parent and ancestor sets: by precomputed hash, then by EntityUid order, so a
/// binary search compares one integer per step and touches the ids only on a hash match.
struct EntityUidSetOrder
{
    [[nodiscard]] auto operator()(const EntityUid& left, const EntityUid& right) const noexcept -> bool
    {
        if (left.Hash() != right.Hash()) { return left.Hash() < right.Hash(); }
        return left < right;
    }
};

/// One table entry. A store operation reseats the pointer to an entity it made or copied itself
/// before changing it, never changing the uid it hashes on, which is why it may be mutable inside
/// the set.
struct EntitySlot
{
    mutable std::shared_ptr<const Entity> entity;
};

struct EntitySlotHash
{
    using is_transparent = void;
    [[nodiscard]] auto operator()(const EntitySlot& slot) const noexcept -> std::size_t;
    [[nodiscard]] auto operator()(const EntityUid& uid) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(uid.Hash());
    }
};

struct EntitySlotEqual
{
    using is_transparent = void;
    [[nodiscard]] auto operator()(const EntitySlot& left, const EntitySlot& right) const noexcept -> bool;
    [[nodiscard]] auto operator()(const EntitySlot& left, const EntityUid& right) const noexcept -> bool;
    [[nodiscard]] auto operator()(const EntityUid& left, const EntitySlot& right) const noexcept -> bool;
};

using EntityTable = std::unordered_set<EntitySlot, EntitySlotHash, EntitySlotEqual>;

} // namespace detail

/// A read-only view of a set of entity uids: an entity's parents or ancestors. Membership is
/// O(log n). Iteration visits each uid once in an internal order (by hash), not in Cedar's
/// EntityUid order: anything that prints the set sorts it by `<=>` first. The view is valid while
/// the entity it came from lives.
class EntityUidSetView final
{
public:
    class Iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = EntityUid;
        using difference_type = std::ptrdiff_t;
        using pointer = const EntityUid*;
        using reference = const EntityUid&;

        Iterator() = default;
        [[nodiscard]] auto operator*() const noexcept -> const EntityUid& { return *position_; }
        [[nodiscard]] auto operator->() const noexcept -> const EntityUid* { return position_; }
        auto operator++() noexcept -> Iterator&
        {
            ++position_;
            return *this;
        }
        auto operator++(int) noexcept -> Iterator
        {
            Iterator previous = *this;
            ++position_;
            return previous;
        }
        friend auto operator==(const Iterator&, const Iterator&) noexcept -> bool = default;

    private:
        friend class EntityUidSetView;
        explicit Iterator(const EntityUid* position) noexcept : position_{position} {}
        const EntityUid* position_ = nullptr;
    };

    /// The empty set.
    EntityUidSetView() noexcept = default;

    [[nodiscard]] auto Contains(const EntityUid& uid) const noexcept -> bool;
    [[nodiscard]] auto Size() const noexcept -> std::size_t { return uids_.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return uids_.empty(); }
    [[nodiscard]] auto begin() const noexcept -> Iterator { return Iterator{uids_.data()}; }
    [[nodiscard]] auto end() const noexcept -> Iterator { return Iterator{uids_.data() + uids_.size()}; }

private:
    friend class Entity;
    friend class PartialEntity;
    explicit EntityUidSetView(const std::span<const EntityUid> uids) noexcept : uids_{uids} {}

    std::span<const EntityUid> uids_;
};

static_assert(std::forward_iterator<EntityUidSetView::Iterator>);

/// An entity: its uid, attributes, tags, parents and ancestors.
///
/// `Ancestors()` is the transitive closure of the parent relation as the store computed or was
/// given it (Rust's `parents` plus `indirect_ancestors`); it always contains `Parents()`.
///
/// Attribute and tag values are views into `Storage()`, which the entity keeps alive. A loader
/// builds one arena per load batch and gives it to every entity of the batch, so removing or
/// replacing an entity frees none of that storage until every entity of its batch is gone. Entities
/// in an `Entities` store are immutable and shared between stores derived from one another.
class Entity final
{
public:
    /// An entity with no attributes or tags.
    explicit Entity(EntityUid uid, std::vector<EntityUid> parents = {});

    /// An entity whose attribute and tag values live in `storage` (or are inline in the Value).
    /// `storage` may be null when neither record points anywhere.
    Entity(EntityUid uid, std::vector<EntityUid> parents, Record attributes, Record tags,
           std::shared_ptr<const ValueArena> storage);

    [[nodiscard]] auto Uid() const noexcept -> const EntityUid& { return uid_; }
    [[nodiscard]] auto Attributes() const noexcept -> Record { return attributes_; }
    [[nodiscard]] auto Tags() const noexcept -> Record { return tags_; }

    /// The direct parents, as given.
    [[nodiscard]] auto Parents() const noexcept -> EntityUidSetView { return EntityUidSetView{parents_}; }

    /// Every ancestor: the parents and the indirect ancestors.
    [[nodiscard]] auto Ancestors() const noexcept -> EntityUidSetView { return EntityUidSetView{ancestors_}; }

    /// Whether `ancestor` is among this entity's ancestors (Rust's `is_descendant_of`). Not
    /// reflexive: Cedar's `in` adds the equality check itself. O(log n).
    [[nodiscard]] auto IsDescendantOf(const EntityUid& ancestor) const noexcept -> bool;

    /// The attribute `key`, or null.
    [[nodiscard]] auto FindAttribute(const std::string_view key) const noexcept -> const Value*
    {
        return attributes_.Find(key);
    }

    /// The tag `key`, or null.
    [[nodiscard]] auto FindTag(const std::string_view key) const noexcept -> const Value* { return tags_.Find(key); }

    /// The arena the attribute and tag values live in, for making an OwnedValue that keeps one of
    /// them alive. Null when no value needs storage.
    [[nodiscard]] auto Storage() const noexcept -> const std::shared_ptr<const ValueArena>& { return storage_; }

    /// Records `ancestor` as an indirect ancestor; nothing changes when it is already an ancestor.
    /// The caller keeps the set closed. This builds entities whose ancestor sets are closed before
    /// they reach a store (a schema's action entities, added with
    /// TransitiveClosureMode::AssumeAlreadyComputed).
    void AddIndirectAncestor(const EntityUid& ancestor);

private:
    friend struct detail::EntityAccess;

    EntityUid uid_;
    Record attributes_;
    Record tags_;
    std::vector<EntityUid> parents_;
    std::vector<EntityUid> ancestors_;
    std::shared_ptr<const ValueArena> storage_;
};

/// How a store treats the ancestor sets of the entities it is given (Rust's `TCComputation`).
enum class TransitiveClosureMode : std::uint8_t
{
    /// Trust that every ancestor set is already closed and the hierarchy is acyclic. Nothing is
    /// checked; a wrong input makes `in` answer from the sets as given.
    AssumeAlreadyComputed,
    /// Check that every ancestor set is already closed (an ancestor's ancestors are ancestors too)
    /// and that no entity is its own ancestor. Fails otherwise.
    EnforceAlreadyComputed,
    /// Close the ancestor sets from the parents. Fails when the hierarchy has a cycle.
    ComputeNow,
};

/// The entity hierarchy: a hash map from EntityUid to Entity with closed ancestor sets.
///
/// A store is immutable once built and safe to share between threads. Add, Remove and Upsert
/// return a new store; entities they do not change are shared with the original, and an entity
/// they change is copied first, so a store derived on one thread never writes an entity another
/// thread may be reading. Called on an rvalue they reuse its table, and on failure the rvalue is
/// left valid but unspecified; called on an lvalue they copy the table first and leave the original
/// untouched.
///
/// Parents and ancestors may name entities that are not in the store; `in` still answers from the
/// sets. An entity given twice must be given identically (same attributes, tags and ancestor set),
/// otherwise the build fails with `entities.duplicate-entity`.
class Entities final
{
public:
    /// Iterates the entities in an unspecified order.
    class Iterator
    {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = Entity;
        using difference_type = std::ptrdiff_t;
        using pointer = const Entity*;
        using reference = const Entity&;

        Iterator() = default;
        [[nodiscard]] auto operator*() const noexcept -> const Entity& { return *position_->entity; }
        [[nodiscard]] auto operator->() const noexcept -> const Entity* { return position_->entity.get(); }
        auto operator++() noexcept -> Iterator&
        {
            ++position_;
            return *this;
        }
        auto operator++(int) noexcept -> Iterator
        {
            Iterator previous = *this;
            ++position_;
            return previous;
        }
        friend auto operator==(const Iterator&, const Iterator&) noexcept -> bool = default;

    private:
        friend class Entities;
        explicit Iterator(detail::EntityTable::const_iterator position) noexcept : position_{position} {}
        detail::EntityTable::const_iterator position_{};
    };

    /// The empty store.
    Entities() = default;

    /// A store of `entities`, closed according to `mode`. Fails on an entity given twice with
    /// different contents, and in the checking modes on a hierarchy that is not closed
    /// (`entities.transitive-closure-missing-edge`) or has a cycle
    /// (`entities.transitive-closure-cycle`).
    [[nodiscard]] static auto Create(std::vector<Entity> entities, TransitiveClosureMode mode)
        -> std::expected<Entities, EntitiesError>;

    /// A store of `entities` checked against `schema` (Rust's `Entities::from_entities` with a
    /// schema): each entity's type must be declared, with the declared attributes, tags and parent
    /// types (`entities.*` conformance codes), and an action entity must equal the schema's. The
    /// store is closed according to `mode`, then the schema's action entities are added.
    [[nodiscard]] static auto Create(std::vector<Entity> entities, TransitiveClosureMode mode, const Schema& schema)
        -> std::expected<Entities, EntitiesError>;

    /// The store with `entities` added (Rust's `add_entities`). An entity already in the store may be
    /// given again only identically, compared against its stored ancestor set. With ComputeNow the
    /// added entities, and every entity that has one of them as an ancestor, are re-closed.
    [[nodiscard]] auto Add(std::vector<Entity> entities, TransitiveClosureMode mode) &&
        -> std::expected<Entities, EntitiesError>;
    [[nodiscard]] auto Add(std::vector<Entity> entities, TransitiveClosureMode mode) const&
        -> std::expected<Entities, EntitiesError>;

    /// Add, sharing the given entities instead of copying them: the store holds these pointers
    /// unless ComputeNow has to change an entity's ancestor set, in which case it holds a changed
    /// copy. This is how a schema's action entities, built once with closed ancestor sets, join
    /// every store checked against it (with AssumeAlreadyComputed).
    [[nodiscard]] auto Add(std::span<const std::shared_ptr<const Entity>> entities, TransitiveClosureMode mode) &&
        -> std::expected<Entities, EntitiesError>;
    [[nodiscard]] auto Add(std::span<const std::shared_ptr<const Entity>> entities, TransitiveClosureMode mode) const&
        -> std::expected<Entities, EntitiesError>;

    /// The store without the entities `uids` (Rust's `remove_entities`); absent uids are ignored.
    /// Every descendant of a removed entity loses it as a parent and ancestor, and its ancestor set
    /// is recomputed. With EnforceAlreadyComputed the whole store is checked afterwards. A removed
    /// entity's attribute storage is freed only when every entity of its load batch is gone.
    [[nodiscard]] auto Remove(std::span<const EntityUid> uids, TransitiveClosureMode mode) &&
        -> std::expected<Entities, EntitiesError>;
    [[nodiscard]] auto Remove(std::span<const EntityUid> uids, TransitiveClosureMode mode) const&
        -> std::expected<Entities, EntitiesError>;

    /// The store with `entities` inserted, replacing any entity with the same uid (Rust's
    /// `upsert_entities`). When a uid is given more than once, the last one wins. The descendants of
    /// a replaced entity lose the indirect ancestors they had through it before the closure is
    /// repaired.
    [[nodiscard]] auto Upsert(std::vector<Entity> entities, TransitiveClosureMode mode) &&
        -> std::expected<Entities, EntitiesError>;
    [[nodiscard]] auto Upsert(std::vector<Entity> entities, TransitiveClosureMode mode) const&
        -> std::expected<Entities, EntitiesError>;

    /// The entity `uid`, or null when the store has none. The pointer is valid while this store,
    /// or any store sharing the entity, lives; a temporary store would leave it dangling, so that
    /// overload is deleted.
    [[nodiscard]] auto Find(const EntityUid& uid) const& noexcept -> const Entity*;
    auto Find(const EntityUid& uid) const&& noexcept -> const Entity* = delete;

    /// Whether `ancestor` is an ancestor of `descendant` (Rust's `Entities::is_ancestor_of`). When
    /// `descendant` is in the store this is `descendant`'s IsDescendantOf, which is not reflexive;
    /// when it is not, the answer is `ancestor == descendant`.
    [[nodiscard]] auto IsAncestorOf(const EntityUid& ancestor, const EntityUid& descendant) const noexcept -> bool;

    /// The ancestors of `uid`, or nothing when the store has no such entity (Rust's
    /// `Entities::ancestors`). Valid as Find's pointer is.
    [[nodiscard]] auto Ancestors(const EntityUid& uid) const& noexcept -> std::optional<EntityUidSetView>;
    auto Ancestors(const EntityUid& uid) const&& noexcept -> std::optional<EntityUidSetView> = delete;

    [[nodiscard]] auto Contains(const EntityUid& uid) const noexcept -> bool { return Find(uid) != nullptr; }
    [[nodiscard]] auto Size() const noexcept -> std::size_t { return table_.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return table_.empty(); }
    [[nodiscard]] auto begin() const noexcept -> Iterator { return Iterator{table_.begin()}; }
    [[nodiscard]] auto end() const noexcept -> Iterator { return Iterator{table_.end()}; }

private:
    detail::EntityTable table_;
};

/// What the evaluator needs from an entity source: the entity with a given uid, or null when there
/// is none. The returned entity's ancestor set must be closed, and the pointer must stay valid for
/// the whole authorization call. `Entities` models it; a service can model it over its own storage
/// (a cache in front of a database) without copying into an `Entities`.
template <typename Store>
concept EntityStore = requires(const Store& store, const EntityUid& uid) {
    { store.Find(uid) } -> std::same_as<const Entity*>;
};

/// A non-owning, type-erased reference to any EntityStore: a pointer to the store and one function
/// pointer. The evaluator is compiled once against this type, and IsAuthorized and
/// EvaluateExpression take it directly; any EntityStore converts to it implicitly. The store must
/// outlive the view, so a view of a temporary store does not compile.
class EntityStoreView final
{
public:
    template <EntityStore Store>
        requires(!std::same_as<std::remove_cvref_t<Store>, EntityStoreView>)
    EntityStoreView(const Store& store) noexcept
        : store_{&store}, find_{[](const void* erased, const EntityUid& uid) -> const Entity* {
              return static_cast<const Store*>(erased)->Find(uid);
          }}
    {
    }

    template <EntityStore Store>
        requires(!std::same_as<std::remove_cvref_t<Store>, EntityStoreView>)
    EntityStoreView(const Store&& store) = delete;

    [[nodiscard]] auto Find(const EntityUid& uid) const -> const Entity* { return find_(store_, uid); }

private:
    const void* store_;
    auto (*find_)(const void*, const EntityUid&) -> const Entity*;
};

static_assert(EntityStore<Entities>);
static_assert(EntityStore<EntityStoreView>);

namespace detail
{

inline auto EntitySlotHash::operator()(const EntitySlot& slot) const noexcept -> std::size_t
{
    return static_cast<std::size_t>(slot.entity->Uid().Hash());
}

inline auto EntitySlotEqual::operator()(const EntitySlot& left, const EntitySlot& right) const noexcept -> bool
{
    return left.entity->Uid() == right.entity->Uid();
}

inline auto EntitySlotEqual::operator()(const EntitySlot& left, const EntityUid& right) const noexcept -> bool
{
    return left.entity->Uid() == right;
}

inline auto EntitySlotEqual::operator()(const EntityUid& left, const EntitySlot& right) const noexcept -> bool
{
    return left == right.entity->Uid();
}

} // namespace detail

} // namespace cedar
