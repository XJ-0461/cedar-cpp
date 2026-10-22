#pragma once

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>
#include <cedar/Value.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Type-aware partial evaluation (Rust's experimental `tpe` feature, `cedar-policy/src/api/tpe.rs`
// over `cedar-policy-core/src/tpe/`). A request may leave the principal id, the resource id or the
// context unknown, and entities may leave their attributes, ancestors or tags unknown. Each policy
// is typechecked in the one request environment the request names and evaluated as far as the
// known data allows, ending as true, false, an error, or a residual expression. Batched
// authorization and the permission queries are built on it.

namespace cedar
{

namespace detail
{

struct PartialEvaluationAccess;
struct PartialAuthorizationResult;

} // namespace detail

// ---- Partial requests ----

/// An entity uid whose id may be unknown (Rust's `PartialEntityUid`). The type is always known.
struct PartialEntityUid
{
    EntityType type;
    std::optional<Eid> id{};

    /// The uid `uid`, with its id known.
    [[nodiscard]] static auto FromConcrete(EntityUid uid) -> PartialEntityUid
    {
        return PartialEntityUid{.type = uid.Type(), .id = uid.Id()};
    }

    friend auto operator==(const PartialEntityUid&, const PartialEntityUid&) noexcept -> bool = default;
};

/// A request whose principal id, resource id or context may be unknown (Rust's `PartialRequest`).
/// The action is always known. It is checked against the schema when it is made, so it names
/// exactly one request environment. Copies share the context's storage.
class PartialRequest final
{
public:
    /// The partial request checked against `schema` (Rust's `PartialRequest::new`), in Rust's order:
    /// the action must be declared, the principal and resource types must be ones it applies to, a
    /// known id of an enumerated type must be one of its listed ids, and a known context must have
    /// the type the action declares (`request.*` codes).
    [[nodiscard]] static auto Create(PartialEntityUid principal, EntityUid action, PartialEntityUid resource,
                                     std::optional<Context> context, const Schema& schema)
        -> std::expected<PartialRequest, RequestValidationError>;

    [[nodiscard]] auto Principal() const noexcept -> const PartialEntityUid& { return principal_; }
    [[nodiscard]] auto Action() const noexcept -> const EntityUid& { return action_; }
    [[nodiscard]] auto Resource() const noexcept -> const PartialEntityUid& { return resource_; }

    /// The context, or nothing when it is unknown.
    [[nodiscard]] auto GetContext() const noexcept -> const std::optional<Context>& { return context_; }

private:
    friend struct detail::PartialEvaluationAccess;

    PartialRequest(PartialEntityUid principal, EntityUid action, PartialEntityUid resource,
                   std::optional<Context> context) noexcept
        : principal_{std::move(principal)}, action_{std::move(action)}, resource_{std::move(resource)},
          context_{std::move(context)}
    {
    }

    PartialEntityUid principal_;
    EntityUid action_;
    PartialEntityUid resource_;
    std::optional<Context> context_;
};

// ---- Partial entities ----

/// An entity whose attributes, ancestors or tags may each be unknown (Rust's `PartialEntity`). Each
/// part is either known in full or unknown in full. Attribute and tag values are views into
/// `Storage()`, which the entity keeps alive, as for `Entity`.
class PartialEntity final
{
public:
    /// The entity `uid` checked against `schema` (Rust's `PartialEntity::new`): its type must be
    /// declared, and its known attributes, ancestors and tags must have the declared types
    /// (`entities.*` conformance codes). `ancestors` may hold only the parents; PartialEntities
    /// closes the set. Attribute and tag values live in `storage` (or are inline in the Value), which
    /// may be null when neither record points anywhere.
    [[nodiscard]] static auto Create(EntityUid uid, std::optional<Record> attributes,
                                     std::optional<std::vector<EntityUid>> ancestors, std::optional<Record> tags,
                                     std::shared_ptr<const ValueArena> storage, const Schema& schema)
        -> std::expected<PartialEntity, PartialEntitiesError>;

    [[nodiscard]] auto Uid() const noexcept -> const EntityUid& { return uid_; }

    /// The attributes, or nothing when they are unknown.
    [[nodiscard]] auto Attributes() const noexcept -> std::optional<Record> { return attributes_; }

    /// The ancestors, or nothing when they are unknown. In a PartialEntities the set is closed.
    /// Valid while this entity, or a copy of it, lives.
    [[nodiscard]] auto Ancestors() const& noexcept -> std::optional<EntityUidSetView>
    {
        if (!ancestors_) { return std::nullopt; }
        return EntityUidSetView{*ancestors_};
    }
    auto Ancestors() const&& noexcept -> std::optional<EntityUidSetView> = delete;

