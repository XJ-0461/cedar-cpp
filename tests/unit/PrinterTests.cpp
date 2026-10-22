#include <cedar/Syntax.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Every expected text here is what the reference Rust CLI prints for the same policy: the Cedar
// text was translated to JSON and back (`cedar translate-policy --direction cedar-to-json`, then
// `json-to-cedar`), which prints Rust's canonical AST form. Expression cases are the `when` body
// of `permit(principal, action, resource) when { ... };`.

namespace
{

using namespace cedar;
using Index = ExpressionIndex;

auto Uid(const std::string_view type, const std::string_view id) -> EntityUid
{
    return EntityUid::Create(type, id).value();
}

auto Type(const std::string_view name) -> EntityType { return EntityType::Parse(name).value(); }

auto Function(const std::string_view name) -> Name { return Name::Parse(name).value(); }

auto ContextAttribute(ExpressionBuilder& b, const std::string_view attribute) -> Index
{
    return b.GetAttribute(b.Variable(Variable::Context), attribute);
}

auto Call(ExpressionBuilder& b, const std::string_view name, const std::initializer_list<Index> arguments) -> Index
{
    return b.ExtensionCall(Function(name), std::span{arguments.begin(), arguments.size()});
}

auto SetOf(ExpressionBuilder& b, const std::initializer_list<Index> elements) -> Index
{
    return b.SetLiteral(std::span{elements.begin(), elements.size()});
}

struct ExpressionCase
{
    std::string_view written;
    Index (*build)(ExpressionBuilder&);
    std::string_view expected;
};

const std::array ExpressionCases{
    ExpressionCase{"(context.a || context.b) && context.c",
                   [](ExpressionBuilder& b) {
                       return b.And(b.Or(ContextAttribute(b, "a"), ContextAttribute(b, "b")), ContextAttribute(b, "c"));
                   },
                   "(context.a || context.b) && context.c"},
    ExpressionCase{"context.a && (context.b && context.c)",
                   [](ExpressionBuilder& b) {
                       return b.And(ContextAttribute(b, "a"), b.And(ContextAttribute(b, "b"), ContextAttribute(b, "c")));
                   },
                   "context.a && (context.b && context.c)"},
    ExpressionCase{"-(-1)", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::Negate, b.Long(-1)); },
                   "-((-1))"},
    ExpressionCase{"-(context.n + 1)",
                   [](ExpressionBuilder& b) {
                       return b.Unary(UnaryOperator::Negate,
                                      b.Binary(BinaryOperator::Add, ContextAttribute(b, "n"), b.Long(1)));
                   },
                   "-(context.n + 1)"},
    ExpressionCase{"context.a != context.b",
                   [](ExpressionBuilder& b) { return b.NotEqual(ContextAttribute(b, "a"), ContextAttribute(b, "b")); },
                   "!(context.a == context.b)"},
    ExpressionCase{"!!context.a", [](ExpressionBuilder& b) { return b.Not(b.Not(ContextAttribute(b, "a"))); },
                   "!(!context.a)"},
    ExpressionCase{"(1 + 2) * 3",
                   [](ExpressionBuilder& b) {
                       return b.Binary(BinaryOperator::Multiply, b.Binary(BinaryOperator::Add, b.Long(1), b.Long(2)),
                                       b.Long(3));
                   },
                   "(1 + 2) * 3"},
    ExpressionCase{"1 - (2 - 3) - 4",
                   [](ExpressionBuilder& b) {
                       const Index inner = b.Binary(BinaryOperator::Subtract, b.Long(2), b.Long(3));
                       return b.Binary(BinaryOperator::Subtract, b.Binary(BinaryOperator::Subtract, b.Long(1), inner),
                                       b.Long(4));
                   },
                   "1 - (2 - 3) - 4"},
    ExpressionCase{"context.n > 3 || context.n >= 4",
                   [](ExpressionBuilder& b) {
                       return b.Or(b.Greater(ContextAttribute(b, "n"), b.Long(3)),
                                   b.GreaterEqual(ContextAttribute(b, "n"), b.Long(4)));
                   },
                   "(!(context.n <= 3)) || (!(context.n < 4))"},
    ExpressionCase{"(if context.a then 1 else 2) + 1",
                   [](ExpressionBuilder& b) {
                       return b.Binary(BinaryOperator::Add, b.If(ContextAttribute(b, "a"), b.Long(1), b.Long(2)),
                                       b.Long(1));
                   },
                   "(if context.a then 1 else 2) + 1"},
    ExpressionCase{"if (if context.a then context.b else context.c) then [1] else {}",
                   [](ExpressionBuilder& b) {
                       const Index condition =
                           b.If(ContextAttribute(b, "a"), ContextAttribute(b, "b"), ContextAttribute(b, "c"));
                       return b.If(condition, SetOf(b, {b.Long(1)}), b.RecordLiteral({}).value());
                   },
                   "if if context.a then context.b else context.c then [1] else {}"},
    ExpressionCase{"(context.n + 1).x",
                   [](ExpressionBuilder& b) {
                       return b.GetAttribute(b.Binary(BinaryOperator::Add, ContextAttribute(b, "n"), b.Long(1)), "x");
                   },
                   "(context.n + 1).x"},
    ExpressionCase{"(-1).x", [](ExpressionBuilder& b) { return b.GetAttribute(b.Long(-1), "x"); }, "(-1).x"},
    ExpressionCase{"-9223372036854775808",
                   [](ExpressionBuilder& b) { return b.Long(std::numeric_limits<std::int64_t>::min()); },
                   "(-9223372036854775808)"},
    ExpressionCase{"[1, 2].contains(3) && (if context.a then [1] else [2]).isEmpty()",
                   [](ExpressionBuilder& b) {
                       const Index contains =
                           b.Binary(BinaryOperator::Contains, SetOf(b, {b.Long(1), b.Long(2)}), b.Long(3));
                       const Index branch = b.If(ContextAttribute(b, "a"), SetOf(b, {b.Long(1)}), SetOf(b, {b.Long(2)}));
                       return b.And(contains, b.Unary(UnaryOperator::IsEmpty, branch));
                   },
                   "[1, 2].contains(3) && (if context.a then [1] else [2]).isEmpty()"},
    ExpressionCase{"context.s.containsAll([1, -2]) || (context.s).containsAny([])",
                   [](ExpressionBuilder& b) {
                       const Index all = b.Binary(BinaryOperator::ContainsAll, ContextAttribute(b, "s"),
                                                  SetOf(b, {b.Long(1), b.Long(-2)}));
                       return b.Or(all, b.Binary(BinaryOperator::ContainsAny, ContextAttribute(b, "s"), SetOf(b, {})));
                   },
                   "context.s.containsAll([1, (-2)]) || context.s.containsAny([])"},
    ExpressionCase{"principal.getTag(\"t\") == principal.hasTag(\"t\")",
                   [](ExpressionBuilder& b) {
                       const Index get = b.Binary(BinaryOperator::GetTag, b.Variable(Variable::Principal), b.String("t"));
                       return b.Equal(get,
                                      b.Binary(BinaryOperator::HasTag, b.Variable(Variable::Principal), b.String("t")));
                   },
                   "principal.getTag(\"t\") == principal.hasTag(\"t\")"},
    ExpressionCase{"ip(\"10.0.0.1\").isInRange(ip(\"10.0.0.0/8\")), constructors folded",
                   [](ExpressionBuilder& b) {
                       const Index address = b.Fold(Call(b, "ip", {b.String("10.0.0.1")}),
                                                    Value::FromIpAddress(IpAddress::V4(0x0A000001)));
                       const Index range = b.Fold(Call(b, "ip", {b.String("10.0.0.0/8")}),
                                                  Value::FromIpAddress(IpAddress::V4(0x0A000000, 8)));
                       return Call(b, "isInRange", {address, range});
                   },
                   "ip(\"10.0.0.1\").isInRange(ip(\"10.0.0.0/8\"))"},
    ExpressionCase{"datetime(\"2024-01-01\").offset(duration(\"1h\")) == (context.d).offset(duration(\"-2ms\"))",
                   [](ExpressionBuilder& b) {
                       const Index start = b.Fold(Call(b, "datetime", {b.String("2024-01-01")}),
                                                  Value::FromDatetime(Datetime{1704067200000}));
                       const Index hour =
                           b.Fold(Call(b, "duration", {b.String("1h")}), Value::FromDuration(Duration{3600000}));
                       const Index shifted = b.Fold(Call(b, "offset", {start, hour}),
                                                    Value::FromDatetime(Datetime{1704070800000}));
                       const Index back =
                           b.Fold(Call(b, "duration", {b.String("-2ms")}), Value::FromDuration(Duration{-2}));
                       return b.Equal(shifted, Call(b, "offset", {ContextAttribute(b, "d"), back}));
                   },
                   "datetime(\"2024-01-01\").offset(duration(\"1h\")) == context.d.offset(duration(\"-2ms\"))"},
    ExpressionCase{"principal like \"a*b\\*c\\u{2a}\"",
                   [](ExpressionBuilder& b) {
                       const std::array<std::string_view, 3> literals{"a", "b*c", ""};
                       return b.Like(b.Variable(Variable::Principal), literals);
                   },
                   R"(principal like "a*b\*c*")"},
    ExpressionCase{"context.s like \"e\\u{301}*\\u{301}x\"",
                   [](ExpressionBuilder& b) {
                       const std::array<std::string_view, 2> literals{"é", "́x"};
                       return b.Like(ContextAttribute(b, "s"), literals);
                   },
                   R"(context.s like "e\u{301}*\u{301}x")"},
    ExpressionCase{"principal is User in Group::\"g\"",
                   [](ExpressionBuilder& b) {
                       return b.IsIn(b.Variable(Variable::Principal), Type("User"), b.Entity(Uid("Group", "g")));
                   },
                   R"((principal is User) && (principal in Group::"g"))"},
    ExpressionCase{"{a: 1, \"b c\": [1, -2], \"if\": true, \"__cedar\": false}",
                   [](ExpressionBuilder& b) {
                       const std::array fields{
                           RecordLiteralField{"a", b.Long(1)},
                           RecordLiteralField{"b c", SetOf(b, {b.Long(1), b.Long(-2)})},
                           RecordLiteralField{"if", b.Bool(true)},
                           RecordLiteralField{"__cedar", b.Bool(false)},
                       };
                       return b.RecordLiteral(fields).value();
                   },
                   R"({"__cedar": false, a: 1, "b c": [1, (-2)], "if": true})"},
    ExpressionCase{"context[\"a b\"] && context[\"if\"] && context[\"__cedar\"]",
                   [](ExpressionBuilder& b) {
                       return b.And(b.And(ContextAttribute(b, "a b"), ContextAttribute(b, "if")),
                                    ContextAttribute(b, "__cedar"));
                   },
                   R"(context["a b"] && context["if"] && context["__cedar"])"},
    ExpressionCase{"principal has a.b.c && principal has \"x y\" && principal has \"if\"",
                   [](ExpressionBuilder& b) {
                       const std::array<std::string_view, 3> path{"a", "b", "c"};
                       const Index deep = b.HasAttribute(b.Variable(Variable::Principal), path);
                       const Index spaced = b.HasAttribute(b.Variable(Variable::Principal), "x y");
                       return b.And(b.And(deep, spaced), b.HasAttribute(b.Variable(Variable::Principal), "if"));
                   },
                   R"((principal has a.b.c) && (principal has "x y") && (principal has "if"))"},
    ExpressionCase{"\"\\u{301}x\\n\\t\\\"'\\\\é\" == \"a\\u{301}\"",
                   [](ExpressionBuilder& b) {
                       return b.Equal(b.String("́x\n\t\"'\\é"), b.String("á"));
                   },
                   R"("\u{301}x\n\t\"\'\\)" "é" R"(" == ")" "á" R"(")"},
    ExpressionCase{"Photo::\"a\\\"b\\\\c\\n\" in context.albums",
                   [](ExpressionBuilder& b) {
                       return b.In(b.Entity(Uid("Photo", "a\"b\\c\n")), ContextAttribute(b, "albums"));
                   },
                   R"(Photo::"a\"b\\c\n" in context.albums)"},
    ExpressionCase{"!(principal in [Group::\"a\", Group::\"b\"]) && -context.n < 0",
                   [](ExpressionBuilder& b) {
                       const Index groups = SetOf(b, {b.Entity(Uid("Group", "a")), b.Entity(Uid("Group", "b"))});
                       const Index notIn = b.Not(b.In(b.Variable(Variable::Principal), groups));
                       const Index negated = b.Unary(UnaryOperator::Negate, ContextAttribute(b, "n"));
                       return b.And(notIn, b.Binary(BinaryOperator::Less, negated, b.Long(0)));
                   },
                   R"((!(principal in [Group::"a", Group::"b"])) && ((-(context.n)) < 0))"},
    ExpressionCase{"context.a.b.c == !context.d",
                   [](ExpressionBuilder& b) {
                       const Index path = b.GetAttribute(b.GetAttribute(ContextAttribute(b, "a"), "b"), "c");
                       return b.Equal(path, b.Not(ContextAttribute(b, "d")));
                   },
                   "context.a.b.c == (!context.d)"},
};

