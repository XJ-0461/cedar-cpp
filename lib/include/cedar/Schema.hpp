#pragma once

#include <cedar/Diagnostic.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Errors.hpp>
#include <cedar/Names.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar
{

namespace detail
{

class SchemaFragmentModel;
class ValidatorSchema;
struct SchemaAccess;

} // namespace detail

/// A value together with the warnings reported while making it. Reading a schema in Cedar syntax
/// can warn (a declaration that shadows a built-in type, say) without failing.
template <typename Content>
struct WithWarnings
{
    Content value;
    std::vector<Diagnostic> warnings{};
};

// ---- Schema fragments ----

/// A schema as written, before linking (Rust's `SchemaFragment`): namespaces, common types, entity
/// types and actions with their annotations, references not yet resolved. Both schema formats read
/// into this one model, and both printers write from it, so a schema translates between the
/// formats without losing its common types or namespace layout. Immutable; copies share it.
class SchemaFragment final
{
private:
    friend struct detail::SchemaAccess;

    explicit SchemaFragment(std::shared_ptr<const detail::SchemaFragmentModel> model) noexcept
        : model_{std::move(model)}
    {
    }

    std::shared_ptr<const detail::SchemaFragmentModel> model_;
};

/// Structural equality of the two fragments' models (Rust's `SchemaFragment` equality). Copies of one
/// fragment compare equal without comparing contents.
[[nodiscard]] auto operator==(const SchemaFragment& left, const SchemaFragment& right) -> bool;

/// Reads a schema fragment in Cedar schema syntax (Rust's `SchemaFragment::from_cedarschema_str`).
/// `sourceName` names the input in diagnostics.
[[nodiscard]] auto ParseSchemaFragment(std::string_view text, std::string_view sourceName = "<schema>")
    -> std::expected<WithWarnings<SchemaFragment>, SchemaError>;

/// Reads a schema fragment in the JSON schema format (Rust's `SchemaFragment::from_json_str`).
[[nodiscard]] auto SchemaFragmentFromJson(std::string_view text, std::string_view sourceName = "<schema>")
    -> std::expected<SchemaFragment, SchemaError>;

/// The fragment in the JSON schema format. Every fragment has a JSON form.
[[nodiscard]] auto ToJson(const SchemaFragment& fragment) -> std::string;

/// The fragment in the JSON schema format with every ambiguous `EntityOrCommon` type resolved to an
/// entity type or a common type (Rust's `schema_str_to_json_with_resolved_types`, `api.rs:2290`, used
/// by the CLI's `translate-schema`, `translate_schema.rs:88`). Fails when a type reference cannot be
/// resolved.
[[nodiscard]] auto ToJsonWithResolvedTypes(const SchemaFragment& fragment) -> std::expected<std::string, SchemaError>;

/// The fragment in Cedar schema syntax (Rust's `SchemaFragment::to_cedarschema`). Fails with
/// `schema.name-collisions` when a name would be ambiguous in Cedar syntax and with
/// `schema.unconvertible-entity-type-shape` for an entity shape that is not a record.
[[nodiscard]] auto ToCedar(const SchemaFragment& fragment) -> std::expected<std::string, SchemaError>;

// ---- Request environments ----

/// One combination of principal type, action and resource type that a schema allows (Rust's
/// `RequestEnv`). For a template the slot types are the types each slot may be linked to.
struct RequestEnvironment
{
    EntityType principal;
    EntityUid action;
    EntityType resource;
    std::optional<EntityType> principalSlot{};
    std::optional<EntityType> resourceSlot{};
};

// ---- Schema ----

/// A linked, well-formed schema (Rust's `Schema` over `ValidatorSchema`): every type reference
/// resolved, the action hierarchy closed, the extension types and the `__cedar` built-ins in place.
/// It is what the validator, schema-based entity and context reading, and request validation work
/// against. Immutable and safe to share between threads; copies share it.
///
/// The introspection functions return their results sorted (entity types by Name order, actions
/// by EntityUid order), so output built from them is deterministic.
class Schema final
{
public:
    /// Links `fragments` into one schema (Rust's `Schema::from_schema_fragments`). Declarations in
    /// one fragment may refer to those in another.
    [[nodiscard]] static auto FromFragments(std::span<const SchemaFragment> fragments)
        -> std::expected<Schema, SchemaError>;

    /// The schema's action entities, with their declared attributes and closed ancestor sets
    /// (Rust's `Schema::action_entities`).
    [[nodiscard]] auto ActionEntities() const -> std::expected<Entities, EntitiesError>;

    /// Every entity type declared, including enumerated and external ones.
    [[nodiscard]] auto EntityTypes() const -> std::vector<EntityType>;

    /// Every action declared.
    [[nodiscard]] auto Actions() const -> std::vector<EntityUid>;

    /// Every action that is the parent of another action.
    [[nodiscard]] auto ActionGroups() const -> std::vector<EntityUid>;

    /// Every entity type that some action allows as its principal.
    [[nodiscard]] auto Principals() const -> std::vector<EntityType>;

    /// Every entity type that some action allows as its resource.
    [[nodiscard]] auto Resources() const -> std::vector<EntityType>;

    /// The principal types `action` applies to, or nothing when the schema does not declare it.
    [[nodiscard]] auto PrincipalsForAction(const EntityUid& action) const -> std::optional<std::vector<EntityType>>;

    /// The resource types `action` applies to, or nothing when the schema does not declare it.
    [[nodiscard]] auto ResourcesForAction(const EntityUid& action) const -> std::optional<std::vector<EntityType>>;

    /// The actions that apply to a principal of `principalType` and a resource of `resourceType`.
    [[nodiscard]] auto ActionsForPrincipalAndResource(const EntityType& principalType,
                                                      const EntityType& resourceType) const -> std::vector<EntityUid>;

    /// The entity types an entity of `type` may have as an ancestor, or nothing when the schema does
    /// not declare `type`.
    [[nodiscard]] auto Ancestors(const EntityType& type) const -> std::optional<std::vector<EntityType>>;

    /// Every principal type, action and resource type combination the schema allows (Rust's
    /// `Schema::request_envs`).
    [[nodiscard]] auto RequestEnvironments() const -> std::vector<RequestEnvironment>;

    /// False when the schema declares an entity type or action `external`, which a schema cannot
    /// validate against (`validation.incomplete-schema`).
    [[nodiscard]] auto IsComplete() const -> bool;

private:
    friend struct detail::SchemaAccess;

    explicit Schema(std::shared_ptr<const detail::ValidatorSchema> schema) noexcept : schema_{std::move(schema)} {}

    std::shared_ptr<const detail::ValidatorSchema> schema_;
};

/// Reads and links a schema in Cedar schema syntax (Rust's `Schema::from_cedarschema_str`).
/// `sourceName` names the input in diagnostics.
[[nodiscard]] auto ParseSchema(std::string_view text, std::string_view sourceName = "<schema>")
    -> std::expected<WithWarnings<Schema>, SchemaError>;

/// Reads and links a schema in the JSON schema format (Rust's `Schema::from_json_str`).
[[nodiscard]] auto SchemaFromJson(std::string_view text, std::string_view sourceName = "<schema>")
    -> std::expected<Schema, SchemaError>;

} // namespace cedar