    /// The tags, or nothing when they are unknown.
    [[nodiscard]] auto Tags() const noexcept -> std::optional<Record> { return tags_; }

    /// The arena the attribute and tag values live in. Null when no value needs storage.
    [[nodiscard]] auto Storage() const noexcept -> const std::shared_ptr<const ValueArena>& { return storage_; }

private:
    friend struct detail::PartialEvaluationAccess;

    PartialEntity(EntityUid uid, std::optional<Record> attributes, std::optional<std::vector<EntityUid>> ancestors,
                  std::optional<Record> tags, std::shared_ptr<const ValueArena> storage) noexcept
        : uid_{std::move(uid)}, attributes_{attributes}, ancestors_{std::move(ancestors)}, tags_{tags},
          storage_{std::move(storage)}
    {
    }

    EntityUid uid_;
    std::optional<Record> attributes_;
    std::optional<std::vector<EntityUid>> ancestors_; ///< Sorted, no uid twice.
    std::optional<Record> tags_;
    std::shared_ptr<const ValueArena> storage_;
};

/// The entity hierarchy for partial evaluation (Rust's `PartialEntities`): every entity checked
/// against the schema, the known ancestor sets closed, and the schema's action entities included.
/// An entity whose ancestors are known may not have an ancestor in the set whose own ancestors are
/// unknown (`partial-evaluation.ancestor-validation`). Immutable, and copies share it. Iteration
/// is in uid order.
class PartialEntities final
{
public:
    /// The store with no entities, not even the schema's actions (Rust's `PartialEntities::empty`).
    [[nodiscard]] static auto Empty() noexcept -> PartialEntities;

    /// `entities` checked against `schema`, closed, with the schema's action entities added (Rust's
    /// `PartialEntities::from_partial_entities`). An action entity in `entities` must have the
    /// ancestors the schema gives it (`partial-evaluation.mismatched-action-ancestors`). A uid given
    /// twice is an `entities.duplicate-entity` error, a known ancestor with unknown ancestors is a
    /// `partial-evaluation.ancestor-validation` error, and a cycle in the known ancestors is an
    /// `entities.transitive-closure-*` error.
    [[nodiscard]] static auto Create(std::vector<PartialEntity> entities, const Schema& schema)
        -> std::expected<PartialEntities, PartialEntitiesError>;

    /// Every entity of `entities`, all of it known, checked and completed as Create does, and failing
    /// as it does (Rust's `PartialEntities::from_concrete`).
    [[nodiscard]] static auto FromConcrete(const Entities& entities, const Schema& schema)
        -> std::expected<PartialEntities, PartialEntitiesError>;

    /// The entity `uid`, or null. Valid while this store, or a copy of it, lives.
    [[nodiscard]] auto Find(const EntityUid& uid) const& noexcept -> const PartialEntity*;
    auto Find(const EntityUid& uid) const&& noexcept -> const PartialEntity* = delete;

    [[nodiscard]] auto Contains(const EntityUid& uid) const noexcept -> bool;
    [[nodiscard]] auto Size() const noexcept -> std::size_t;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return Size() == 0; }

    /// Iteration in uid order. Valid as Find's pointer is.
    [[nodiscard]] auto begin() const& noexcept -> const PartialEntity*;
    [[nodiscard]] auto end() const& noexcept -> const PartialEntity*;
    auto begin() const&& noexcept -> const PartialEntity* = delete;
    auto end() const&& noexcept -> const PartialEntity* = delete;

private:
    friend struct detail::PartialEvaluationAccess;

    explicit PartialEntities(std::shared_ptr<const std::vector<PartialEntity>> entities) noexcept
        : entities_{std::move(entities)}
    {
    }

    /// Sorted by uid, no uid twice. Null for the empty store.
    std::shared_ptr<const std::vector<PartialEntity>> entities_;
};

// ---- Partial authorization ----

/// The result of partial authorization (Rust's `TpeResponse`). Every policy of the set is
/// classified by its effect and by what its condition reduced to: true, false, an error, or a
/// residual expression. Each residual is an ordinary `Policy` with the source policy's id and
/// annotations, an unconstrained scope and one `when` clause holding the residual. A condition that
/// is known to error is the residual `error()`, a call to an extension function that does not
/// exist, which evaluates to `evaluation.extension-function-lookup-failed` on reauthorization.
///
/// The response owns copies of the partial request, the partial entities and the schema (all of
/// which share their storage), so it does not depend on its inputs living. It is immutable, and
/// copies share it. Every id list is sorted by policy id.
class PartialAuthorizationResponse final
{
public:
    /// The decision, or nothing when the known data does not settle it (Rust's `decision`): Deny
    /// when a forbid is true, or when no permit is true or residual; otherwise nothing when a
    /// forbid is residual; otherwise Allow when a permit is true, and nothing when none is.
    [[nodiscard]] auto GetDecision() const noexcept -> std::optional<cedar::Decision>;

