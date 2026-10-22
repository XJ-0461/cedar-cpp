// Partial entities (Rust's `cedar-policy-core/src/tpe/entities.rs`): validation against the
// schema, the known-ancestors rule, the closure of known ancestor sets, the schema's action
// entities, and consistency with concrete entities.

#include "partial-evaluation/PartialEvaluationAccess.hpp"

#include "entities/Conformance.hpp"
#include "entities/TransitiveClosure.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"

#include <cedar/PartialEvaluation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cedar
{

namespace
{

using detail::PartialEvaluationAccess;
using detail::ValidatorSchema;

[[nodiscard]] auto Problem(const DiagnosticCode code, std::string message) -> Diagnostic
{
    return Diagnostic{.code = code, .message = std::move(message)};
}

/// The whole ancestor set of a concrete entity: its parents and its ancestors.
[[nodiscard]] auto AllAncestors(const Entity& entity) -> std::vector<EntityUid>
{
    std::vector<EntityUid> ancestors(entity.Parents().begin(), entity.Parents().end());
    ancestors.insert(ancestors.end(), entity.Ancestors().begin(), entity.Ancestors().end());
    detail::SortUidSet(ancestors);
    return ancestors;
}

/// A concrete entity as a partial entity with every part known (Rust's `TryFrom<Entity>`).
[[nodiscard]] auto FromEntity(const Entity& entity) -> PartialEntity
{
    return PartialEvaluationAccess::MakeEntity(entity.Uid(), entity.Attributes(), AllAncestors(entity), entity.Tags(),
                                               entity.Storage());
}

/// Checks an action entity (the action arm of Rust's `PartialEntity::validate`): no attributes, no
/// tags, declared, and known ancestors equal to the schema's.
[[nodiscard]] auto ValidateAction(const PartialEntity& entity, const ValidatorSchema& schema)
    -> std::expected<void, Diagnostic>
{
    const EntityUid& uid = entity.Uid();
    if (const auto attributes = entity.Attributes(); attributes && !attributes->IsEmpty())
    {
        return std::unexpected{Problem(DiagnosticCode::UnexpectedEntityAttribute,
                                       std::format("`{}` has the attribute `{}`, which the schema does not declare",
                                                   uid.ToString(), attributes->begin()->key))};
    }
    if (const auto tags = entity.Tags(); tags && !tags->IsEmpty())
    {
        return std::unexpected{
            Problem(DiagnosticCode::UnexpectedEntityTag,
                    std::format("`{}` has the tag `{}`, but the schema declares no tags for its type", uid.ToString(),
                                tags->begin()->key))};
    }
    const detail::ValidatorAction* const action = schema.FindAction(uid);
    if (action == nullptr)
    {
        return std::unexpected{
            detail::DescribeInvalidEntityUid({.uid = uid, .problem = detail::EntityUidProblem::UndeclaredAction})};
    }
    if (const auto ancestors = entity.Ancestors())
    {
        std::vector<EntityUid> declared = action->ancestors;
        detail::SortUidSet(declared);
        if (!std::ranges::equal(declared, *ancestors))
        {
            return std::unexpected{
                Problem(DiagnosticCode::MismatchedActionAncestors,
                        std::format("the ancestors of action `{}` differ from those the schema declares",
                                    uid.ToString()))};
        }
    }
    return {};
}

/// Rust's `PartialEntity::validate`: an action as ValidateAction checks it; any other entity of a
/// declared, non-external type, with known ancestors, attributes and tags that conform.
[[nodiscard]] auto Validate(const PartialEntity& entity, const ValidatorSchema& schema) -> std::expected<void, Diagnostic>
{
    const EntityUid& uid = entity.Uid();
    if (uid.IsAction()) { return ValidateAction(entity, schema); }
    auto declaration = detail::CheckEntityDeclaration(schema, uid);
    if (!declaration) { return std::unexpected{std::move(declaration).error()}; }
    if (const auto ancestors = entity.Ancestors())
    {
        if (auto checked = detail::CheckEntityAncestors(schema, uid, **declaration, *ancestors); !checked)
        {
            return checked;
        }
    }
    if (const auto attributes = entity.Attributes())
    {
        if (auto checked = detail::CheckEntityAttributes(schema, uid, **declaration, *attributes); !checked)
        {
            return checked;
        }
    }
    if (const auto tags = entity.Tags()) { return detail::CheckEntityTags(schema, uid, **declaration, *tags); }
    return {};
}

/// The entity `uid` in `entities`, sorted by uid, or null.
[[nodiscard]] auto FindIn(std::span<PartialEntity> entities, const EntityUid& uid) noexcept -> PartialEntity*
{
    const auto found = std::ranges::lower_bound(entities, uid, {}, &PartialEntity::Uid);
    if (found == entities.end() || found->Uid() != uid) { return nullptr; }
    return &*found;
}

/// Rust's `validate_concrete_ancestors_concrete`: no entity with known ancestors has an ancestor in
/// the store whose own ancestors are unknown.
[[nodiscard]] auto CheckKnownAncestors(std::span<PartialEntity> entities) -> std::expected<void, Diagnostic>
{
    for (const PartialEntity& entity : entities)
    {
        const auto ancestors = entity.Ancestors();
        if (!ancestors) { continue; }
        for (const EntityUid& ancestor : *ancestors)
        {
            const PartialEntity* const found = FindIn(entities, ancestor);
            if (found != nullptr && !found->Ancestors())
            {
                return std::unexpected{Problem(
                    DiagnosticCode::AncestorValidation,
                    std::format("`{}` has the ancestor `{}`, whose own ancestors are unknown", entity.Uid().ToString(),
                                ancestor.ToString()))};
            }
        }
    }
    return {};
}

/// Closes every known ancestor set and refuses a cycle (Rust's `compute_tc` with the DAG check).
[[nodiscard]] auto CloseAncestors(std::span<PartialEntity> entities) -> std::expected<void, Diagnostic>
{
    // The nodes are the entities whose ancestors are known; an unknown set has no edges to follow.
    std::vector<std::size_t> nodes;
    std::vector<std::optional<std::size_t>> nodeOf(entities.size());
    for (std::size_t index = 0; index < entities.size(); ++index)
    {
        if (!entities[index].Ancestors()) { continue; }
        nodeOf[index] = nodes.size();
        nodes.push_back(index);
    }
    detail::CloseAncestorSets(
        nodes.size(),
        [&](const std::size_t node) -> std::vector<EntityUid>& {
            return *PartialEvaluationAccess::Ancestors(entities[nodes[node]]);
        },
        [&](const EntityUid& uid) -> detail::AncestorLookup {
            PartialEntity* const found = FindIn(entities, uid);
            if (found == nullptr) { return {}; }
            const auto index = static_cast<std::size_t>(found - entities.data());
            if (!nodeOf[index]) { return {}; }
            return detail::AncestorLookup{.ancestors = &*PartialEvaluationAccess::Ancestors(*found),
                                          .node = nodeOf[index]};
        });
    for (const PartialEntity& entity : entities)
    {
        if (const auto ancestors = entity.Ancestors(); ancestors && ancestors->Contains(entity.Uid()))
        {
            return std::unexpected{
                Problem(DiagnosticCode::TransitiveClosureCycle,
                        std::format("the entity hierarchy has a cycle through `{}`", entity.Uid().ToString()))};
        }
    }
    return {};
}

/// Rust's `from_entities_map`: `entities`, no uid twice, validated in order, the known-ancestors
/// rule, the closure when `computeClosure`, then the schema's action entities in place of any
/// given ones.
[[nodiscard]] auto FromUniqueEntities(std::vector<PartialEntity> entities, const ValidatorSchema& schema,
                                      const bool computeClosure) -> std::expected<PartialEntities, PartialEntitiesError>
{
    for (const PartialEntity& entity : entities)
    {
        if (auto checked = Validate(entity, schema); !checked)
        {
            return std::unexpected{PartialEntitiesError{std::move(checked).error()}};
        }
    }
    std::ranges::sort(entities, {}, &PartialEntity::Uid);
    if (auto checked = CheckKnownAncestors(entities); !checked)
    {
        return std::unexpected{PartialEntitiesError{std::move(checked).error()}};
    }
    if (computeClosure)
    {
        if (auto closed = CloseAncestors(entities); !closed)
        {
            return std::unexpected{PartialEntitiesError{std::move(closed).error()}};
        }
    }
    for (const std::shared_ptr<const Entity>& action : schema.ActionEntities())
    {
        PartialEntity entity = FromEntity(*action);
        const auto position = std::ranges::lower_bound(entities, entity.Uid(), {}, &PartialEntity::Uid);
        if (position != entities.end() && position->Uid() == entity.Uid()) { *position = std::move(entity); }
        else { entities.insert(position, std::move(entity)); }
    }
    return PartialEvaluationAccess::MakeEntities(std::make_shared<const std::vector<PartialEntity>>(std::move(entities)));
}

} // namespace

auto PartialEntity::Create(EntityUid uid, std::optional<Record> attributes,
                           std::optional<std::vector<EntityUid>> ancestors, std::optional<Record> tags,
                           std::shared_ptr<const ValueArena> storage, const Schema& schema)
    -> std::expected<PartialEntity, PartialEntitiesError>
{
    if (ancestors) { detail::SortUidSet(*ancestors); }
    PartialEntity entity = PartialEvaluationAccess::MakeEntity(std::move(uid), attributes, std::move(ancestors), tags,
                                                               std::move(storage));
    if (auto checked = Validate(entity, detail::SchemaAccess::Validator(schema)); !checked)
    {
        return std::unexpected{PartialEntitiesError{std::move(checked).error()}};
    }
    return entity;
}

auto PartialEntities::Empty() noexcept -> PartialEntities
{
    return PartialEntities{nullptr};
}

auto PartialEntities::Create(std::vector<PartialEntity> entities, const Schema& schema)
    -> std::expected<PartialEntities, PartialEntitiesError>
{
    std::set<EntityUid> seen;
    for (const PartialEntity& entity : entities)
    {
        if (!seen.insert(entity.Uid()).second)
        {
            return std::unexpected{PartialEntitiesError{
                Problem(DiagnosticCode::DuplicateEntity,
                        std::format("the entity `{}` is given more than once", entity.Uid().ToString()))}};
        }
    }
    return FromUniqueEntities(std::move(entities), detail::SchemaAccess::Validator(schema), true);
}

auto PartialEntities::FromConcrete(const Entities& entities, const Schema& schema)
    -> std::expected<PartialEntities, PartialEntitiesError>
{
    std::vector<PartialEntity> partial;
    for (const Entity& entity : entities) { partial.push_back(FromEntity(entity)); }
    return FromUniqueEntities(std::move(partial), detail::SchemaAccess::Validator(schema), false);
}

auto PartialEntities::Find(const EntityUid& uid) const& noexcept -> const PartialEntity*
{
    const PartialEntity* const first = begin();
    const PartialEntity* const last = end();
    const PartialEntity* const found = std::lower_bound(
        first, last, uid, [](const PartialEntity& entity, const EntityUid& key) { return entity.Uid() < key; });
    if (found == last || found->Uid() != uid) { return nullptr; }
    return found;
}

auto PartialEntities::Contains(const EntityUid& uid) const noexcept -> bool
{
    return Find(uid) != nullptr;
}

auto PartialEntities::Size() const noexcept -> std::size_t
{
    return entities_ ? entities_->size() : 0;
}

auto PartialEntities::begin() const& noexcept -> const PartialEntity*
{
    return entities_ ? entities_->data() : nullptr;
}

auto PartialEntities::end() const& noexcept -> const PartialEntity*
{
    return entities_ ? entities_->data() + entities_->size() : nullptr;
}

namespace detail
{

auto CheckEntitiesConsistency(const PartialEntities& partial, const Entities& concrete)
    -> std::expected<void, ReauthorizationError>
{
    const auto fail = [](const DiagnosticCode code, std::string message) {
        return std::unexpected{ReauthorizationError{Problem(code, std::move(message))}};
    };
    for (const PartialEntity& entity : partial)
    {
        const EntityUid& uid = entity.Uid();
        const Entity* const found = concrete.Find(uid);
        if (found == nullptr)
        {
            return fail(DiagnosticCode::MissingEntity,
                        std::format("the partial entity `{}` is missing from the concrete entities", uid.ToString()));
        }
        if (const auto attributes = entity.Attributes();
            attributes && Value::FromRecord(*attributes) != Value::FromRecord(found->Attributes()))
        {
            return fail(DiagnosticCode::MismatchedAttribute,
                        std::format("the attributes of `{}` differ from its partial entity's", uid.ToString()));
        }
        if (const auto ancestors = entity.Ancestors(); ancestors && !std::ranges::equal(*ancestors, AllAncestors(*found)))
        {
            return fail(DiagnosticCode::MismatchedAncestor,
                        std::format("the ancestors of `{}` differ from its partial entity's", uid.ToString()));
        }
        if (const auto tags = entity.Tags(); tags && Value::FromRecord(*tags) != Value::FromRecord(found->Tags()))
        {
            return fail(DiagnosticCode::MismatchedTag,
                        std::format("the tags of `{}` differ from its partial entity's", uid.ToString()));
        }
    }
    return {};
}

auto AddLoadedEntities(const PartialEntities& entities, const std::span<const LoadedEntity> loaded,
                       const Schema& schema) -> std::expected<PartialEntities, PartialEntitiesError>
{
    const ValidatorSchema& validator = SchemaAccess::Validator(schema);
    std::vector<PartialEntity> table(entities.begin(), entities.end());
    bool checkAncestors = false;
    for (const LoadedEntity& entry : loaded)
    {
        // A missing entity is one with every part known and empty, added without validation.
        PartialEntity entity = entry.entity == nullptr
                                   ? PartialEvaluationAccess::MakeEntity(entry.uid, Record{}, std::vector<EntityUid>{},
                                                                         Record{}, nullptr)
                                   : FromEntity(*entry.entity);
        if (entry.entity != nullptr)
        {
            if (auto checked = Validate(entity, validator); !checked)
            {
                return std::unexpected{PartialEntitiesError{std::move(checked).error()}};
            }
            checkAncestors = true;
        }
        const auto position = std::ranges::lower_bound(table, entry.uid, {}, &PartialEntity::Uid);
        if (position != table.end() && position->Uid() == entry.uid)
        {
            return std::unexpected{PartialEntitiesError{
                Problem(DiagnosticCode::DuplicateEntity,
                        std::format("the entity `{}` is loaded more than once", entry.uid.ToString()))}};
        }
        table.insert(position, std::move(entity));
    }
    if (checkAncestors)
    {
        if (auto checked = CheckKnownAncestors(table); !checked)
        {
            return std::unexpected{PartialEntitiesError{std::move(checked).error()}};
        }
    }
    return PartialEvaluationAccess::MakeEntities(std::make_shared<const std::vector<PartialEntity>>(std::move(table)));
}

} // namespace detail

} // namespace cedar
