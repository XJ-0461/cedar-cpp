#pragma once

#include "validation/Types.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Names.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace cedar::detail
{

// The linked schema behind `Schema` (Rust's `ValidatorSchema`, `cedar-policy-core/src/validator/
// schema.rs`). Every name is fully qualified, common types are inlined into the types that use
// them, and the entity type and action hierarchies are transitively closed. SchemaLinker makes
// it; schema-based entity and context reading, request validation, the typechecker and the
// public introspection functions read it. Lookups are by EntityType and EntityUid, which compare
// and hash as interned symbols.

/// What an entity type declaration declares (Rust's `ValidatorEntityTypeKind`).
enum class ValidatorEntityTypeKind : std::uint8_t
{
    /// Attributes, optional tags.
    Standard,
    /// Only the listed entity ids exist; no attributes, no tags (RFC 53).
    Enumerated,
    /// Declared `external` (RFC 116): only its parents are known here.
    External,
};

/// One entity type of a linked schema (Rust's `ValidatorEntityType`).
struct ValidatorEntityType
{
    EntityType name;
    ValidatorEntityTypeKind kind = ValidatorEntityTypeKind::Standard;
    /// The shape's attributes; none unless Standard.
    Attributes attributes{};
    /// Always closed in a schema the linker accepts (open records are an experimental feature).
    OpenTag openAttributes = OpenTag::ClosedAttributes;
    /// The tag type; nothing means the entities have no tags. Only a Standard type has one.
    std::optional<Type> tags{};
    /// The allowed entity ids of an Enumerated type, as written (a repeated id is kept).
    std::vector<Eid> enumChoices{};
    /// Every entity type that may be a descendant of this one, transitively, in Name order. A type
    /// on a hierarchy cycle is its own descendant.
    std::vector<EntityType> descendants{};
    /// Every entity type this one may be a descendant of, transitively, in Name order: the types
    /// an entity of this type may have as parents (Rust's `allowed_parent_types`).
    std::vector<EntityType> ancestors{};
    /// The declaration, when the schema was read from Cedar syntax.
    std::optional<SourceSpan> span{};

    [[nodiscard]] auto HasDescendant(const EntityType& type) const noexcept -> bool
    {
        return std::ranges::find(descendants, type) != descendants.end();
    }

    [[nodiscard]] auto AllowsParentType(const EntityType& type) const noexcept -> bool
    {
        return std::ranges::find(ancestors, type) != ancestors.end();
    }

    /// True when `id` is one of an Enumerated type's ids. Always false for other kinds.
    [[nodiscard]] auto IsEnumChoice(const Eid& id) const noexcept -> bool
    {
        return std::ranges::find(enumChoices, id) != enumChoices.end();
    }
};

/// One action of a linked schema (Rust's `ValidatorActionId`).
struct ValidatorAction
{
    EntityUid name;
    /// Declared `external` (RFC 116): no context and nothing it applies to.
    bool external = false;
    /// The principal and resource types it applies to, in Name order, each once.
    std::vector<EntityType> principalTypes{};
    std::vector<EntityType> resourceTypes{};
    /// The context type, always a closed record; nothing for an external action.
    std::optional<Type> context{};
    /// Every action that is a descendant of this one, transitively, in EntityUid order.
    std::vector<EntityUid> descendants{};
    /// Every action this one is a descendant of, transitively, in EntityUid order.
    std::vector<EntityUid> ancestors{};
    /// The action as an entity: no attributes, no tags, `ancestors` as its parents (Rust's
    /// `ValidatorSchema::actions`). Set by ValidatorSchema.
    std::shared_ptr<const Entity> entity{};
    std::optional<SourceSpan> span{};

    [[nodiscard]] auto AppliesToPrincipal(const EntityType& type) const noexcept -> bool
    {
        return std::ranges::find(principalTypes, type) != principalTypes.end();
    }

    [[nodiscard]] auto AppliesToResource(const EntityType& type) const noexcept -> bool
    {
        return std::ranges::find(resourceTypes, type) != resourceTypes.end();
    }

    [[nodiscard]] auto HasDescendant(const EntityUid& action) const noexcept -> bool
    {
        return std::ranges::binary_search(descendants, action);
    }
};

/// A principal type, an action and a resource type the schema allows together, with no slots
/// (Rust's `UnlinkedRequestEnv`). Points into the schema that made it.
struct UnlinkedRequestEnvironment
{
    EntityType principal;
    const ValidatorAction* action;
    EntityType resource;

    [[nodiscard]] auto Context() const noexcept -> const Type& { return *action->context; }
};

class ValidatorSchema final
{
public:
    /// Takes linked declarations: each name once, every reference resolved, `descendants` and
    /// `ancestors` closed. Sorts them, indexes them, makes each action's entity and the request
    /// environments.
    ValidatorSchema(std::vector<ValidatorEntityType> entityTypes, std::vector<ValidatorAction> actions);

    ValidatorSchema(const ValidatorSchema&) = delete;
    auto operator=(const ValidatorSchema&) -> ValidatorSchema& = delete;

    /// The declared entity type `type`, or null. An action type is not an entity type here.
    [[nodiscard]] auto FindEntityType(const EntityType& type) const noexcept -> const ValidatorEntityType*;

    /// The declared action `action`, or null.
    [[nodiscard]] auto FindAction(const EntityUid& action) const noexcept -> const ValidatorAction*;

    /// True for a declared entity type and for every action type, declared or not (Rust's
    /// `is_known_entity_type`).
    [[nodiscard]] auto IsKnownEntityType(const EntityType& type) const noexcept -> bool
    {
        return type.IsAction() || FindEntityType(type) != nullptr;
    }

    /// Every entity type, in Name order.
    [[nodiscard]] auto EntityTypes() const noexcept -> std::span<const ValidatorEntityType> { return entityTypes_; }

    /// Every action, in EntityUid order.
    [[nodiscard]] auto Actions() const noexcept -> std::span<const ValidatorAction> { return actions_; }

    /// The entities of the actions that are not external, in EntityUid order (Rust's
    /// `action_entities`), for `Entities::Add(span<shared_ptr<const Entity>>, AssumeAlreadyComputed)`.
    [[nodiscard]] auto ActionEntities() const noexcept -> std::span<const std::shared_ptr<const Entity>>
    {
        return actionEntities_;
    }

    /// For every action that is not external, every principal type and resource type it applies to,
    /// by action in EntityUid order, then principal, then resource in Name order.
    [[nodiscard]] auto RequestEnvironments() const noexcept -> std::span<const UnlinkedRequestEnvironment>
    {
        return environments_;
    }

    /// False when some entity type or action is external (Rust's `is_complete`).
    [[nodiscard]] auto IsComplete() const noexcept -> bool { return complete_; }

private:
    std::vector<ValidatorEntityType> entityTypes_;
    std::vector<ValidatorAction> actions_;
    std::unordered_map<EntityType, std::uint32_t> entityTypeIndex_;
    std::unordered_map<EntityUid, std::uint32_t> actionIndex_;
    std::vector<std::shared_ptr<const Entity>> actionEntities_;
    std::vector<UnlinkedRequestEnvironment> environments_;
    bool complete_ = true;
};

} // namespace cedar::detail