    /// The determining policies when there is a decision (Rust's `reason`): the true forbids for
    /// Deny with a true forbid, the true permits for Allow, and none for Deny with nothing
    /// satisfied. Nothing when there is no decision. Valid while this response or a copy lives.
    [[nodiscard]] auto Reason() const& noexcept -> std::optional<std::span<const PolicyId>>;
    auto Reason() const&& noexcept -> std::optional<std::span<const PolicyId>> = delete;

    /// The eight classifications. Valid as Reason's span is.
    [[nodiscard]] auto TruePermits() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto FalsePermits() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto ErrorPermits() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto ResidualPermits() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto TrueForbids() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto FalseForbids() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto ErrorForbids() const& noexcept -> std::span<const PolicyId>;
    [[nodiscard]] auto ResidualForbids() const& noexcept -> std::span<const PolicyId>;
    auto TruePermits() const&& noexcept -> std::span<const PolicyId> = delete;
    auto FalsePermits() const&& noexcept -> std::span<const PolicyId> = delete;
    auto ErrorPermits() const&& noexcept -> std::span<const PolicyId> = delete;
    auto ResidualPermits() const&& noexcept -> std::span<const PolicyId> = delete;
    auto TrueForbids() const&& noexcept -> std::span<const PolicyId> = delete;
    auto FalseForbids() const&& noexcept -> std::span<const PolicyId> = delete;
    auto ErrorForbids() const&& noexcept -> std::span<const PolicyId> = delete;
    auto ResidualForbids() const&& noexcept -> std::span<const PolicyId> = delete;

    /// The residual policies whose condition is neither true, false nor an error (Rust's
    /// `residual_policies`): the residual permits, then the residual forbids, each by policy id.
    [[nodiscard]] auto ResidualPolicies() const -> std::vector<Policy>;

    /// Every residual policy, the true, false and error ones included, by policy id (Rust's
    /// `policies`).
    [[nodiscard]] auto Policies() const -> std::vector<Policy>;

    /// Policies() collected into a set (Rust's `policy_set`). This is the set Reauthorize answers
    /// against.
    [[nodiscard]] auto ToPolicySet() const -> PolicySet;

    /// The residual policy `id`, or null when the policy set had no policy `id` (Rust's
    /// `get_policy`). Valid as Reason's span is.
    [[nodiscard]] auto GetPolicy(std::string_view id) const& noexcept -> const Policy*;
    auto GetPolicy(std::string_view id) const&& noexcept -> const Policy* = delete;

    /// Completes the authorization with a concrete request and entities (Rust's `reauthorize`), in
    /// Rust's order: `request` must conform to the schema (`request.*` codes), `entities` must
    /// conform to it (`entities.*` conformance codes), `entities` must agree with every known part
    /// of the partial entities (`partial-evaluation.missing-entity` through `mismatched-tag`), and
    /// `request` with every known part of the partial request
    /// (`partial-evaluation.inconsistent-*`). The response is IsAuthorized's over ToPolicySet(),
    /// with every policy evaluated. When this response has a decision, the result has the same one.
    [[nodiscard]] auto Reauthorize(const Request& request, const Entities& entities) const
        -> std::expected<Response, ReauthorizationError>;

private:
    friend struct detail::PartialEvaluationAccess;

    explicit PartialAuthorizationResponse(std::shared_ptr<const detail::PartialAuthorizationResult> result) noexcept
        : result_{std::move(result)}
    {
    }

    std::shared_ptr<const detail::PartialAuthorizationResult> result_;
};

/// Partially authorizes `request` against `policies` (Rust's `PolicySet::tpe`). Fails when the
/// schema is not complete (`validation.incomplete-schema`), when no request environment of the
/// schema matches the request (`partial-evaluation.no-matching-request-environment`), and when a
/// policy does not validate in that environment in strict mode
/// (`partial-evaluation.policy-validation`, followed by the validator's diagnostics). A policy that
/// cannot apply in the environment but has no type error is not an error. Turning a typed policy
/// into a residual fails on a node the typechecker left untyped
/// (`partial-evaluation.missing-type-annotation`) and on an unlinked slot
/// (`partial-evaluation.residual-unlinked-slot`).
[[nodiscard]] auto IsAuthorizedPartial(const PartialRequest& request, const PolicySet& policies,
                                       const PartialEntities& entities, const Schema& schema)
    -> std::expected<PartialAuthorizationResponse, PartialEvaluationError>;

