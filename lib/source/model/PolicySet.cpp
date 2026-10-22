#include "evaluation/ScopeIndex.hpp"

#include <cedar/PolicySet.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

namespace
{

auto MakeError(const DiagnosticCode code, std::string message) -> PolicySetError
{
    return PolicySetError{Diagnostic{.code = code, .message = std::move(message)}};
}

// ---- Structural equality (Rust's derived `PartialEq`, which ignores every `Loc`) ----

auto ReferencesEqual(const EntityReference& left, const EntityReference& right) -> bool
{
    if (left.index() != right.index()) { return false; }
    if (const auto* uid = std::get_if<EntityUid>(&left)) { return *uid == std::get<EntityUid>(right); }
    return true; // Both slots; the slot is implied by the constraint and its span is not compared.
}

auto ScopesEqual(const PrincipalOrResourceConstraint& left, const PrincipalOrResourceConstraint& right) -> bool
{
    if (left.index() != right.index()) { return false; }
    return std::visit(Overloaded{
                          [](const ScopeAny&) { return true; },
                          [&](const ScopeEqual& equal) {
                              return ReferencesEqual(equal.target, std::get<ScopeEqual>(right).target);
                          },
                          [&](const ScopeIn& in) { return ReferencesEqual(in.target, std::get<ScopeIn>(right).target); },
                          [&](const ScopeIs& is) { return is.type == std::get<ScopeIs>(right).type; },
                          [&](const ScopeIsIn& isIn) {
                              const auto& other = std::get<ScopeIsIn>(right);
                              return isIn.type == other.type && ReferencesEqual(isIn.target, other.target);
                          },
                      },
                      left);
}

// `action in A` and `action in [A]` are the same constraint in Rust's AST.
auto ActionsEqual(const ActionConstraint& left, const ActionConstraint& right) -> bool
{
    if (left.index() != right.index()) { return false; }
    if (const auto* equal = std::get_if<ActionEqual>(&left)) { return equal->action == std::get<ActionEqual>(right).action; }
    if (const auto* in = std::get_if<ActionIn>(&left)) { return in->actions == std::get<ActionIn>(right).actions; }
    return true;
}

// Rust's AST stores `@key` as the empty string, so `@key` equals `@key("")`.
auto AnnotationsEqual(const Annotations& left, const Annotations& right) -> bool
{
    return std::ranges::equal(left.Entries(), right.Entries(), [](const Annotation& a, const Annotation& b) {
        return a.key == b.key && a.value.value_or(std::string{}) == b.value.value_or(std::string{});
    });
}

// The node standing for `index` in Rust's tree: a folded node is its written form.
auto Unfold(const ExpressionArena& arena, ExpressionIndex index) -> ExpressionIndex
{
    while (const auto* folded = std::get_if<node::Folded>(&arena.Node(index))) { index = folded->written; }
    return index;
}

// Whether the expression at `leftRoot` in `left` and the one at `rightRoot` in `right` are the same
// tree (Rust's `Expr` equality, which ignores source locations).
auto ExpressionsEqual(const ExpressionArena& left, const ExpressionIndex leftRoot, const ExpressionArena& right,
                      const ExpressionIndex rightRoot) -> bool
{
    std::vector<std::pair<ExpressionIndex, ExpressionIndex>> pending{{leftRoot, rightRoot}};
    const auto pushRanges = [&](const ArenaRange<ExpressionIndex> a, const ArenaRange<ExpressionIndex> b) {
        if (a.count != b.count) { return false; }
        const auto leftElements = left.Elements(a);
        const auto rightElements = right.Elements(b);
        for (std::size_t index = 0; index < leftElements.size(); ++index)
        {
            pending.emplace_back(leftElements[index], rightElements[index]);
        }
        return true;
    };
    const auto textsEqual = [&](const ArenaRange<std::string_view> a, const ArenaRange<std::string_view> b) {
        return std::ranges::equal(left.Elements(a), right.Elements(b));
    };
    while (!pending.empty())
    {
        const auto [leftIndex, rightIndex] = pending.back();
        pending.pop_back();
        const ExpressionNode& leftNode = left.Node(Unfold(left, leftIndex));
        const ExpressionNode& rightNode = right.Node(Unfold(right, rightIndex));
        if (leftNode.index() != rightNode.index()) { return false; }
        const bool same = std::visit(
            Overloaded{
                [&](const node::Literal& literal) { return literal.value == std::get<node::Literal>(rightNode).value; },
                [](const node::Folded&) { return false; }, // Unfolded above.
                [&](const node::Variable& variable) {
                    return variable.variable == std::get<node::Variable>(rightNode).variable;
                },
                [&](const node::Slot& slot) { return slot.slot == std::get<node::Slot>(rightNode).slot; },
                [&](const node::If& branch) {
                    const auto& other = std::get<node::If>(rightNode);
                    pending.emplace_back(branch.condition, other.condition);
                    pending.emplace_back(branch.thenBranch, other.thenBranch);
                    pending.emplace_back(branch.elseBranch, other.elseBranch);
                    return true;
                },
                [&](const node::And& conjunction) {
                    const auto& other = std::get<node::And>(rightNode);
                    pending.emplace_back(conjunction.left, other.left);
                    pending.emplace_back(conjunction.right, other.right);
                    return true;
                },
                [&](const node::Or& disjunction) {
                    const auto& other = std::get<node::Or>(rightNode);
                    pending.emplace_back(disjunction.left, other.left);
                    pending.emplace_back(disjunction.right, other.right);
                    return true;
                },
                [&](const node::Unary& unary) {
                    const auto& other = std::get<node::Unary>(rightNode);
                    pending.emplace_back(unary.operand, other.operand);
                    return unary.operation == other.operation;
                },
                [&](const node::Binary& binary) {
                    const auto& other = std::get<node::Binary>(rightNode);
                    pending.emplace_back(binary.left, other.left);
                    pending.emplace_back(binary.right, other.right);
                    return binary.operation == other.operation;
                },
                [&](const node::ExtensionCall& call) {
                    const auto& other = std::get<node::ExtensionCall>(rightNode);
                    return call.function == other.function && pushRanges(call.arguments, other.arguments);
                },
                [&](const node::GetAttribute& get) {
                    const auto& other = std::get<node::GetAttribute>(rightNode);
                    pending.emplace_back(get.object, other.object);
                    return get.attribute == other.attribute;
                },
                [&](const node::HasAttribute& has) {
                    const auto& other = std::get<node::HasAttribute>(rightNode);
                    pending.emplace_back(has.object, other.object);
                    return textsEqual(has.path, other.path);
                },
                [&](const node::Like& like) {
                    const auto& other = std::get<node::Like>(rightNode);
                    pending.emplace_back(like.operand, other.operand);
                    return textsEqual(like.pattern, other.pattern);
                },
                [&](const node::Is& is) {
                    const auto& other = std::get<node::Is>(rightNode);
                    pending.emplace_back(is.operand, other.operand);
                    return is.type == other.type;
                },
                [&](const node::SetLiteral& set) {
                    return pushRanges(set.elements, std::get<node::SetLiteral>(rightNode).elements);
                },
                [&](const node::RecordLiteral& record) {
                    const auto leftFields = left.Elements(record.fields);
                    const auto rightFields = right.Elements(std::get<node::RecordLiteral>(rightNode).fields);
                    if (leftFields.size() != rightFields.size()) { return false; }
                    for (std::size_t index = 0; index < leftFields.size(); ++index)
                    {
                        if (leftFields[index].key != rightFields[index].key) { return false; }
                        pending.emplace_back(leftFields[index].value, rightFields[index].value);
                    }
                    return true;
                },
            },
            leftNode);
        if (!same) { return false; }
    }
    return true;
}

// Rust's `Template` equality: the body (id, annotations, effect, scope, non-scope condition) with
// every location ignored; the slot list follows from the body.
auto TemplatesEqual(const Template& left, const Template& right) -> bool
{
    if (&left == &right) { return true; }
    if (left.Id() != right.Id() || left.GetEffect() != right.GetEffect() ||
        !AnnotationsEqual(left.GetAnnotations(), right.GetAnnotations()) ||
        !ScopesEqual(left.Principal().constraint, right.Principal().constraint) ||
        !ActionsEqual(left.Action(), right.Action()) ||
        !ScopesEqual(left.Resource().constraint, right.Resource().constraint))
    {
        return false;
    }
    const auto leftCondition = left.NonScopeCondition();
    const auto rightCondition = right.NonScopeCondition();
    if (leftCondition.has_value() != rightCondition.has_value()) { return false; }
    return !leftCondition || ExpressionsEqual(left.Arena(), *leftCondition, right.Arena(), *rightCondition);
}

// Rust's `Policy` equality: the template, the link id and the bound values.
auto PoliciesEqual(const Policy& left, const Policy& right) -> bool
{
    if (left.IsStatic() != right.IsStatic() || left.Id() != right.Id()) { return false; }
    for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
    {
        const EntityUid* leftBinding = left.Binding(slot);
        const EntityUid* rightBinding = right.Binding(slot);
        if ((leftBinding == nullptr) != (rightBinding == nullptr)) { return false; }
        if (leftBinding != nullptr && *leftBinding != *rightBinding) { return false; }
    }
    return TemplatesEqual(left.GetTemplate(), right.GetTemplate());
}

} // namespace

