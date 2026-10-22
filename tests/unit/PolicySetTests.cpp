#include <cedar/Expression.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace
{

using namespace cedar;

auto Uid(const std::string_view type, const std::string_view id) -> EntityUid
{
    return EntityUid::Create(type, id).value();
}

auto Type(const std::string_view name) -> EntityType { return EntityType::Parse(name).value(); }

constexpr auto Matches(std::span<const std::string_view> literals, std::string_view text) -> bool
{
    return Pattern{literals}.Matches(text);
}

constexpr std::array<std::string_view, 1> Exact{"ab"};
constexpr std::array<std::string_view, 2> Prefix{"a", ""};
constexpr std::array<std::string_view, 3> Middle{"a", "b", "c"};
constexpr std::array<std::string_view, 3> Overlap{"ab", "", "ba"};
static_assert(Matches(Exact, "ab") && !Matches(Exact, "abc"));
static_assert(Matches(Prefix, "a") && Matches(Prefix, "abc") && !Matches(Prefix, "ba"));
static_assert(Matches(Middle, "abc") && Matches(Middle, "axxbyyc") && !Matches(Middle, "acb"));
static_assert(Matches(Overlap, "abba") && !Matches(Overlap, "aba"));

// permit(principal == ?principal, action in [Action::"view", Action::"edit"], resource is Photo in ?resource)
// when { context.level > 3 && resource has owner.name };
auto MakeTemplate(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}};
    EXPECT_TRUE(definition.annotations.Insert(Symbol::Intern("if"), "reserved keys are allowed"));
    EXPECT_TRUE(definition.annotations.Insert(Symbol::Intern("advice"), std::nullopt));
    EXPECT_FALSE(definition.annotations.Insert(Symbol::Intern("advice"), "again"));
    definition.principal.constraint = ScopeEqual{SlotReference{}};
    definition.action = ActionIn{{Uid("Action", "view"), Uid("Action", "edit")}};
    definition.resource.constraint = ScopeIsIn{Type("Photo"), SlotReference{}};

    ExpressionBuilder builder{definition.arena};
    const ExpressionIndex level = builder.GetAttribute(builder.Variable(Variable::Context), "level");
    const ExpressionIndex greater = builder.Greater(level, builder.Long(3));
    const std::array<std::string_view, 2> path{"owner", "name"};
    const ExpressionIndex has = builder.HasAttribute(builder.Variable(Variable::Resource), path);
    definition.nonScopeCondition = builder.And(greater, has);
    return Template::Create(std::move(definition));
}

// forbid(principal, action == Action::"delete", resource);
auto MakeStatic(const std::string_view id) -> Policy
{
    TemplateDefinition definition{.id = PolicyId{id}, .effect = Effect::Forbid};
    definition.action = ActionEqual{Uid("Action", "delete")};
    return Policy::Static(Template::Create(std::move(definition))).value();
}

auto Code(const PolicySetError& error) -> DiagnosticCode { return error.Primary().code; }

TEST(PolicySetTests, CombinedConditionMatchesRustShape)
{
    const auto body = MakeTemplate("t");
    EXPECT_EQ(DumpExpression(body->Arena(), body->Condition()),
              "(&& (== principal ?principal)"
              " (&& (in action (folded [Action::\"edit\", Action::\"view\"]"
              " (set Action::\"view\" Action::\"edit\")))"
              " (&& (&& (is resource Photo) (in resource ?resource))"
              " (&& (! (<= (. context \"level\") 3)) (has resource \"owner\" \"name\")))))");
    EXPECT_EQ(body->Condition(), body->Arena().Root());
    EXPECT_EQ(std::vector(body->Slots().begin(), body->Slots().end()),
              (std::vector{SlotId::Resource, SlotId::Principal}));
    ASSERT_EQ(body->GetAnnotations().Size(), 2U);
    EXPECT_EQ(body->GetAnnotations().Entries()[0].key.Text(), "advice");
    EXPECT_FALSE(body->GetAnnotations().Entries()[0].value.has_value());
    EXPECT_EQ(body->GetAnnotations().Find("advice"), "");
    EXPECT_EQ(body->GetAnnotations().Find("if"), "reserved keys are allowed");
    EXPECT_FALSE(body->OriginalText().has_value());

    const Policy policy = MakeStatic("s");
    EXPECT_TRUE(policy.IsStatic());
    EXPECT_EQ(DumpExpression(policy.Arena(), policy.Condition()),
              "(&& true (&& (== action Action::\"delete\") true))");
}

