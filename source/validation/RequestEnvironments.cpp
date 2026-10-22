#include "validation/RequestEnvironments.hpp"

#include <utility>
#include <variant>

namespace cedar::detail
{

auto PossibleSlotLinks(const ValidatorSchema& schema, const Template& body, const SlotId slot,
                       const EntityType& variableType, const PrincipalOrResourceConstraint& constraint)
    -> std::vector<std::optional<EntityType>>
{
    if (!body.HasSlot(slot)) { return {std::nullopt}; }
    std::vector<std::optional<EntityType>> links;
    if (std::holds_alternative<ScopeEqual>(constraint))
    {
        links.emplace_back(variableType);
        return links;
    }
    const bool descendant = std::holds_alternative<ScopeIn>(constraint) || std::holds_alternative<ScopeIsIn>(constraint);
    for (const ValidatorEntityType& type : schema.EntityTypes())
    {
        if (!descendant || type.HasDescendant(variableType)) { links.emplace_back(type.name); }
    }
    if (descendant) { links.emplace_back(variableType); }
    return links;
}

auto LinkedEnvironments(const ValidatorSchema& schema, const Template& body) -> std::vector<TypecheckEnvironment>
{
    std::vector<TypecheckEnvironment> environments;
    for (const UnlinkedRequestEnvironment& unlinked : schema.RequestEnvironments())
    {
        const auto principalLinks =
            PossibleSlotLinks(schema, body, SlotId::Principal, unlinked.principal, body.Principal().constraint);
        const auto resourceLinks =
            PossibleSlotLinks(schema, body, SlotId::Resource, unlinked.resource, body.Resource().constraint);
        for (const std::optional<EntityType>& principalSlot : principalLinks)
        {
            for (const std::optional<EntityType>& resourceSlot : resourceLinks)
            {
                TypecheckEnvironment environment = TypecheckEnvironment::FromUnlinked(unlinked);
                environment.principalSlot = principalSlot;
                environment.resourceSlot = resourceSlot;
                environments.push_back(std::move(environment));
            }
        }
    }
    return environments;
}

} // namespace cedar::detail
