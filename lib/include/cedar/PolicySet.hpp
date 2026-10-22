#pragma once

#include <cedar/Errors.hpp>
#include <cedar/Policy.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cedar
{

namespace detail
{
struct PolicySetAccess;
struct ScopeIndexCache;
} // namespace detail

/// A set of static policies, templates and template-linked policies (Rust's public `PolicySet` in
/// `cedar-policy/src/api.rs`, over `cedar-policy-core/src/ast/policy_set.rs`).
///
/// Policies (static and linked) and templates each keep their insertion order. Every id names at
/// most one thing: a static policy, a template or a link. Policies and templates are immutable and
/// shared, so a PolicySet copies cheaply, and a PolicySet that is no longer being changed is safe
/// to read from any number of threads. Lookups take the id as a string_view and do not allocate.
class PolicySet final
{
public:
    /// The empty set.
    PolicySet();

    /// The set holding `policies`, added in order with Add (Rust's `PolicySet::from_policies`).
    /// Fails as the first failing Add does.
    [[nodiscard]] static auto FromPolicies(std::vector<Policy> policies) -> std::expected<PolicySet, PolicySetError>;

    /// Adds a static policy. Fails with ExpectedStaticPolicyId for a template-linked policy (use
    /// Link) and with PolicyIdAlreadyDefined when its id is already a policy, link or template id.
    [[nodiscard]] auto Add(Policy policy) -> std::expected<void, PolicySetError>;

    /// Adds a template. Fails with PolicyIdAlreadyDefined when its id is already a policy, link or
    /// template id. A template with no slots is allowed; linking it makes a link, not a static
    /// policy.
    [[nodiscard]] auto AddTemplate(std::shared_ptr<const Template> body) -> std::expected<void, PolicySetError>;

    /// Links the template `templateId` as the new policy `newId`. The checks run in Rust's order:
    /// ExpectedTemplateId when `templateId` names a static policy or a link, LinkNoSuchTemplate when
    /// it names nothing, LinkWrongSlots when `bindings` does not bind exactly the template's slots,
    /// LinkPolicyIdConflict when `newId` is already a policy, link or template id.
    [[nodiscard]] auto Link(std::string_view templateId, PolicyId newId, SlotBindings bindings)
        -> std::expected<void, PolicySetError>;

    /// Removes the template-linked policy `id` and returns it. Fails with LinkNonexistent when no
    /// policy has that id and with UnlinkLinkNotLink when it is a static policy, which then moves
    /// to the end of Policies() (Rust removes it and adds it back).
    [[nodiscard]] auto Unlink(std::string_view id) -> std::expected<Policy, PolicySetError>;

    /// Removes the static policy `id` and returns it. Fails with PolicyNonexistent when `id` is not
    /// a static policy (nothing, a template or a link), as Rust's `remove_static` does; a link
    /// named `id` then moves to the end of Policies(), as Rust removes it and adds it back.
    [[nodiscard]] auto RemoveStatic(std::string_view id) -> std::expected<Policy, PolicySetError>;

    /// Removes the template `id` and returns it. Fails with TemplateNonexistent when `id` is not a
    /// template added with AddTemplate (a static policy id included, as in Rust's public API) and
    /// with RemoveTemplateWithActiveLinks while a policy is linked to it, when the template moves to
    /// the end of Templates() (Rust removes it and adds it back). RemoveTemplateNotTemplate,
    /// Rust's error for an id that is both a template and a policy, cannot happen here because ids
    /// are never shared.
    [[nodiscard]] auto RemoveTemplate(std::string_view id) -> std::expected<std::shared_ptr<const Template>, PolicySetError>;

    /// Adds every template and policy of `other` (Rust's `PolicySet::merge`), leaving `other` as it
    /// is. A template, static policy or link of `other` that is structurally equal to the one this
    /// set has under the same id (source positions and original text are not compared; Rust
    /// compares the same way) is merged as one. Every other id clash, and a template of `other`
    /// with slots whose id is a policy or link here (or a link whose id is a template here), needs
    /// a renaming: with `renameDuplicates` false the merge fails with PolicyIdAlreadyDefined for
    /// the first such id and changes nothing; with it true each such item of `other` gets the
    /// first `policyN` (counting from 0) free in both sets, in Rust's order (templates and static
    /// policy bodies of `other` in the order they were added, then its static policies and links),
    /// and links follow their template's new id. Returns each renaming as (id in `other`, new id),
    /// in the order the new ids were chosen. Items new to this set are added after its own, in
    /// `other`'s order.
    [[nodiscard]] auto Merge(const PolicySet& other, bool renameDuplicates)
        -> std::expected<std::vector<std::pair<PolicyId, PolicyId>>, PolicySetError>;

    /// Every static and template-linked policy, in insertion order.
    [[nodiscard]] auto Policies() const noexcept -> std::span<const std::shared_ptr<const Policy>> { return policies_; }

    /// Every template added with AddTemplate (not the bodies of static policies), in insertion order.
    /// A template without slots read from a JSON policy set is not listed, as Rust's
    /// `PolicySet::from_est` does not list it: it is held only for its links, its id stays taken,
    /// and GetTemplate, Link and RemoveTemplate do not find it.
    [[nodiscard]] auto Templates() const noexcept -> std::span<const std::shared_ptr<const Template>>
    {
        return templates_;
    }

    /// Every template, unlisted ones included, and the body of every static policy, in the order
    /// they were added (Rust's core `PolicySet::all_templates`, what the validator checks).
    [[nodiscard]] auto AllTemplates() const -> std::vector<const Template*>;

    /// The static or template-linked policy `id`, or null. Valid until the set is changed.
    [[nodiscard]] auto GetPolicy(std::string_view id) const noexcept -> const Policy*;

    /// The template `id` added with AddTemplate, or null (also for a static policy id, as Rust's
    /// public `template()`). Valid until the set is changed.
    [[nodiscard]] auto GetTemplate(std::string_view id) const noexcept -> const Template*;

    /// The ids of the policies linked to template `templateId`, in link order. For a static policy
    /// id this is that one id, as in Rust, where a static policy is the one link of its own body.
    /// Fails with TemplateNonexistent for a link id or an unknown id. The span is valid until the
    /// set is changed.
    [[nodiscard]] auto LinkedPolicies(std::string_view templateId) const
        -> std::expected<std::span<const PolicyId>, PolicySetError>;

    /// The value of annotation `key` (Template::Annotation) on the static or template-linked
    /// policy `id`, or nothing when there is no such policy or annotation (Rust's
    /// `PolicySet::annotation`). A link has its template's annotations.
    [[nodiscard]] auto Annotation(std::string_view id, std::string_view key) const noexcept
        -> std::optional<std::string_view>;

    /// The value of annotation `key` on the template `id`, or nothing (Rust's
    /// `PolicySet::template_annotation`). As in Rust, a static policy's id also names its body
    /// here, unlike GetTemplate.
    [[nodiscard]] auto TemplateAnnotation(std::string_view id, std::string_view key) const noexcept
        -> std::optional<std::string_view>;

    [[nodiscard]] auto PolicyCount() const noexcept -> std::size_t { return policies_.size(); }
    [[nodiscard]] auto TemplateCount() const noexcept -> std::size_t { return templates_.size(); }
    [[nodiscard]] auto IsEmpty() const noexcept -> bool { return policiesById_.empty() && templatesById_.empty(); }

private:
    // Rust keeps two insertion-ordered maps under the public set: one of bodies (templates and the
    // bodies of static policies) and one of policies (static and linked). A merge walks the other
    // set's maps in that order to choose new ids, and re-inserting a key moves it to the end, so
    // each entry keeps its place in those orders as a sequence number.
    struct PolicyEntry
    {
        std::shared_ptr<const Policy> policy;
        std::uint64_t policyOrder = 0;
        /// The place of a static policy's body among the bodies.
        std::uint64_t bodyOrder = 0;
    };

    struct TemplateEntry
    {
        std::shared_ptr<const Template> body;
        std::vector<PolicyId> links;
        std::uint64_t bodyOrder = 0;
        /// Not in Templates(): a template without slots read from a JSON policy set.
        bool hidden = false;
    };

    friend struct detail::PolicySetAccess;

    /// Takes every template without slots out of Templates() (the JSON policy set reader's last
    /// step).
    auto HideSlotFreeTemplates() -> void;

    template <typename Mapped>
    using IdMap = std::unordered_map<PolicyId, Mapped, std::hash<PolicyId>, std::equal_to<>>;

    [[nodiscard]] auto IsIdTaken(std::string_view id) const noexcept -> bool;

    /// The body named `id` in Rust's map of bodies: a template, or a static policy's body.
    [[nodiscard]] auto FindBody(std::string_view id) const noexcept -> const Template*;

    auto MoveToEnd(const std::shared_ptr<const Policy>& policy) -> void;

    /// Gives the set a new, unbuilt scope index; called whenever `policies_` changes.
    auto InvalidateScopeIndex() -> void;

    std::vector<std::shared_ptr<const Policy>> policies_;
    std::vector<std::shared_ptr<const Template>> templates_;
    IdMap<PolicyEntry> policiesById_;
    IdMap<TemplateEntry> templatesById_;
    std::uint64_t nextOrder_ = 0;
    /// The scope index of `policies_` as it is now, built on first use and shared by copies until
    /// either one changes. Null only in a moved-from set.
    std::shared_ptr<detail::ScopeIndexCache> scopeIndex_;
};

} // namespace cedar