TEST(PolicySetTests, LinkAndUnlink)
{
    PolicySet set;
    ASSERT_TRUE(set.Add(MakeStatic("s")));
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t")));

    auto bindings = SlotBindings{}.Bind(SlotId::Principal, Uid("User", "alice")).Bind(SlotId::Resource, Uid("Album", "a"));
    ASSERT_TRUE(set.Link("t", PolicyId{"link1"}, bindings));
    ASSERT_TRUE(set.Link("t", PolicyId{"link2"}, bindings));

    const Policy* link = set.GetPolicy("link1");
    ASSERT_NE(link, nullptr);
    EXPECT_FALSE(link->IsStatic());
    EXPECT_EQ(link->GetTemplate().Id(), PolicyId{"t"});
    EXPECT_EQ(*link->Binding(SlotId::Principal), Uid("User", "alice"));
    EXPECT_EQ(set.GetPolicy("t"), nullptr);
    EXPECT_EQ(set.GetTemplate("s"), nullptr);
    EXPECT_EQ(set.PolicyCount(), 3U);
    EXPECT_EQ(set.TemplateCount(), 1U);

    auto removed = set.Unlink("link1");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->Id(), PolicyId{"link1"});
    EXPECT_EQ(set.GetPolicy("link1"), nullptr);
    EXPECT_EQ(std::vector(set.LinkedPolicies("t")->begin(), set.LinkedPolicies("t")->end()),
              std::vector{PolicyId{"link2"}});

    // A relinked id goes to the end of the insertion order.
    ASSERT_TRUE(set.Link("t", PolicyId{"link1"}, bindings));
    std::vector<std::string_view> order;
    for (const auto& policy : set.Policies()) { order.push_back(policy->Id().Text()); }
    EXPECT_EQ(order, (std::vector<std::string_view>{"s", "link2", "link1"}));
}