auto PolicySet::IsIdTaken(const std::string_view id) const noexcept -> bool
{
    return policiesById_.contains(id) || templatesById_.contains(id);
}

auto PolicySet::FindBody(const std::string_view id) const noexcept -> const Template*
{
    if (const auto found = templatesById_.find(id); found != templatesById_.end()) { return found->second.body.get(); }
    if (const auto found = policiesById_.find(id); found != policiesById_.end() && found->second.policy->IsStatic())
    {
        return &found->second.policy->GetTemplate();
    }
    return nullptr;
}

PolicySet::PolicySet() : scopeIndex_{std::make_shared<detail::ScopeIndexCache>()} {}

auto PolicySet::InvalidateScopeIndex() -> void
{
    // An index nobody has built and no copy shares describes no list yet, so it can stay. A set is
    // never changed while another thread reads it, so `index` is read here without the once flag.
    if (scopeIndex_ != nullptr && scopeIndex_.use_count() == 1 && !scopeIndex_->index) { return; }
    scopeIndex_ = std::make_shared<detail::ScopeIndexCache>();
}

auto PolicySet::MoveToEnd(const std::shared_ptr<const Policy>& policy) -> void
{
    const auto position = std::ranges::find(policies_, policy);
    assert(position != policies_.end());
    std::rotate(position, position + 1, policies_.end());
    InvalidateScopeIndex();
}