// ---- Batched authorization ----

/// One entity a loader was asked for. A null `entity` means the entity does not exist.
struct LoadedEntity
{
    EntityUid uid;
    std::shared_ptr<const Entity> entity{};
};

/// What batched authorization needs from an entity source (Rust's `EntityLoader`): the entities
/// with the given uids, which arrive sorted and without repeats. The result must have an entry for
/// every uid asked for and may have entries for others. Each entity's ancestor set must already be
/// closed; it is taken as given.
template <typename Loader>
concept EntityLoader = requires(Loader& loader, std::span<const EntityUid> uids) {
    { loader.Load(uids) } -> std::same_as<std::vector<LoadedEntity>>;
};

/// A non-owning, type-erased reference to any EntityLoader: a pointer to the loader and one
/// function pointer, as EntityStoreView is for stores. The loader is called through a non-const
/// reference, so it may keep state between rounds; a const loader is called through a const one.
/// It must outlive the reference.
class EntityLoaderReference final
{
public:
    template <EntityLoader Loader>
        requires(!std::same_as<std::remove_cvref_t<Loader>, EntityLoaderReference>)
    EntityLoaderReference(Loader& loader) noexcept
        : loader_{const_cast<void*>(static_cast<const void*>(std::addressof(loader)))},
          load_{[](void* erased, std::span<const EntityUid> uids) -> std::vector<LoadedEntity> {
              return static_cast<Loader*>(erased)->Load(uids);
          }}
    {
    }

    [[nodiscard]] auto Load(std::span<const EntityUid> uids) const -> std::vector<LoadedEntity>
    {
        return load_(loader_, uids);
    }

private:
    void* loader_;
    auto (*load_)(void*, std::span<const EntityUid>) -> std::vector<LoadedEntity>;
};

static_assert(EntityLoader<EntityLoaderReference>);

/// An EntityLoader over any EntityStore (Rust's `TestEntityLoader`): it copies each entity the
/// store has, sharing its value storage, and reports the rest as missing. The store's ancestor
/// sets must be closed, as EntityLoader requires; an `Entities` built with AssumeAlreadyComputed
/// from unclosed sets breaks that. The store must outlive the loader.
class EntityStoreLoader final
{
public:
    explicit EntityStoreLoader(EntityStoreView store) noexcept : store_{store} {}

    [[nodiscard]] auto Load(std::span<const EntityUid> uids) const -> std::vector<LoadedEntity>;

private:
    EntityStoreView store_;
};

static_assert(EntityLoader<EntityStoreLoader>);

/// Authorizes `request` against `policies`, loading entities on demand from `loader` (Rust's
/// `PolicySet::is_authorized_batched`). Each round partially authorizes with the entities loaded
/// so far and, while there is no decision, asks the loader for the entity literals of the residuals
/// not yet loaded; a missing entity counts as one with no attributes, ancestors or tags. The loader
/// is called at most `maximumIterations` times. Fails as IsAuthorizedPartial does, when the
/// request does not conform to the schema (`request.*` codes), when a loaded entity is rejected,
/// and with `partial-evaluation.insufficient-iterations` when the rounds run out undecided.
[[nodiscard]] auto IsAuthorizedBatched(const Request& request, const PolicySet& policies, const Schema& schema,
                                       EntityLoaderReference loader, std::uint32_t maximumIterations)
    -> std::expected<Decision, BatchedAuthorizationError>;

// ---- Permission queries ----

/// A request whose principal id is unknown, for QueryPrincipal (Rust's `PrincipalQueryRequest`).
class PrincipalQueryRequest final
{
public:
    /// The query request checked against `schema`, as PartialRequest::Create checks it.
    [[nodiscard]] static auto Create(EntityType principalType, EntityUid action, EntityUid resource, Context context,
                                     const Schema& schema)
        -> std::expected<PrincipalQueryRequest, RequestValidationError>;

    [[nodiscard]] auto GetPartialRequest() const noexcept -> const PartialRequest& { return request_; }

    /// The concrete request with principal `principalId`, not checked (Rust's `to_request` without a
    /// schema).
    [[nodiscard]] auto ToRequest(Eid principalId) const -> Request;