TEST(PrinterTests, ExpressionsPrintAsRustPrintsThem)
{
    for (const ExpressionCase& testCase : ExpressionCases)
    {
        ExpressionArena arena;
        ExpressionBuilder builder{arena};
        const Index root = testCase.build(builder);
        EXPECT_EQ(ToCedar(Expression{std::move(arena), root}), testCase.expected) << "written: " << testCase.written;
    }
}

// ---- Policies ----

// @zeta("last") @advice @if("a\"b\n\u{301}")
// forbid(principal == User::"a\"b", action == Action::"view", resource);
auto AnnotatedForbid(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}, .effect = Effect::Forbid};
    EXPECT_TRUE(definition.annotations.Insert(Symbol::Intern("zeta"), "last"));
    EXPECT_TRUE(definition.annotations.Insert(Symbol::Intern("advice"), std::nullopt));
    EXPECT_TRUE(definition.annotations.Insert(Symbol::Intern("if"), "a\"b\ń"));
    definition.principal.constraint = ScopeEqual{Uid("User", "a\"b")};
    definition.action = ActionEqual{Uid("Action", "view")};
    return Template::Create(std::move(definition));
}
constexpr std::string_view AnnotatedForbidText = "@advice(\"\")\n"
                                                 "@if(\"a\\\"b\\ń\")\n"
                                                 "@zeta(\"last\")\n"
                                                 "forbid(\n"
                                                 "  principal == User::\"a\\\"b\",\n"
                                                 "  action == Action::\"view\",\n"
                                                 "  resource\n"
                                                 ");";

