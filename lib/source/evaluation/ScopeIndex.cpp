#include "evaluation/ScopeIndex.hpp"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <limits>
#include <utility>
#include <variant>

namespace cedar::detail
{

namespace
{

auto SetBit(PolicyBits& bits, const std::uint32_t position) -> void
{
    bits[position / 64] |= std::uint64_t{1} << (position % 64);
}

// After Policy::Principal and Policy::Resource every slot is replaced by its binding.
auto BoundUid(const EntityReference& reference) -> const EntityUid&
{
    assert(std::holds_alternative<EntityUid>(reference));
    return std::get<EntityUid>(reference);
}

} // namespace

auto ScopeIndex::Bucket::Finish(const std::size_t wordCount) -> void
{
    if (positions_.size() < wordCount) { return; }
    bits_.assign(wordCount, 0);
    for (const std::uint32_t position : positions_) { SetBit(bits_, position); }
    positions_ = {};
}

auto ScopeIndex::Bucket::MarkInto(PolicyBits& bits) const -> void
{
    if (bits_.empty())
    {
        for (const std::uint32_t position : positions_) { SetBit(bits, position); }
        return;
    }
    for (std::size_t word = 0; word < bits_.size(); ++word) { bits[word] |= bits_[word]; }
}

auto ScopeIndex::ConstraintIndex::AddUnconstrained(const std::uint32_t position) -> void
{
    SetBit(unconstrained_, position);
}

auto ScopeIndex::ConstraintIndex::AddEqual(const EntityUid& uid, const std::uint32_t position) -> void
{
    equal_[uid].Add(position);
}

auto ScopeIndex::ConstraintIndex::AddIs(const EntityType& type, const std::uint32_t position) -> void
{
    is_[type].Add(position);
}

auto ScopeIndex::ConstraintIndex::AddIn(const EntityUid& uid, const std::optional<EntityType>& type,
                                        const std::uint32_t position) -> void
{
    InBuckets& buckets = in_[uid];
    (type ? buckets.byType[*type] : buckets.untyped).Add(position);
}

auto ScopeIndex::ConstraintIndex::Finish() -> void
{
    const std::size_t wordCount = unconstrained_.size();
    for (auto& [uid, bucket] : equal_) { bucket.Finish(wordCount); }
    for (auto& [type, bucket] : is_) { bucket.Finish(wordCount); }
    for (auto& [uid, buckets] : in_)
    {
        buckets.untyped.Finish(wordCount);
        for (auto& [type, bucket] : buckets.byType) { bucket.Finish(wordCount); }
    }
}

auto ScopeIndex::ConstraintIndex::MarkIn(const InBuckets& buckets, const EntityType& type, PolicyBits& bits) -> void
{
    buckets.untyped.MarkInto(bits);
    if (buckets.byType.empty()) { return; }
    if (const auto found = buckets.byType.find(type); found != buckets.byType.end()) { found->second.MarkInto(bits); }
}

auto ScopeIndex::ConstraintIndex::Match(const EntityUid& uid, const EntityStoreView entities, PolicyBits& bits) const
    -> void
{
    bits.assign(unconstrained_.begin(), unconstrained_.end());
    if (const auto found = equal_.find(uid); found != equal_.end()) { found->second.MarkInto(bits); }
    if (const auto found = is_.find(uid.Type()); found != is_.end()) { found->second.MarkInto(bits); }
    if (in_.empty()) { return; }

    if (const auto found = in_.find(uid); found != in_.end()) { MarkIn(found->second, uid.Type(), bits); }
    const Entity* const entity = entities.Find(uid);
    if (entity == nullptr) { return; }
    // Walk whichever is smaller: the entity's ancestors, or the uids that `in` constraints name.
    const EntityUidSetView ancestors = entity->Ancestors();
    if (ancestors.Size() <= in_.size())
    {
        for (const EntityUid& ancestor : ancestors)
        {
            if (const auto found = in_.find(ancestor); found != in_.end()) { MarkIn(found->second, uid.Type(), bits); }
        }
    }
    else
    {
        for (const auto& [named, buckets] : in_)
        {
            if (ancestors.Contains(named)) { MarkIn(buckets, uid.Type(), bits); }
        }
    }
}

ScopeIndex::ScopeIndex(const std::span<const std::shared_ptr<const Policy>> policies)
    : wordCount_{(policies.size() + 63) / 64}, principal_{wordCount_}, action_{wordCount_}, resource_{wordCount_}
{
    assert(policies.size() <= std::numeric_limits<std::uint32_t>::max());
    for (std::uint32_t position = 0; position < policies.size(); ++position)
    {
        const Policy& policy = *policies[position];
        AddPrincipalOrResource(principal_, policy.Principal().constraint, position);
        AddPrincipalOrResource(resource_, policy.Resource().constraint, position);
        std::visit(
            [&]<typename Constraint>(const Constraint& constraint) {
                if constexpr (std::same_as<Constraint, ActionAny>) { action_.AddUnconstrained(position); }
                else if constexpr (std::same_as<Constraint, ActionEqual>) { action_.AddEqual(constraint.action, position); }
                else
                {
                    for (const EntityUid& action : constraint.actions) { action_.AddIn(action, std::nullopt, position); }
                }
            },
            policy.Action());
    }
    principal_.Finish();
    action_.Finish();
    resource_.Finish();
}

auto ScopeIndex::AddPrincipalOrResource(ConstraintIndex& index, const PrincipalOrResourceConstraint& constraint,
                                        const std::uint32_t position) -> void
{
    std::visit(
        [&]<typename Constraint>(const Constraint& part) {
            if constexpr (std::same_as<Constraint, ScopeAny>) { index.AddUnconstrained(position); }
            else if constexpr (std::same_as<Constraint, ScopeEqual>) { index.AddEqual(BoundUid(part.target), position); }
            else if constexpr (std::same_as<Constraint, ScopeIn>)
            {
                index.AddIn(BoundUid(part.target), std::nullopt, position);
            }
            else if constexpr (std::same_as<Constraint, ScopeIs>) { index.AddIs(part.type, position); }
            else { index.AddIn(BoundUid(part.target), part.type, position); }
        },
        constraint);
}

auto ScopeIndex::Candidates(const Request& request, const EntityStoreView entities, PolicyBits& candidates,
                            PolicyBits& scratch) const -> void
{
    principal_.Match(request.Principal(), entities, candidates);
    action_.Match(request.Action(), entities, scratch);
    for (std::size_t word = 0; word < wordCount_; ++word) { candidates[word] &= scratch[word]; }
    resource_.Match(request.Resource(), entities, scratch);
    for (std::size_t word = 0; word < wordCount_; ++word) { candidates[word] &= scratch[word]; }
}

} // namespace cedar::detail
