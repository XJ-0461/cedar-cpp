#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Syntax.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The policy-set API beyond add and link: Merge (with the renaming order of Rust's
// `merge_policyset`, policy_set.rs:266, worked out by hand from its loops since no CLI command
// reaches it), the annotation getters, the constraint accessors and EntityLiterals.

namespace
{

using namespace cedar;

auto Uid(const std::string_view type, const std::string_view id) -> EntityUid
{
    return EntityUid::Create(type, id).value();
}

auto Static(const std::string_view id, const std::string_view text) -> Policy
{
    return ParsePolicy(text, PolicyId{id}).value();
}

auto MakeTemplate(const std::string_view id, const std::string_view text) -> std::shared_ptr<const Template>
{
    return ParseTemplate(text, PolicyId{id}).value();
}

auto Principal(const std::string_view id) -> SlotBindings
{
    return SlotBindings{}.Bind(SlotId::Principal, Uid("User", id));
}

auto Ids(const PolicySet& set) -> std::vector<std::string>
{
    std::vector<std::string> ids;
    for (const auto& policy : set.Policies()) { ids.emplace_back(policy->Id().Text()); }
    return ids;
}

auto TemplateIds(const PolicySet& set) -> std::vector<std::string>
{
    std::vector<std::string> ids;
    for (const auto& body : set.Templates()) { ids.emplace_back(body->Id().Text()); }
    return ids;
}

auto Renamings(const std::vector<std::pair<PolicyId, PolicyId>>& renaming) -> std::vector<std::string>
{
    std::vector<std::string> lines;
    for (const auto& [from, to] : renaming) { lines.push_back(std::string{from.Text()} + "->" + std::string{to.Text()}); }
    return lines;
}

auto Linked(const PolicySet& set, const std::string_view templateId) -> std::vector<std::string>
{
    std::vector<std::string> ids;
    for (const PolicyId& id : set.LinkedPolicies(templateId).value()) { ids.emplace_back(id.Text()); }
    return ids;
}

constexpr std::string_view PermitAll = "permit(principal, action, resource);";
constexpr std::string_view PrincipalTemplate = "permit(principal == ?principal, action, resource);";

TEST(PolicySetApiTests, MergeRenamesInRustOrder)
{
    PolicySet mine;
    ASSERT_TRUE(mine.Add(Static("a", PermitAll)).has_value());
    ASSERT_TRUE(mine.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(mine.Link("t", PolicyId{"l"}, Principal("x")).has_value());
    ASSERT_TRUE(mine.Add(Static("policy0", "forbid(principal, action, resource);")).has_value());

    PolicySet other;
    ASSERT_TRUE(other.AddTemplate(MakeTemplate("t", "forbid(principal == ?principal, action, resource);")).has_value());
    ASSERT_TRUE(other.Add(Static("a", "permit(principal, action, resource) when { true };")).has_value());
    ASSERT_TRUE(other.Add(Static("b", PermitAll)).has_value());
    ASSERT_TRUE(other.Link("t", PolicyId{"l"}, Principal("x")).has_value());
    ASSERT_TRUE(other.Add(Static("policy1", PermitAll)).has_value());

    // Bodies first (t, then a's body), then policies (l): `policy0` is taken here and `policy1`
    // in `other`, so the fresh ids start at `policy2`.
    PolicySet refused = mine;
    const auto failed = refused.Merge(other, false);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().Primary().code, DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Ids(refused), Ids(mine));

    const auto renaming = mine.Merge(other, true);
    ASSERT_TRUE(renaming.has_value());
    EXPECT_EQ(Renamings(*renaming), (std::vector<std::string>{"t->policy2", "a->policy3", "l->policy4"}));
    EXPECT_EQ(Ids(mine), (std::vector<std::string>{"a", "l", "policy0", "policy3", "b", "policy4", "policy1"}));
    EXPECT_EQ(TemplateIds(mine), (std::vector<std::string>{"t", "policy2"}));
    EXPECT_EQ(Linked(mine, "t"), std::vector<std::string>{"l"});
    EXPECT_EQ(Linked(mine, "policy2"), std::vector<std::string>{"policy4"});
    const Policy* moved = mine.GetPolicy("policy4");
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(*moved->TemplateId(), PolicyId{"policy2"});
    EXPECT_EQ(moved->GetEffect(), Effect::Forbid);
    EXPECT_EQ(*moved->Binding(SlotId::Principal), Uid("User", "x"));
    EXPECT_EQ(mine.GetPolicy("policy3")->GetTemplate().NonScopeCondition().has_value(), true);
    EXPECT_EQ(other.PolicyCount(), 4U);
}

TEST(PolicySetApiTests, MergeJoinsEqualItemsIgnoringPositions)
{
    PolicySet mine;
    ASSERT_TRUE(mine.Add(Static("a", "@note permit(principal, action, resource) when { 1 < 2 && ip(\"10.0.0.1\").isLoopback() };"))
                    .has_value());
    ASSERT_TRUE(mine.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(mine.Link("t", PolicyId{"l"}, Principal("x")).has_value());

    // The same items written differently: `@note("")` is `@note` in Rust's AST.
    PolicySet other;
    ASSERT_TRUE(other.Add(Static("a", "@note(\"\")\npermit (principal,action,resource) // same\n"
                                      "when {1<2 && ip( \"10.0.0.1\" ).isLoopback()};"))
                    .has_value());
    ASSERT_TRUE(other.AddTemplate(MakeTemplate("t", "permit(\n  principal == ?principal,\n  action,\n  resource\n);")).has_value());
    ASSERT_TRUE(other.Link("t", PolicyId{"l"}, Principal("x")).has_value());
    ASSERT_TRUE(other.Link("t", PolicyId{"m"}, Principal("y")).has_value());

    const auto renaming = mine.Merge(other, false);
    ASSERT_TRUE(renaming.has_value());
    EXPECT_TRUE(renaming->empty());
    EXPECT_EQ(Ids(mine), (std::vector<std::string>{"a", "l", "m"}));
    EXPECT_EQ(TemplateIds(mine), std::vector<std::string>{"t"});
    EXPECT_EQ(Linked(mine, "t"), (std::vector<std::string>{"l", "m"}));

    // A different binding is a different link.
    PolicySet rebound;
    ASSERT_TRUE(rebound.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(rebound.Link("t", PolicyId{"l"}, Principal("z")).has_value());
    const auto clash = mine.Merge(rebound, true);
    ASSERT_TRUE(clash.has_value());
    EXPECT_EQ(Renamings(*clash), std::vector<std::string>{"l->policy0"});
    EXPECT_EQ(Linked(mine, "t"), (std::vector<std::string>{"l", "m", "policy0"}));
}

TEST(PolicySetApiTests, MergeMovesMergedLinksAndBodiesToTheEnd)
{
    // An equal link already listed moves to the end of its template's links (LinkedHashSet).
    PolicySet mine;
    ASSERT_TRUE(mine.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    for (const std::string_view id : {"l1", "l2", "l3"})
    {
        ASSERT_TRUE(mine.Link("t", PolicyId{id}, Principal(id)).has_value());
    }
    PolicySet other;
    ASSERT_TRUE(other.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(other.Link("t", PolicyId{"l1"}, Principal("l1")).has_value());
    ASSERT_TRUE(mine.Merge(other, false).has_value());
    EXPECT_EQ(Linked(mine, "t"), (std::vector<std::string>{"l2", "l3", "l1"}));
    EXPECT_EQ(Ids(mine), (std::vector<std::string>{"l1", "l2", "l3"}));

    // A merged equal static policy moves to the end of the order a later merge renames in.
    PolicySet first;
    ASSERT_TRUE(first.Add(Static("a", PermitAll)).has_value());
    ASSERT_TRUE(first.Add(Static("b", PermitAll)).has_value());
    PolicySet again;
    ASSERT_TRUE(again.Add(Static("a", PermitAll)).has_value());
    ASSERT_TRUE(first.Merge(again, false).has_value());
    EXPECT_EQ(Ids(first), (std::vector<std::string>{"a", "b"}));

    PolicySet target;
    ASSERT_TRUE(target.Add(Static("a", "forbid(principal, action, resource);")).has_value());
    ASSERT_TRUE(target.Add(Static("b", "forbid(principal, action, resource);")).has_value());
    const auto renaming = target.Merge(first, true);
    ASSERT_TRUE(renaming.has_value());
    EXPECT_EQ(Renamings(*renaming), (std::vector<std::string>{"b->policy0", "a->policy1"}));
}

TEST(PolicySetApiTests, MergeRenamesKindClashes)
{
    // A link of `other` named like a template here (Rust's fourth loop).
    PolicySet mine;
    ASSERT_TRUE(mine.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    PolicySet other;
    ASSERT_TRUE(other.AddTemplate(MakeTemplate("u", PrincipalTemplate)).has_value());
    ASSERT_TRUE(other.Link("u", PolicyId{"t"}, Principal("x")).has_value());
    const auto renaming = mine.Merge(other, true);
    ASSERT_TRUE(renaming.has_value());
    EXPECT_EQ(Renamings(*renaming), std::vector<std::string>{"t->policy0"});
    EXPECT_EQ(*mine.GetPolicy("policy0")->TemplateId(), PolicyId{"u"});
    EXPECT_EQ(Linked(mine, "u"), std::vector<std::string>{"policy0"});

    // A template with slots of `other` named like a link here (the third loop).
    PolicySet links;
    ASSERT_TRUE(links.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(links.Link("t", PolicyId{"l"}, Principal("x")).has_value());
    PolicySet templates;
    ASSERT_TRUE(templates.AddTemplate(MakeTemplate("l", PrincipalTemplate)).has_value());
    const auto renamed = links.Merge(templates, true);
    ASSERT_TRUE(renamed.has_value());
    EXPECT_EQ(Renamings(*renamed), std::vector<std::string>{"l->policy0"});
    EXPECT_EQ(TemplateIds(links), (std::vector<std::string>{"t", "policy0"}));

    // A static policy and a slot-free template with equal bodies are not merged here, though
    // Rust's core can hold both under one id.
    PolicySet statics;
    ASSERT_TRUE(statics.Add(Static("s", PermitAll)).has_value());
    TemplateDefinition definition{.id = PolicyId{"s"}};
    PolicySet slotFree;
    ASSERT_TRUE(slotFree.AddTemplate(Template::Create(std::move(definition))).has_value());
    const auto kinds = statics.Merge(slotFree, true);
    ASSERT_TRUE(kinds.has_value());
    EXPECT_EQ(Renamings(*kinds), std::vector<std::string>{"s->policy0"});
}

TEST(PolicySetApiTests, AnnotationGetters)
{
    PolicySet set;
    ASSERT_TRUE(set.Add(Static("s", "@id(\"x\") @flag @if(\"keyword\") permit(principal, action, resource);")).has_value());
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t", "@owner(\"ops\") permit(principal == ?principal, action, resource);"))
                    .has_value());
    ASSERT_TRUE(set.Link("t", PolicyId{"l"}, Principal("x")).has_value());

    const Template& body = set.GetPolicy("s")->GetTemplate();
    EXPECT_EQ(body.Annotation("id"), "x");
    EXPECT_EQ(body.Annotation("flag"), "");
    EXPECT_EQ(body.Annotation("if"), "keyword");
    // Rust parses the key as an identifier: whitespace and comments around it are allowed.
    EXPECT_EQ(body.Annotation(" id // the key\n"), "x");
    EXPECT_FALSE(body.Annotation("i d").has_value());
    EXPECT_FALSE(body.Annotation("").has_value());
    EXPECT_FALSE(body.Annotation("?principal").has_value());
    EXPECT_FALSE(body.Annotation("missing").has_value());

    EXPECT_EQ(set.Annotation("l", "owner"), "ops");
    EXPECT_EQ(set.GetPolicy("l")->Annotation("owner"), "ops");
    EXPECT_FALSE(set.Annotation("t", "owner").has_value());
    EXPECT_EQ(set.TemplateAnnotation("t", "owner"), "ops");
    // A static policy's id names its body for TemplateAnnotation, not for GetTemplate.
    EXPECT_EQ(set.TemplateAnnotation("s", "id"), "x");
    EXPECT_EQ(set.GetTemplate("s"), nullptr);
    EXPECT_FALSE(set.TemplateAnnotation("l", "owner").has_value());
}

TEST(PolicySetApiTests, ConstraintsAndEntityLiterals)
{
    const Policy policy = Static("p", "permit(principal == User::\"p\", action in [Action::\"a\", Action::\"b\"], "
                                      "resource in Folder::\"r\") when { User::\"c\" in [User::\"d\", User::\"e\"] };");
    EXPECT_TRUE(policy.IsStatic());
    EXPECT_EQ(policy.TemplateId(), nullptr);
    // Rust's walk: the node, then its children last to first.
    const std::vector<EntityUid> expected{Uid("User", "e"),     Uid("User", "d"),     Uid("User", "c"),
                                          Uid("Folder", "r"),   Uid("Action", "b"),   Uid("Action", "a"),
                                          Uid("User", "p")};
    EXPECT_EQ(policy.EntityLiterals(), expected);

    PolicySet set;
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t", "permit(principal in ?principal, action == Action::\"view\", "
                                                  "resource is Photo in ?resource);"))
                    .has_value());
    const Template& body = *set.GetTemplate("t");
    EXPECT_EQ(std::vector(body.Slots().begin(), body.Slots().end()),
              (std::vector{SlotId::Resource, SlotId::Principal}));
    ASSERT_TRUE(set.Link("t", PolicyId{"l"},
                         SlotBindings{}.Bind(SlotId::Principal, Uid("Group", "g")).Bind(SlotId::Resource, Uid("Album", "x")))
                    .has_value());
    const Policy& link = *set.GetPolicy("l");
    EXPECT_FALSE(link.IsStatic());
    EXPECT_EQ(*link.TemplateId(), PolicyId{"t"});
    EXPECT_TRUE(std::holds_alternative<SlotReference>(std::get<ScopeIn>(body.Principal().constraint).target));
    EXPECT_EQ(std::get<EntityUid>(std::get<ScopeIn>(link.Principal().constraint).target), Uid("Group", "g"));
    const ResourceConstraint resource = link.Resource();
    const auto& isIn = std::get<ScopeIsIn>(resource.constraint);
    EXPECT_EQ(isIn.type, EntityType::Parse("Photo").value());
    EXPECT_EQ(std::get<EntityUid>(isIn.target), Uid("Album", "x"));
    EXPECT_EQ(std::get<ActionEqual>(link.Action()).action, Uid("Action", "view"));
    // Slots are not literals.
    EXPECT_EQ(link.EntityLiterals(), std::vector{Uid("Action", "view")});
}

TEST(PolicySetApiTests, FailedRemovalsReorderAsRust)
{
    PolicySet set;
    ASSERT_TRUE(set.Add(Static("s", PermitAll)).has_value());
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t", PrincipalTemplate)).has_value());
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("u", PrincipalTemplate)).has_value());
    ASSERT_TRUE(set.Link("t", PolicyId{"l"}, Principal("x")).has_value());
    ASSERT_TRUE(set.Add(Static("z", PermitAll)).has_value());

    EXPECT_FALSE(set.Unlink("s").has_value());
    EXPECT_EQ(Ids(set), (std::vector<std::string>{"l", "z", "s"}));
    EXPECT_FALSE(set.RemoveStatic("l").has_value());
    EXPECT_EQ(Ids(set), (std::vector<std::string>{"z", "s", "l"}));
    EXPECT_FALSE(set.RemoveTemplate("t").has_value());
    EXPECT_EQ(TemplateIds(set), (std::vector<std::string>{"u", "t"}));
}

} // namespace