auto PolicySet::FromPolicies(std::vector<Policy> policies) -> std::expected<PolicySet, PolicySetError>
{
    PolicySet set;
    for (Policy& policy : policies)
    {
        if (auto added = set.Add(std::move(policy)); !added) { return std::unexpected{std::move(added).error()}; }
    }
    return set;
}

auto PolicySet::Add(Policy policy) -> std::expected<void, PolicySetError>
{
    if (!policy.IsStatic())
    {
        return std::unexpected{MakeError(DiagnosticCode::ExpectedStaticPolicyId,
                                         std::format("`{}` is a template-linked policy; add it by linking its template",
                                                     policy.Id().Text()))};
    }
    if (IsIdTaken(policy.Id().Text()))
    {
        return std::unexpected{MakeError(DiagnosticCode::PolicyIdAlreadyDefined,
                                         std::format("duplicate template or policy id `{}`", policy.Id().Text()))};
    }
    auto shared = std::make_shared<const Policy>(std::move(policy));
    const std::uint64_t order = nextOrder_++;
    policiesById_.emplace(shared->Id(), PolicyEntry{shared, order, order});
    policies_.push_back(std::move(shared));
    InvalidateScopeIndex();
    return {};
}

auto PolicySet::AddTemplate(std::shared_ptr<const Template> body) -> std::expected<void, PolicySetError>
{
    if (IsIdTaken(body->Id().Text()))
    {
        return std::unexpected{MakeError(DiagnosticCode::PolicyIdAlreadyDefined,
                                         std::format("duplicate template or policy id `{}`", body->Id().Text()))};
    }
    templatesById_.emplace(body->Id(), TemplateEntry{body, {}, nextOrder_++});
    templates_.push_back(std::move(body));
    return {};
}