TEST(PolicySetTests, RejectsCollisionsAndBadLinks)
{
    PolicySet set;
    ASSERT_TRUE(set.Add(MakeStatic("s")));
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t")));
    const auto both = SlotBindings{}.Bind(SlotId::Principal, Uid("User", "a")).Bind(SlotId::Resource, Uid("Album", "b"));
    ASSERT_TRUE(set.Link("t", PolicyId{"l"}, both));

    EXPECT_EQ(Code(set.Add(MakeStatic("s")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.Add(MakeStatic("t")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.Add(MakeStatic("l")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.AddTemplate(MakeTemplate("s")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.AddTemplate(MakeTemplate("t")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.AddTemplate(MakeTemplate("l")).error()), DiagnosticCode::PolicyIdAlreadyDefined);
    EXPECT_EQ(Code(set.Add(*set.GetPolicy("l")).error()), DiagnosticCode::ExpectedStaticPolicyId);

    for (const char* taken : {"s", "t", "l"})
    {
        EXPECT_EQ(Code(set.Link("t", PolicyId{taken}, both).error()), DiagnosticCode::LinkPolicyIdConflict);
    }
    EXPECT_EQ(Code(set.Link("s", PolicyId{"x"}, both).error()), DiagnosticCode::ExpectedTemplateId);
    EXPECT_EQ(Code(set.Link("l", PolicyId{"x"}, both).error()), DiagnosticCode::ExpectedTemplateId);
    EXPECT_EQ(Code(set.Link("missing", PolicyId{"x"}, both).error()), DiagnosticCode::LinkNoSuchTemplate);

    const auto principalOnly = SlotBindings{}.Bind(SlotId::Principal, Uid("User", "a"));
    EXPECT_EQ(Code(set.Link("t", PolicyId{"x"}, principalOnly).error()), DiagnosticCode::LinkWrongSlots);
    // The slot check comes before the id check, as in Rust.
    EXPECT_EQ(Code(set.Link("t", PolicyId{"s"}, principalOnly).error()), DiagnosticCode::LinkWrongSlots);

    // A slot-free template: any binding is extra, and linking it makes a link, not a static policy.
    TemplateDefinition slotFree{.id = PolicyId{"free"}};
    ASSERT_TRUE(set.AddTemplate(Template::Create(std::move(slotFree))));
    EXPECT_EQ(Code(set.Link("free", PolicyId{"x"}, principalOnly).error()), DiagnosticCode::LinkWrongSlots);
    ASSERT_TRUE(set.Link("free", PolicyId{"x"}, SlotBindings{}));
    EXPECT_FALSE(set.GetPolicy("x")->IsStatic());

    EXPECT_EQ(Code(set.Unlink("s").error()), DiagnosticCode::UnlinkLinkNotLink);
    EXPECT_EQ(Code(set.Unlink("t").error()), DiagnosticCode::LinkNonexistent);
    EXPECT_EQ(Code(set.Unlink("missing").error()), DiagnosticCode::LinkNonexistent);
    EXPECT_EQ(Code(set.LinkedPolicies("l").error()), DiagnosticCode::TemplateNonexistent);
    EXPECT_EQ(Code(set.LinkedPolicies("missing").error()), DiagnosticCode::TemplateNonexistent);
    // A static policy is the one link of its own body, as in Rust.
    EXPECT_EQ(std::vector(set.LinkedPolicies("s")->begin(), set.LinkedPolicies("s")->end()), std::vector{PolicyId{"s"}});

    EXPECT_EQ(Code(Policy::Static(MakeTemplate("t2")).error()), DiagnosticCode::LinkWrongSlots);
}

TEST(PolicySetTests, BuilderLiterals)
{
    ExpressionArena arena;
    ExpressionBuilder builder{arena};
    const std::array fields{RecordLiteralField{"b", builder.Long(1)}, RecordLiteralField{"a", builder.String("x\n")}};
    const auto record = builder.RecordLiteral(fields);
    ASSERT_TRUE(record);
    EXPECT_EQ(DumpExpression(arena, *record), "(record (\"a\" \"x\\n\") (\"b\" 1))");

    const std::array duplicate{RecordLiteralField{"a", builder.Long(1)}, RecordLiteralField{"a", builder.Long(2)}};
    EXPECT_EQ(builder.RecordLiteral(duplicate).error().key, "a");

    const std::array literals{std::string_view{"a*"}, std::string_view{""}};
    const ExpressionIndex like = builder.Like(builder.String("a*b"), literals);
    EXPECT_EQ(DumpExpression(arena, like), "(like \"a*b\" \"a\\**\")");
    EXPECT_TRUE(arena.PatternOf(std::get<node::Like>(arena.Node(like))).Matches("a*xyz"));

    const ExpressionIndex call =
        builder.ExtensionCall(Name::Parse("ip").value(), std::array{builder.String("10.0.0.1")}, SourceSpan{4, 20});
    EXPECT_EQ(DumpExpression(arena, call), "(call ip \"10.0.0.1\")");
    EXPECT_EQ(arena.Span(call), (SourceSpan{4, 20}));
    EXPECT_FALSE(arena.Span(like).has_value());
}

TEST(PolicySetTests, SlotsComeFromTheConditionOnly)
{
    // A slot node the condition does not reach (left over by a reader) is not a slot of the template.
    TemplateDefinition definition{.id = PolicyId{"orphan"}};
    ExpressionBuilder builder{definition.arena};
    [[maybe_unused]] const ExpressionIndex unused = builder.Slot(SlotId::Principal);
    const ExpressionIndex written = builder.SetLiteral(std::array{builder.Slot(SlotId::Resource)});
    definition.nonScopeCondition = builder.Fold(written, Value::FromBool(true));
    const auto body = Template::Create(std::move(definition));
    // The resource slot is reached only through the folded node's written form.
    EXPECT_EQ(std::vector(body->Slots().begin(), body->Slots().end()), std::vector{SlotId::Resource});
}

TEST(PolicySetTests, WithIdSharesTheArena)
{
    const auto body = MakeTemplate("t");
    const auto renamed = body->WithId(PolicyId{"u"});
    EXPECT_EQ(renamed->Id(), PolicyId{"u"});
    EXPECT_EQ(body->Id(), PolicyId{"t"});
    EXPECT_EQ(&renamed->Arena(), &body->Arena());
    EXPECT_EQ(renamed->Condition(), body->Condition());
    EXPECT_EQ(renamed->Slots().size(), 2U);
    EXPECT_EQ(std::get<ActionIn>(renamed->Action()).actions.size(), 2U);
}

TEST(PolicySetTests, RemoveAndFromPolicies)
{
    std::vector<Policy> policies;
    policies.push_back(MakeStatic("a"));
    policies.push_back(MakeStatic("b"));
    auto created = PolicySet::FromPolicies(std::move(policies));
    ASSERT_TRUE(created);
    PolicySet set = std::move(*created);
    EXPECT_EQ(set.PolicyCount(), 2U);

    std::vector<Policy> twice;
    twice.push_back(MakeStatic("a"));
    twice.push_back(MakeStatic("a"));
    EXPECT_EQ(Code(PolicySet::FromPolicies(std::move(twice)).error()), DiagnosticCode::PolicyIdAlreadyDefined);

    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t")));
    const auto both = SlotBindings{}.Bind(SlotId::Principal, Uid("User", "a")).Bind(SlotId::Resource, Uid("Album", "b"));
    ASSERT_TRUE(set.Link("t", PolicyId{"l"}, both));

    EXPECT_EQ(Code(set.RemoveStatic("l").error()), DiagnosticCode::PolicyNonexistent);
    EXPECT_EQ(Code(set.RemoveStatic("t").error()), DiagnosticCode::PolicyNonexistent);
    EXPECT_EQ(Code(set.RemoveStatic("missing").error()), DiagnosticCode::PolicyNonexistent);
    auto removed = set.RemoveStatic("a");
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->Id(), PolicyId{"a"});
    EXPECT_EQ(set.GetPolicy("a"), nullptr);
    EXPECT_EQ(set.PolicyCount(), 2U);

    EXPECT_EQ(Code(set.RemoveTemplate("b").error()), DiagnosticCode::TemplateNonexistent);
    EXPECT_EQ(Code(set.RemoveTemplate("l").error()), DiagnosticCode::TemplateNonexistent);
    EXPECT_EQ(Code(set.RemoveTemplate("t").error()), DiagnosticCode::RemoveTemplateWithActiveLinks);
    ASSERT_TRUE(set.Unlink("l"));
    auto removedTemplate = set.RemoveTemplate("t");
    ASSERT_TRUE(removedTemplate);
    EXPECT_EQ((*removedTemplate)->Id(), PolicyId{"t"});
    EXPECT_EQ(set.TemplateCount(), 0U);
    // The ids are free again.
    ASSERT_TRUE(set.Add(MakeStatic("a")));
    ASSERT_TRUE(set.AddTemplate(MakeTemplate("t")));
}

} // namespace