    /// The concrete request with principal `principalId`, checked against `schema` as
    /// Request::Create checks, which also checks that an id of an enumerated type is one of its
    /// listed ids (Rust's `to_request` with a schema).
    [[nodiscard]] auto ToRequest(Eid principalId, const Schema& schema) const
        -> std::expected<Request, RequestValidationError>;

private:
    explicit PrincipalQueryRequest(PartialRequest request) noexcept : request_{std::move(request)} {}

    PartialRequest request_;
};

/// A request whose resource id is unknown, for QueryResource (Rust's `ResourceQueryRequest`).
class ResourceQueryRequest final
{
public:
    /// The query request checked against `schema`, as PartialRequest::Create checks it.
    [[nodiscard]] static auto Create(EntityUid principal, EntityUid action, EntityType resourceType, Context context,
                                     const Schema& schema)
        -> std::expected<ResourceQueryRequest, RequestValidationError>;

    [[nodiscard]] auto GetPartialRequest() const noexcept -> const PartialRequest& { return request_; }

    /// The concrete request with resource `resourceId`, not checked.
    [[nodiscard]] auto ToRequest(Eid resourceId) const -> Request;

    /// The concrete request with resource `resourceId`, checked as PrincipalQueryRequest::ToRequest
    /// checks.
    [[nodiscard]] auto ToRequest(Eid resourceId, const Schema& schema) const
        -> std::expected<Request, RequestValidationError>;

private:
    explicit ResourceQueryRequest(PartialRequest request) noexcept : request_{std::move(request)} {}

    PartialRequest request_;
};

/// A request with no action, for QueryAction (Rust's `ActionQueryRequest`). It is not checked when
/// made, because checking needs an action; QueryAction checks it once per action. It keeps a copy
/// of the schema, which shares it.
class ActionQueryRequest final
{
public:
    ActionQueryRequest(PartialEntityUid principal, PartialEntityUid resource, std::optional<Context> context,
                       Schema schema) noexcept
        : principal_{std::move(principal)}, resource_{std::move(resource)}, context_{std::move(context)},
          schema_{std::move(schema)}
    {
    }

    [[nodiscard]] auto Principal() const noexcept -> const PartialEntityUid& { return principal_; }
    [[nodiscard]] auto Resource() const noexcept -> const PartialEntityUid& { return resource_; }
    [[nodiscard]] auto GetContext() const noexcept -> const std::optional<Context>& { return context_; }
    [[nodiscard]] auto GetSchema() const noexcept -> const Schema& { return schema_; }

private:
    PartialEntityUid principal_;
    PartialEntityUid resource_;
    std::optional<Context> context_;
    Schema schema_;
};

/// One action QueryAction found: Allow when it is authorized whatever the unknowns turn out to be,
/// nothing when that depends on them.
struct ActionQueryResult
{
    EntityUid action;
    std::optional<Decision> decision{};
};

/// The entities of the principal type that `request` allows (Rust's `query_principal`), sorted by
/// uid. The schema must be complete (`validation.incomplete-schema`, checked first). `entities` are
/// made partial with PartialEntities::FromConcrete, then the request is partially authorized: on
/// Allow every entity of the principal type is returned, on Deny none, and otherwise each one whose
/// concrete request IsAuthorized allows against the residual policies.
[[nodiscard]] auto QueryPrincipal(const PrincipalQueryRequest& request, const PolicySet& policies,
                                  const Entities& entities, const Schema& schema)
    -> std::expected<std::vector<EntityUid>, PermissionQueryError>;

/// The entities of the resource type that `request` allows (Rust's `query_resource`), sorted by
/// uid, found as QueryPrincipal finds principals.
[[nodiscard]] auto QueryResource(const ResourceQueryRequest& request, const PolicySet& policies,
                                 const Entities& entities, const Schema& schema)
    -> std::expected<std::vector<EntityUid>, PermissionQueryError>;

/// The actions that may be authorized for `request` (Rust's `query_action`), sorted by action
/// uid. The schema must be complete (`validation.incomplete-schema`). Only the actions that apply to
/// the request's principal and resource types are tried, and an action whose partial request does
/// not validate is skipped. An action whose partial authorization decides Deny is left out; the
/// rest are returned with their decision, Allow or nothing. When partial authorization fails for
/// an action tried, the query fails as IsAuthorizedPartial does.
[[nodiscard]] auto QueryAction(const ActionQueryRequest& request, const PolicySet& policies,
                               const PartialEntities& entities)
    -> std::expected<std::vector<ActionQueryResult>, PermissionQueryError>;

} // namespace cedar