// permit(principal in Group::"g", action in [Action::"a", Action::"b"], resource is Photo)
// when { context.a } unless { context.b };
auto GroupPermit(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}};
    definition.principal.constraint = ScopeIn{Uid("Group", "g")};
    definition.action = ActionIn{{Uid("Action", "a"), Uid("Action", "b")}};
    definition.resource.constraint = ScopeIs{Type("Photo")};
    ExpressionBuilder b{definition.arena};
    definition.nonScopeCondition = b.And(ContextAttribute(b, "a"), b.Not(ContextAttribute(b, "b")));
    return Template::Create(std::move(definition));
}
constexpr std::string_view GroupPermitText = "permit(\n"
                                             "  principal in Group::\"g\",\n"
                                             "  action in [Action::\"a\",Action::\"b\"],\n"
                                             "  resource is Photo\n"
                                             ") when {\n"
                                             "  context.a && (!context.b)\n"
                                             "};";

// permit(principal is User, action in Action::"one", resource is Photo in Album::"a");
auto TypedPermit(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}};
    definition.principal.constraint = ScopeIs{Type("User")};
    definition.action = ActionIn{{Uid("Action", "one")}, false};
    definition.resource.constraint = ScopeIsIn{Type("Photo"), Uid("Album", "a")};
    return Template::Create(std::move(definition));
}
constexpr std::string_view TypedPermitText = "permit(\n"
                                             "  principal is User,\n"
                                             "  action in [Action::\"one\"],\n"
                                             "  resource is Photo in Album::\"a\"\n"
                                             ");";