auto PolicySet::Link(const std::string_view templateId, PolicyId newId, SlotBindings bindings)
    -> std::expected<void, PolicySetError>
{
    // Rust's public `link` looks in the public template map, which an unlisted template is not in.
    const auto entry = templatesById_.find(templateId);
    if (entry == templatesById_.end() || entry->second.hidden)
    {
        if (policiesById_.contains(templateId))
        {
            return std::unexpected{MakeError(DiagnosticCode::ExpectedTemplateId,
                                             std::format("`{}` is a policy, not a template", templateId))};
        }
        return std::unexpected{MakeError(DiagnosticCode::LinkNoSuchTemplate,
                                         std::format("failed to find a template with id `{}`", templateId))};
    }

    auto linked = Policy::Link(entry->second.body, std::move(newId), std::move(bindings));
    if (!linked) { return std::unexpected{std::move(linked).error()}; }

    if (IsIdTaken(linked->Id().Text()))
    {
        return std::unexpected{
            MakeError(DiagnosticCode::LinkPolicyIdConflict,
                      std::format("template-linked policy id `{}` conflicts with an existing policy id",
                                  linked->Id().Text()))};
    }
    auto shared = std::make_shared<const Policy>(std::move(*linked));
    entry->second.links.push_back(shared->Id());
    policiesById_.emplace(shared->Id(), PolicyEntry{shared, nextOrder_++, 0});
    policies_.push_back(std::move(shared));
    InvalidateScopeIndex();
    return {};
}

auto PolicySet::Unlink(const std::string_view id) -> std::expected<Policy, PolicySetError>
{
    const auto found = policiesById_.find(id);
    if (found == policiesById_.end())
    {
        return std::unexpected{MakeError(DiagnosticCode::LinkNonexistent,
                                         std::format("unable to unlink `{}`: no such policy", id))};
    }
    if (found->second.policy->IsStatic())
    {
        MoveToEnd(found->second.policy);
        return std::unexpected{MakeError(DiagnosticCode::UnlinkLinkNotLink,
                                         std::format("unable to unlink `{}`: it is a static policy", id))};
    }

    std::shared_ptr<const Policy> policy = std::move(found->second.policy);
    policiesById_.erase(found);
    std::erase(policies_, policy);
    InvalidateScopeIndex();
    const auto entry = templatesById_.find(policy->GetTemplate().Id());
    assert(entry != templatesById_.end());
    std::erase(entry->second.links, policy->Id());
    return *policy;
}

auto PolicySet::RemoveStatic(const std::string_view id) -> std::expected<Policy, PolicySetError>
{
    const auto found = policiesById_.find(id);
    if (found == policiesById_.end() || !found->second.policy->IsStatic())
    {
        if (found != policiesById_.end())
        {
            // A link: Rust's core and public maps both remove it and add it back.
            MoveToEnd(found->second.policy);
            found->second.policyOrder = nextOrder_++;
        }
        return std::unexpected{
            MakeError(DiagnosticCode::PolicyNonexistent, std::format("no static policy with id `{}`", id))};
    }
    std::shared_ptr<const Policy> policy = std::move(found->second.policy);
    policiesById_.erase(found);
    std::erase(policies_, policy);
    InvalidateScopeIndex();
    return *policy;
}

