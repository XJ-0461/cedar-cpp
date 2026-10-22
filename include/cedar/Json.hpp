#pragma once

#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cedar
{

// Cedar's JSON formats in and out: entities, contexts and entity uids in the entity JSON format
// (`__entity` and `__extn` escapes), and policies, templates and policy sets in the JSON policy
// format (Rust's EST). Every reader takes the JSON text first and, last, the name the input goes by
// in diagnostics. Duplicate object keys are rejected. A reader given a schema reads values by the
// declared types (so an entity reference or extension value may be written without its escape)
// and checks the input against the schema.

// ---- Entities ----

/// Reads a JSON array of entities and closes their ancestor sets (Rust's `Entities::from_json_str`
/// without a schema).
[[nodiscard]] auto EntitiesFromJson(std::string_view json, std::string_view sourceName = "<entities>")
    -> std::expected<Entities, EntitiesError>;

/// Reads a JSON array of entities by the types `schema` declares, checks each against its
/// declaration, closes their ancestor sets and adds the schema's action entities (Rust's
/// `Entities::from_json_str` with a schema).
[[nodiscard]] auto EntitiesFromJson(std::string_view json, const Schema& schema,
                                    std::string_view sourceName = "<entities>")
    -> std::expected<Entities, EntitiesError>;

/// Reads one JSON entity object (Rust's `Entity::from_json_str` without a schema).
[[nodiscard]] auto EntityFromJson(std::string_view json, std::string_view sourceName = "<entity>")
    -> std::expected<Entity, EntitiesError>;

/// Reads one JSON entity object by the types `schema` declares and checks it (Rust's
/// `Entity::from_json_str` with a schema).
[[nodiscard]] auto EntityFromJson(std::string_view json, const Schema& schema, std::string_view sourceName = "<entity>")
    -> std::expected<Entity, EntitiesError>;

/// The entities as a JSON array, in EntityUid order, each entity's `parents` listing its whole
/// ancestor set in EntityUid order (as Rust writes it). Fails on a value the format cannot hold.
[[nodiscard]] auto ToJson(const Entities& entities) -> std::expected<std::string, EntitiesError>;

/// The entity as a JSON object, written as ToJson(const Entities&) writes each entity.
[[nodiscard]] auto ToJson(const Entity& entity) -> std::expected<std::string, EntitiesError>;

// ---- Contexts ----

/// Reads a JSON object as a request context (Rust's `Context::from_json_str` without a schema).
[[nodiscard]] auto ContextFromJson(std::string_view json, std::string_view sourceName = "<context>")
    -> std::expected<Context, ContextError>;

/// Reads a JSON object as the context of `action`, by the context type `schema` declares for it,
/// and checks it (Rust's `Context::from_json_str` with a schema and action). Fails with
/// `request.context-missing-action` when the schema does not declare `action`.
[[nodiscard]] auto ContextFromJson(std::string_view json, const Schema& schema, const EntityUid& action,
                                   std::string_view sourceName = "<context>") -> std::expected<Context, ContextError>;

/// The context as a JSON object.
[[nodiscard]] auto ToJson(const Context& context) -> std::expected<std::string, ContextError>;

// ---- Entity uids ----

/// Reads an entity uid written as `{"type": ..., "id": ...}` or `{"__entity": {...}}` (Rust's
/// `EntityUid::from_json`).
[[nodiscard]] auto EntityUidFromJson(std::string_view json, std::string_view sourceName = "<entity uid>")
    -> std::expected<EntityUid, ParseError>;

/// The entity uid as `{"type": ..., "id": ...}`, the implicit form Rust writes for an entity's own
/// `uid` and `parents` (`entities/json/entities.rs:459-466`) and the form `EntityUidFromJson` reads
/// first. Inside a context or attribute value the entity is written with the `{"__entity": {...}}`
/// escape (Rust's `From<&EntityUID> for EntityUidJson`, `entities/json/value.rs:1067-1073`); that
/// form belongs to `ToJson(const Context&)` and `ToJson(const Entities&)`, not to this function.
[[nodiscard]] auto ToJson(const EntityUid& uid) -> std::string;

// ---- Policies ----

/// Reads one static policy in the JSON policy format (Rust's `Policy::from_json`). Without `id` the
/// policy is `JSON policy`.
[[nodiscard]] auto PolicyFromJson(std::string_view json, std::optional<PolicyId> id = std::nullopt,
                                  std::string_view sourceName = "<policy>") -> std::expected<Policy, ParseError>;

/// Reads one template in the JSON policy format (Rust's `Template::from_json`). Without `id` the
/// template is `JSON policy`.
[[nodiscard]] auto TemplateFromJson(std::string_view json, std::optional<PolicyId> id = std::nullopt,
                                    std::string_view sourceName = "<template>")
    -> std::expected<std::shared_ptr<const Template>, ParseError>;

/// Reads a policy set in the JSON policy format: `staticPolicies` and `templates` keyed by id, and
/// `templateLinks` (Rust's `PolicySet::from_json_str`).
[[nodiscard]] auto PolicySetFromJson(std::string_view json, std::string_view sourceName = "<policies>")
    -> std::expected<PolicySet, PolicySetError>;

/// The template in the JSON policy format.
[[nodiscard]] auto ToJson(const Template& body) -> std::string;

/// The policy in the JSON policy format. A template-linked policy is written as its template with
/// each slot replaced by the entity bound to it.
[[nodiscard]] auto ToJson(const Policy& policy) -> std::string;

/// The policy set in the JSON policy format, links kept as `templateLinks`.
[[nodiscard]] auto ToJson(const PolicySet& policies) -> std::string;

} // namespace cedar