// permit(principal == ?principal, action, resource in ?resource) when { resource.owner == principal };
auto OwnerTemplate(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}};
    definition.principal.constraint = ScopeEqual{SlotReference{}};
    definition.resource.constraint = ScopeIn{SlotReference{}};
    ExpressionBuilder b{definition.arena};
    definition.nonScopeCondition =
        b.Equal(b.GetAttribute(b.Variable(Variable::Resource), "owner"), b.Variable(Variable::Principal));
    return Template::Create(std::move(definition));
}
constexpr std::string_view OwnerTemplateText = "permit(\n"
                                               "  principal == ?principal,\n"
                                               "  action,\n"
                                               "  resource in ?resource\n"
                                               ") when {\n"
                                               "  resource.owner == principal\n"
                                               "};";

// permit(principal is User in ?principal, action, resource == Photo::"p");
auto MemberTemplate(const std::string_view id) -> std::shared_ptr<const Template>
{
    TemplateDefinition definition{.id = PolicyId{id}};
    definition.principal.constraint = ScopeIsIn{Type("User"), SlotReference{}};
    definition.resource.constraint = ScopeEqual{Uid("Photo", "p")};
    return Template::Create(std::move(definition));
}
constexpr std::string_view MemberTemplateText = "permit(\n"
                                                "  principal is User in ?principal,\n"
                                                "  action,\n"
                                                "  resource == Photo::\"p\"\n"
                                                ");";