auto PolicySet::RemoveTemplate(const std::string_view id) -> std::expected<std::shared_ptr<const Template>, PolicySetError>
{
    const auto found = templatesById_.find(id);
    if (found == templatesById_.end() || found->second.hidden)
    {
        return std::unexpected{
            MakeError(DiagnosticCode::TemplateNonexistent, std::format("no template with id `{}`", id))};
    }
    if (!found->second.links.empty())
    {
        // Rust's public map removes the template and adds it back; its core map is unchanged.
        const auto position = std::ranges::find(templates_, found->second.body);
        assert(position != templates_.end());
        std::rotate(position, position + 1, templates_.end());
        return std::unexpected{
            MakeError(DiagnosticCode::RemoveTemplateWithActiveLinks,
                      std::format("unable to remove template `{}`: policies are still linked to it", id))};
    }
    std::shared_ptr<const Template> body = std::move(found->second.body);
    templatesById_.erase(found);
    std::erase(templates_, body);
    return body;
}

auto PolicySet::Merge(const PolicySet& other, const bool renameDuplicates)
    -> std::expected<std::vector<std::pair<PolicyId, PolicyId>>, PolicySetError>
{
    // `other`'s two Rust maps, in their order (policy_set.rs `merge_policyset`).
    struct OtherBody
    {
        std::string_view id;
        const Template* body;
        bool isStatic;
        std::uint64_t order;
    };
    std::vector<OtherBody> otherBodies;
    for (const auto& [id, entry] : other.templatesById_)
    {
        otherBodies.push_back({id.Text(), entry.body.get(), false, entry.bodyOrder});
    }
    std::vector<const PolicyEntry*> otherPolicies;
    for (const auto& [id, entry] : other.policiesById_)
    {
        if (entry.policy->IsStatic()) { otherBodies.push_back({id.Text(), &entry.policy->GetTemplate(), true, entry.bodyOrder}); }
        otherPolicies.push_back(&entry);
    }
    std::ranges::sort(otherBodies, {}, &OtherBody::order);
    std::ranges::sort(otherPolicies, {}, &PolicyEntry::policyOrder);

    std::vector<std::pair<PolicyId, PolicyId>> renaming;
    IdMap<PolicyId> renamedTo;
    std::uint32_t nextFresh = 0;
    const auto rename = [&](const std::string_view id) {
        if (renamedTo.contains(id)) { return; }
        std::string fresh;
        do {
            fresh = std::format("policy{}", nextFresh++);
        } while (IsIdTaken(fresh) || other.IsIdTaken(fresh));
        renamedTo.emplace(PolicyId{id}, PolicyId{fresh});
        renaming.emplace_back(PolicyId{id}, PolicyId{std::move(fresh)});
    };

    // A body of `other` that differs from this set's body of the same id. A static policy's body
    // and a template never merge here, though Rust's core can hold both under one id: ids are
    // never shared in this set.
    for (const OtherBody& body : otherBodies)
    {
        const Template* mine = FindBody(body.id);
        if (mine == nullptr) { continue; }
        const bool mineIsStatic = !templatesById_.contains(body.id);
        if (mineIsStatic != body.isStatic || !TemplatesEqual(*mine, *body.body)) { rename(body.id); }
    }
    // A static policy or link of `other` that differs from this set's policy of the same id.
    for (const PolicyEntry* entry : otherPolicies)
    {
        const auto found = policiesById_.find(entry->policy->Id().Text());
        if (found != policiesById_.end() && !PoliciesEqual(*found->second.policy, *entry->policy))
        {
            rename(entry->policy->Id().Text());
        }
    }
    // A template with slots named like a policy or link here, and a link named like a body here.
    for (const OtherBody& body : otherBodies)
    {
        if (!body.body->Slots().empty() && policiesById_.contains(body.id)) { rename(body.id); }
    }
    for (const PolicyEntry* entry : otherPolicies)
    {
        if (!entry->policy->IsStatic() && FindBody(entry->policy->Id().Text()) != nullptr)
        {
            rename(entry->policy->Id().Text());
        }
    }

    if (!renameDuplicates && !renaming.empty())
    {
        return std::unexpected{MakeError(DiagnosticCode::PolicyIdAlreadyDefined,
                                         std::format("duplicate template or policy id `{}`", renaming.front().first.Text()))};
    }

    const auto newId = [&](const std::string_view id) -> std::string_view {
        const auto found = renamedTo.find(id);
        return found == renamedTo.end() ? id : found->second.Text();
    };

    // Rust re-inserts every body, then every policy, of `other`: each moves to the end of its
    // order, whether it is new here or merged with an equal one.
    IdMap<std::uint64_t> bodyOrders;
    for (const OtherBody& body : otherBodies) { bodyOrders.emplace(PolicyId{newId(body.id)}, nextOrder_++); }
    IdMap<std::uint64_t> policyOrders;
    for (const PolicyEntry* entry : otherPolicies)
    {
        policyOrders.emplace(PolicyId{newId(entry->policy->Id().Text())}, nextOrder_++);
    }

    // Templates() gains `other`'s listed templates new to it, in `other`'s order (Rust's public
    // map); the unlisted ones join only the core, as Rust's do.
    const auto addTemplate = [&](const std::shared_ptr<const Template>& body, const bool hidden) {
        const std::string_view id = newId(body->Id().Text());
        const std::uint64_t order = bodyOrders.find(id)->second;
        if (const auto found = templatesById_.find(id); found != templatesById_.end())
        {
            found->second.bodyOrder = order;
            if (found->second.hidden && !hidden)
            {
                found->second.hidden = false;
                templates_.push_back(found->second.body);
            }
            return;
        }
        std::shared_ptr<const Template> added = id == body->Id().Text() ? body : body->WithId(PolicyId{id});
        templatesById_.emplace(PolicyId{id}, TemplateEntry{added, {}, order, hidden});
        if (!hidden) { templates_.push_back(std::move(added)); }
    };
    for (const std::shared_ptr<const Template>& body : other.templates_) { addTemplate(body, false); }
    for (const OtherBody& body : otherBodies)
    {
        if (!body.isStatic && other.templatesById_.find(body.id)->second.hidden)
        {
            addTemplate(other.templatesById_.find(body.id)->second.body, true);
        }
    }

    for (const std::shared_ptr<const Policy>& policy : other.policies_)
    {
        const std::string_view id = newId(policy->Id().Text());
        const std::uint64_t order = policyOrders.find(id)->second;
        if (const auto found = policiesById_.find(id); found != policiesById_.end())
        {
            found->second.policyOrder = order;
            if (found->second.policy->IsStatic()) { found->second.bodyOrder = bodyOrders.find(id)->second; }
            continue;
        }
        std::shared_ptr<const Policy> added;
        std::uint64_t bodyOrder = 0;
        if (policy->IsStatic())
        {
            bodyOrder = bodyOrders.find(id)->second;
            added = id == policy->Id().Text()
                        ? policy
                        : std::make_shared<const Policy>(*Policy::Static(policy->GetTemplate().WithId(PolicyId{id})));
        }
        else
        {
            const std::string_view templateId = policy->GetTemplate().Id().Text();
            const std::string_view newTemplateId = newId(templateId);
            if (id == policy->Id().Text() && newTemplateId == templateId) { added = policy; }
            else
            {
                // A renamed template is the one just added under its new id; otherwise the link
                // keeps the template it was made from, as Rust's does.
                std::shared_ptr<const Template> body = newTemplateId == templateId
                                                           ? policy->SharedTemplate()
                                                           : templatesById_.find(newTemplateId)->second.body;
                added = std::make_shared<const Policy>(*Policy::Link(std::move(body), PolicyId{id}, policy->Bindings()));
            }
        }
        policiesById_.emplace(PolicyId{id}, PolicyEntry{added, order, bodyOrder});
        policies_.push_back(std::move(added));
        InvalidateScopeIndex();
    }

    // Each template's links: this set's, then `other`'s in its order, a link already listed moving
    // to the end (Rust's `LinkedHashSet::insert`).
    for (const auto& [templateId, otherEntry] : other.templatesById_)
    {
        const auto mine = templatesById_.find(newId(templateId.Text()));
        assert(mine != templatesById_.end());
        for (const PolicyId& link : otherEntry.links)
        {
            const std::string_view linkId = newId(link.Text());
            std::erase(mine->second.links, linkId);
            mine->second.links.emplace_back(linkId);
        }
    }
    return renaming;
}

