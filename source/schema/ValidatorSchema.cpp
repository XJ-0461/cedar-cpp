#include "schema/ValidatorSchema.hpp"

#include "schema/SchemaAccess.hpp"

#include <cedar/Entities.hpp>
#include <cedar/Schema.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace cedar
{

namespace detail
{

namespace
{

/// The items ordered by their `name` member. Sorts a permutation of indices and then moves each item
/// once into a fresh vector, so no element is ever swapped or held in a sort temporary.
template <typename Item, typename Name>
[[nodiscard]] auto OrderedByName(std::vector<Item> items, Name Item::* const name) -> std::vector<Item>
{
    std::vector<std::size_t> order(items.size());
    std::ranges::iota(order, std::size_t{0});
    std::ranges::sort(order, [&](const std::size_t left, const std::size_t right)
                      { return items[left].*name < items[right].*name; });
    std::vector<Item> ordered;
    ordered.reserve(items.size());
    for (const std::size_t index : order) { ordered.push_back(std::move(items[index])); }
    return ordered;
}

} // namespace

ValidatorSchema::ValidatorSchema(std::vector<ValidatorEntityType> entityTypes, std::vector<ValidatorAction> actions)
    : entityTypes_{OrderedByName(std::move(entityTypes), &ValidatorEntityType::name)},
      actions_{OrderedByName(std::move(actions), &ValidatorAction::name)}
{
    entityTypeIndex_.reserve(entityTypes_.size());
    for (std::uint32_t index = 0; index < entityTypes_.size(); ++index)
    {
        entityTypeIndex_.emplace(entityTypes_[index].name, index);
        complete_ = complete_ && entityTypes_[index].kind != ValidatorEntityTypeKind::External;
    }
    actionIndex_.reserve(actions_.size());
    for (std::uint32_t index = 0; index < actions_.size(); ++index)
    {
        ValidatorAction& action = actions_[index];
        actionIndex_.emplace(action.name, index);
        complete_ = complete_ && !action.external;
        // Rust gives the closed ancestor set as the entity's parents (`action_entities_iter`).
        action.entity = std::make_shared<const Entity>(action.name, action.ancestors);
        if (action.external) { continue; }
        actionEntities_.push_back(action.entity);
        for (const EntityType& principal : action.principalTypes)
        {
            for (const EntityType& resource : action.resourceTypes)
            {
                environments_.push_back(UnlinkedRequestEnvironment{principal, &action, resource});
            }
        }
    }
}

auto ValidatorSchema::FindEntityType(const EntityType& type) const noexcept -> const ValidatorEntityType*
{
    const auto found = entityTypeIndex_.find(type);
    return found == entityTypeIndex_.end() ? nullptr : &entityTypes_[found->second];
}

auto ValidatorSchema::FindAction(const EntityUid& action) const noexcept -> const ValidatorAction*
{
    const auto found = actionIndex_.find(action);
    return found == actionIndex_.end() ? nullptr : &actions_[found->second];
}

} // namespace detail

namespace
{

template <typename Element>
void SortUnique(std::vector<Element>& elements)
{
    std::ranges::sort(elements);
    const auto [first, last] = std::ranges::unique(elements);
    elements.erase(first, last);
}

} // namespace

auto Schema::ActionEntities() const -> std::expected<Entities, EntitiesError>
{
    return Entities{}.Add(schema_->ActionEntities(), TransitiveClosureMode::AssumeAlreadyComputed);
}

auto Schema::EntityTypes() const -> std::vector<EntityType>
{
    std::vector<EntityType> types;
    types.reserve(schema_->EntityTypes().size());
    for (const detail::ValidatorEntityType& type : schema_->EntityTypes()) { types.push_back(type.name); }
    return types;
}

auto Schema::Actions() const -> std::vector<EntityUid>
{
    std::vector<EntityUid> actions;
    actions.reserve(schema_->Actions().size());
    for (const detail::ValidatorAction& action : schema_->Actions()) { actions.push_back(action.name); }
    return actions;
}

auto Schema::ActionGroups() const -> std::vector<EntityUid>
{
    std::vector<EntityUid> groups;
    for (const detail::ValidatorAction& action : schema_->Actions())
    {
        if (!action.descendants.empty()) { groups.push_back(action.name); }
    }
    return groups;
}

auto Schema::Principals() const -> std::vector<EntityType>
{
    std::vector<EntityType> types;
    for (const detail::ValidatorAction& action : schema_->Actions())
    {
        types.insert(types.end(), action.principalTypes.begin(), action.principalTypes.end());
    }
    SortUnique(types);
    return types;
}

auto Schema::Resources() const -> std::vector<EntityType>
{
    std::vector<EntityType> types;
    for (const detail::ValidatorAction& action : schema_->Actions())
    {
        types.insert(types.end(), action.resourceTypes.begin(), action.resourceTypes.end());
    }
    SortUnique(types);
    return types;
}

auto Schema::PrincipalsForAction(const EntityUid& action) const -> std::optional<std::vector<EntityType>>
{
    const detail::ValidatorAction* found = schema_->FindAction(action);
    if (found == nullptr) { return std::nullopt; }
    return found->principalTypes;
}

auto Schema::ResourcesForAction(const EntityUid& action) const -> std::optional<std::vector<EntityType>>
{
    const detail::ValidatorAction* found = schema_->FindAction(action);
    if (found == nullptr) { return std::nullopt; }
    return found->resourceTypes;
}

auto Schema::ActionsForPrincipalAndResource(const EntityType& principalType, const EntityType& resourceType) const
    -> std::vector<EntityUid>
{
    std::vector<EntityUid> actions;
    for (const detail::ValidatorAction& action : schema_->Actions())
    {
        if (action.AppliesToPrincipal(principalType) && action.AppliesToResource(resourceType))
        {
            actions.push_back(action.name);
        }
    }
    return actions;
}

auto Schema::Ancestors(const EntityType& type) const -> std::optional<std::vector<EntityType>>
{
    const detail::ValidatorEntityType* found = schema_->FindEntityType(type);
    if (found == nullptr) { return std::nullopt; }
    return found->ancestors;
}

auto Schema::RequestEnvironments() const -> std::vector<RequestEnvironment>
{
    std::vector<RequestEnvironment> environments;
    environments.reserve(schema_->RequestEnvironments().size());
    for (const detail::UnlinkedRequestEnvironment& environment : schema_->RequestEnvironments())
    {
        environments.push_back(RequestEnvironment{environment.principal, environment.action->name, environment.resource});
    }
    return environments;
}

auto Schema::IsComplete() const -> bool { return schema_->IsComplete(); }

} // namespace cedar