TEST(PrinterTests, PoliciesAndTemplatesPrintAsRustPrintsThem)
{
    EXPECT_EQ(ToCedar(Policy::Static(AnnotatedForbid("p")).value()), AnnotatedForbidText);
    EXPECT_EQ(ToCedar(Policy::Static(GroupPermit("p")).value()), GroupPermitText);
    EXPECT_EQ(ToCedar(Policy::Static(TypedPermit("p")).value()), TypedPermitText);
    EXPECT_EQ(ToCedar(*OwnerTemplate("t")), OwnerTemplateText);
    EXPECT_EQ(ToCedar(*MemberTemplate("t")), MemberTemplateText);
}

TEST(PrinterTests, PolicySetPrintsPoliciesThenTemplatesEachSortedById)
{
    PolicySet set;
    ASSERT_TRUE(set.Add(Policy::Static(TypedPermit("b")).value()));
    ASSERT_TRUE(set.Add(Policy::Static(AnnotatedForbid("a10")).value()));
    ASSERT_TRUE(set.Add(Policy::Static(GroupPermit("a9")).value()));
    ASSERT_TRUE(set.AddTemplate(MemberTemplate("t1")));
    ASSERT_TRUE(set.AddTemplate(OwnerTemplate("t0")));
    const std::string expected = std::string{AnnotatedForbidText} + "\n\n" + std::string{GroupPermitText} + "\n\n" +
                                 std::string{TypedPermitText} + "\n\n" + std::string{OwnerTemplateText} + "\n\n" +
                                 std::string{MemberTemplateText};
    EXPECT_EQ(ToCedar(set), expected);
}

// Rust refuses to print a linked policy in Cedar syntax (`Policy::to_cedar` returns None); this
// prints the template's canonical text with each slot replaced by its entity.
TEST(PrinterTests, LinkedPolicyPrintsTheTemplateWithItsBindings)
{
    SlotBindings bindings;
    bindings.Bind(SlotId::Principal, Uid("User", "alice")).Bind(SlotId::Resource, Uid("Folder", "f\""));
    const Policy linked = Policy::Link(OwnerTemplate("t"), PolicyId{"link"}, bindings).value();
    EXPECT_EQ(ToCedar(linked), "permit(\n"
                               "  principal == User::\"alice\",\n"
                               "  action,\n"
                               "  resource in Folder::\"f\\\"\"\n"
                               ") when {\n"
                               "  resource.owner == principal\n"
                               "};");
}

TEST(PrinterTests, RetainedSourceTextPrintsVerbatim)
{
    constexpr std::string_view written = "permit (principal,action,resource) // as written\n;";
    TemplateDefinition definition{.id = PolicyId{"p"}};
    definition.source = MakeSourceText("<policy>", "  " + std::string{written} + "  ");
    definition.span = SourceSpan{2, static_cast<std::uint32_t>(2 + written.size())};
    const auto body = Template::Create(std::move(definition));
    EXPECT_EQ(ToCedar(*body), written);
    EXPECT_EQ(ToCedar(Policy::Static(body).value()), written);
}

} // namespace