auto PolicySet::GetPolicy(const std::string_view id) const noexcept -> const Policy*
{
    const auto found = policiesById_.find(id);
    return found == policiesById_.end() ? nullptr : found->second.policy.get();
}

auto PolicySet::GetTemplate(const std::string_view id) const noexcept -> const Template*
{
    const auto found = templatesById_.find(id);
    return found == templatesById_.end() || found->second.hidden ? nullptr : found->second.body.get();
}

auto PolicySet::AllTemplates() const -> std::vector<const Template*>
{
    std::vector<std::pair<std::uint64_t, const Template*>> ordered;
    ordered.reserve(templatesById_.size() + policiesById_.size());
    for (const auto& [id, entry] : templatesById_) { ordered.emplace_back(entry.bodyOrder, entry.body.get()); }
    for (const auto& [id, entry] : policiesById_)
    {
        if (entry.policy->IsStatic()) { ordered.emplace_back(entry.bodyOrder, &entry.policy->GetTemplate()); }
    }
    std::ranges::sort(ordered, {}, &std::pair<std::uint64_t, const Template*>::first);
    std::vector<const Template*> bodies;
    bodies.reserve(ordered.size());
    for (const auto& [order, body] : ordered) { bodies.push_back(body); }
    return bodies;
}

auto PolicySet::HideSlotFreeTemplates() -> void
{
    for (auto& [id, entry] : templatesById_)
    {
        if (!entry.body->IsStatic() || entry.hidden) { continue; }
        entry.hidden = true;
        std::erase(templates_, entry.body);
    }
}

auto PolicySet::LinkedPolicies(const std::string_view templateId) const
    -> std::expected<std::span<const PolicyId>, PolicySetError>
{
    if (const auto found = templatesById_.find(templateId); found != templatesById_.end())
    {
        return std::span<const PolicyId>{found->second.links};
    }
    // A static policy's id names its body too, linked once, to itself (policy_set.rs:387).
    if (const auto found = policiesById_.find(templateId); found != policiesById_.end() && found->second.policy->IsStatic())
    {
        return std::span<const PolicyId>{&found->second.policy->Id(), 1};
    }
    return std::unexpected{
        MakeError(DiagnosticCode::TemplateNonexistent, std::format("no template with id `{}`", templateId))};
}

auto PolicySet::Annotation(const std::string_view id, const std::string_view key) const noexcept
    -> std::optional<std::string_view>
{
    const Policy* policy = GetPolicy(id);
    return policy == nullptr ? std::nullopt : policy->Annotation(key);
}

auto PolicySet::TemplateAnnotation(const std::string_view id, const std::string_view key) const noexcept
    -> std::optional<std::string_view>
{
    const Template* body = FindBody(id);
    return body == nullptr ? std::nullopt : body->Annotation(key);
}

} // namespace cedar
