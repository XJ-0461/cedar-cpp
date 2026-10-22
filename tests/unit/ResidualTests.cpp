#include "partial-evaluation/Residual.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Typechecker.hpp"

#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace
{

using cedar::ExpressionBuilder;
using cedar::ExpressionIndex;
using cedar::detail::NodeTypes;
using cedar::detail::Residual;
using cedar::detail::ResidualTree;
using cedar::detail::Type;
using cedar::detail::TypecheckEnvironment;

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

auto Uid(const std::string_view type, const std::string_view id) -> cedar::EntityUid
{
    return *cedar::EntityUid::Create(type, id);
}

/// The partial evaluation corpus schema, and its request environment for `User` viewing a `Doc`.
struct Fixture
{
    cedar::Schema schema = cedar::ParseSchema(ReadFile(CEDAR_PARTIAL_EVALUATION_SCHEMA))->value;

    [[nodiscard]] auto Environment() const -> TypecheckEnvironment
    {
        for (const auto& environment : cedar::detail::SchemaAccess::Validator(schema).RequestEnvironments())
        {
            if (environment.principal == Uid("User", "x").Type() &&
                environment.action->name == Uid("Action", "view") && environment.resource == Uid("Doc", "x").Type())
            {
                auto result = TypecheckEnvironment::FromUnlinked(environment);
                result.principalSlot = Uid("User", "x").Type();
                result.resourceSlot = Uid("Doc", "x").Type();
                return result;
            }
        }
        ADD_FAILURE() << "no User/view/Doc request environment";
        return TypecheckEnvironment::FromUnlinked(
            cedar::detail::SchemaAccess::Validator(schema).RequestEnvironments().front());
    }

    /// The node types of the first template of `text`, typechecked in Environment().
    [[nodiscard]] auto Typecheck(const cedar::Template& body) const -> NodeTypes
    {
        NodeTypes types;
        const cedar::detail::Typechecker typechecker{cedar::detail::SchemaAccess::Validator(schema),
                                                     cedar::ValidationMode::Strict, body};
        const auto result = typechecker.Check(body.Condition(), Environment(), &types);
        EXPECT_NE(result.outcome, cedar::detail::PolicyCheckOutcome::Fail) << ToCedar(body);
        return types;
    }

    /// The residual of the one static policy in `text`.
    [[nodiscard]] auto ResidualOf(const std::string& text) const -> Residual
    {
        const auto policies = cedar::ParsePolicySet(text);
        EXPECT_TRUE(policies.has_value()) << text;
        const cedar::Policy& policy = *policies->Policies().front();
        auto residual = Residual::FromTypedPolicy(policy, Typecheck(policy.GetTemplate()));
        EXPECT_TRUE(residual.has_value()) << (residual ? "" : residual.error().Render());
        return *residual;
    }
};

auto Dump(const Residual& residual) -> std::string
{
    return cedar::DumpExpression(residual.Arena(), residual.Root());
}

/// The scope a policy with an unconstrained scope wraps its `when` condition in.
auto InScope(const std::string& condition) -> std::string
{
    return "(&& true (&& true (&& true " + condition + ")))";
}

// ---- The typed-tree reshapings ----
// The policies are `tests/parity/partial-evaluation/Evaluation/policies/reshape.cedar`; the oracle's
// residuals for them (case `typed-tree-reshaping:and-or-if`) are these trees after evaluation.

TEST(Residual, AndWhoseLeftIsTypedFalseIsItsLeftOperand)
{
    const Fixture fixture;
    const Residual residual = fixture.ResidualOf(
        R"(permit(principal, action, resource) when { (principal.age > 0 && false) && ("a" < 1) };)");
    EXPECT_EQ(Dump(residual), InScope(R"((&& (! (<= (. principal "age") 0)) false))"));
    EXPECT_EQ(residual.GetType(), cedar::detail::SingletonBooleanType(false));
}

TEST(Residual, OrWhoseLeftIsTypedTrueIsItsLeftOperand)
{
    const Fixture fixture;
    const Residual residual = fixture.ResidualOf(
        R"(permit(principal, action, resource) when { (principal.age > 0 || true) || ("a" < 1) };)");
    EXPECT_EQ(Dump(residual), InScope(R"((|| (! (<= (. principal "age") 0)) true))"));
}

TEST(Residual, IfWhoseTestIsTypedTrueTakesTheThenBranchInBothArms)
{
    const Fixture fixture;
    const Residual residual = fixture.ResidualOf(
        R"(permit(principal, action, resource) when {
               if (principal.age > 0 || true) then resource.isPublic else principal.age > 1 };)");
    EXPECT_EQ(Dump(residual), InScope(R"((if (|| (! (<= (. principal "age") 0)) true) )"
                                      R"((. resource "isPublic") (. resource "isPublic")))"));
}

TEST(Residual, IfWhoseTestIsTypedFalseTakesTheElseBranchInBothArms)
{
    const Fixture fixture;
    const Residual residual = fixture.ResidualOf(
        R"(permit(principal, action, resource) when {
               if (principal.age > 0 && false) then resource.isPublic else principal.age > 1 };)");
    EXPECT_EQ(Dump(residual), InScope(R"((if (&& (! (<= (. principal "age") 0)) false) )"
                                      R"((! (<= (. principal "age") 1)) (! (<= (. principal "age") 1))))"));
}

TEST(Residual, AndWithALiteralFalseLeftIsThatLiteral)
{
    const Fixture fixture;
    const Residual residual =
        fixture.ResidualOf(R"(permit(principal, action, resource) when { false && ("a" < 1) };)");
    ASSERT_NE(residual.ConcreteValue(), nullptr);
    EXPECT_EQ(*residual.ConcreteValue(), cedar::Value::FromBool(false));
}

TEST(Residual, AnOperandTypedAnyBooleanIsKept)
{
    const Fixture fixture;
    const Residual residual =
        fixture.ResidualOf(R"(permit(principal, action, resource) when { context.flag && context.n > 0 };)");
    EXPECT_EQ(Dump(residual), InScope(R"((&& (. context "flag") (! (<= (. context "n") 0))))"));
}

// ---- Conversion errors and slots ----

TEST(Residual, AnUntypedNodeIsAMissingTypeAnnotation)
{
    const auto policies = cedar::ParsePolicySet("permit(principal, action, resource) when { context.flag };");
    ASSERT_TRUE(policies.has_value());
    const auto residual = Residual::FromTypedPolicy(*policies->Policies().front(), NodeTypes{});
    ASSERT_FALSE(residual.has_value());
    EXPECT_EQ(residual.error().Primary().code, cedar::DiagnosticCode::MissingTypeAnnotation);
}

TEST(Residual, AnUnboundSlotIsAnError)
{
    const Fixture fixture;
    const auto policies = cedar::ParsePolicySet("permit(principal == ?principal, action, resource);");
    ASSERT_TRUE(policies.has_value());
    const cedar::Template& body = *policies->Templates().front();
    const auto residual = Residual::FromTypedTemplate(body, cedar::SlotBindings{}, fixture.Typecheck(body));
    ASSERT_FALSE(residual.has_value());
    EXPECT_EQ(residual.error().Primary().code, cedar::DiagnosticCode::ResidualUnlinkedSlot);
}

TEST(Residual, ABoundSlotIsItsEntity)
{
    const Fixture fixture;
    auto policies = cedar::ParsePolicySet("@id(\"t\") permit(principal == ?principal, action, resource);");
    ASSERT_TRUE(policies.has_value());
    cedar::SlotBindings bindings;
    bindings.Bind(cedar::SlotId::Principal, Uid("User", "alice"));
    ASSERT_TRUE(policies->Link(policies->Templates().front()->Id().Text(), cedar::PolicyId{"link"}, bindings));
    const cedar::Policy& link = *policies->Policies().front();
    const auto residual = Residual::FromTypedPolicy(link, fixture.Typecheck(link.GetTemplate()));
    ASSERT_TRUE(residual.has_value()) << residual.error().Render();
    EXPECT_EQ(Dump(*residual), R"((&& (== principal User::"alice") true))");
    EXPECT_EQ(residual->AllLiteralUids(), std::vector{Uid("User", "alice")});
}

// ---- Literal uids ----

TEST(Residual, LiteralUidsAreSortedWithoutRepeats)
{
    const Fixture fixture;
    const Residual residual = fixture.ResidualOf(
        R"(permit(principal in Group::"b", action in [Action::"view"], resource in Folder::"f")
           when { principal in [Group::"b", Group::"a"] && context.groups.contains(Group::"a") &&
                  resource.owner == User::"z" };)");
    EXPECT_EQ(residual.AllLiteralUids(), (std::vector{Uid("Action", "view"), Uid("Folder", "f"), Uid("Group", "a"),
                                                      Uid("Group", "b"), Uid("User", "z")}));
}

TEST(Residual, LiteralUidsIncludeThoseInsideAConcreteValue)
{
    cedar::ValueArena values;
    const cedar::EntityUid b = Uid("User", "b");
    const cedar::EntityUid a = Uid("User", "a");
    const std::vector elements{cedar::Value::FromEntity(b), cedar::Value::FromEntity(a)};
    const cedar::Value set = values.MakeSet(elements);
    ResidualTree tree{};
    ExpressionBuilder builder{tree.arena};
    tree.root = builder.Literal(set);
    tree.types.resize(tree.arena.Size());
    EXPECT_EQ(Residual::Create(std::move(tree)).AllLiteralUids(), (std::vector{Uid("User", "a"), Uid("User", "b")}));
    EXPECT_TRUE(Residual::Error(Type{}).AllLiteralUids().empty());
}

// ---- can_error_assuming_well_formed ----

using Build = std::function<ExpressionIndex(ExpressionBuilder&)>;

auto Built(const Build& build) -> Residual
{
    ResidualTree tree{};
    ExpressionBuilder builder{tree.arena};
    tree.root = build(builder);
    tree.types.resize(tree.arena.Size());
    return Residual::Create(std::move(tree));
}

auto Principal(ExpressionBuilder& builder) -> ExpressionIndex
{
    return builder.Variable(cedar::Variable::Principal);
}

/// `principal.a`, which can error.
auto Access(ExpressionBuilder& builder) -> ExpressionIndex
{
    return builder.GetAttribute(Principal(builder), "a");
}

using Child = ExpressionIndex (*)(ExpressionBuilder&);

auto BinaryOf(const cedar::BinaryOperator operation, const Child right) -> Build
{
    return [=](ExpressionBuilder& builder)
    { return builder.Binary(operation, Principal(builder), right(builder)); };
}

TEST(Residual, CanErrorAssumingWellFormedFollowsRust)
{
    using cedar::BinaryOperator;
    using cedar::UnaryOperator;
    struct Row
    {
        std::string name;
        Build build;
        bool canError;
    };
    std::vector<Row> rows{
        {"concrete", [](ExpressionBuilder& builder) { return builder.Long(1); }, false},
        {"var", Principal, false},
        {"getattr", Access, true},
        {"and of vars", [](ExpressionBuilder& b) { return b.And(Principal(b), Principal(b)); }, false},
        {"and, right errs", [](ExpressionBuilder& b) { return b.And(Principal(b), Access(b)); }, true},
        {"and, left errs", [](ExpressionBuilder& b) { return b.And(Access(b), Principal(b)); }, true},
        {"or of vars", [](ExpressionBuilder& b) { return b.Or(Principal(b), Principal(b)); }, false},
        {"or, right errs", [](ExpressionBuilder& b) { return b.Or(Principal(b), Access(b)); }, true},
        {"if of vars", [](ExpressionBuilder& b) { return b.If(Principal(b), Principal(b), Principal(b)); }, false},
        {"if, test errs", [](ExpressionBuilder& b) { return b.If(Access(b), Principal(b), Principal(b)); }, true},
        {"if, then errs", [](ExpressionBuilder& b) { return b.If(Principal(b), Access(b), Principal(b)); }, true},
        {"if, else errs", [](ExpressionBuilder& b) { return b.If(Principal(b), Principal(b), Access(b)); }, true},
        {"is var", [](ExpressionBuilder& b) { return b.Is(Principal(b), Uid("User", "x").Type()); }, false},
        {"is getattr", [](ExpressionBuilder& b) { return b.Is(Access(b), Uid("User", "x").Type()); }, true},
        {"like var",
         [](ExpressionBuilder& b)
         {
             const std::vector<std::string_view> pattern{"a", ""};
             return b.Like(Principal(b), pattern);
         },
         false},
        {"like getattr",
         [](ExpressionBuilder& b)
         {
             const std::vector<std::string_view> pattern{"a", ""};
             return b.Like(Access(b), pattern);
         },
         true},
        {"add", BinaryOf(BinaryOperator::Add, Principal), true},
        {"subtract", BinaryOf(BinaryOperator::Subtract, Principal), true},
        {"multiply", BinaryOf(BinaryOperator::Multiply, Principal), true},
        {"getTag", BinaryOf(BinaryOperator::GetTag, Principal), true},
        {"extension call",
         [](ExpressionBuilder& b)
         {
             const ExpressionIndex argument = b.String("1.0");
             return b.ExtensionCall(*cedar::Name::Parse("decimal"), std::span{&argument, 1});
         },
         true},
        {"error()", [](ExpressionBuilder& b) { return b.ExtensionCall(cedar::detail::ResidualErrorName(), {}); },
         true},
        {"has var", [](ExpressionBuilder& b) { return b.HasAttribute(Principal(b), "a"); }, false},
        {"has getattr", [](ExpressionBuilder& b) { return b.HasAttribute(Access(b), "a"); }, true},
        {"has path var",
         [](ExpressionBuilder& b)
         {
             const std::vector<std::string_view> path{"a", "b"};
             return b.HasAttribute(Principal(b), path);
         },
         false},
        {"has path getattr",
         [](ExpressionBuilder& b)
         {
             const std::vector<std::string_view> path{"a", "b"};
             return b.HasAttribute(Access(b), path);
         },
         true},
        {"negate", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::Negate, Principal(b)); }, true},
        {"not var", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::Not, Principal(b)); }, false},
        {"not getattr", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::Not, Access(b)); }, true},
        {"isEmpty var", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::IsEmpty, Principal(b)); }, false},
        {"isEmpty getattr", [](ExpressionBuilder& b) { return b.Unary(UnaryOperator::IsEmpty, Access(b)); }, true},
        {"empty set", [](ExpressionBuilder& b) { return b.SetLiteral({}); }, false},
        {"set of vars",
         [](ExpressionBuilder& b)
         {
             const std::vector elements{Principal(b), Principal(b)};
             return b.SetLiteral(elements);
         },
         false},
        {"set, one errs",
         [](ExpressionBuilder& b)
         {
             const std::vector elements{Principal(b), Access(b)};
             return b.SetLiteral(elements);
         },
         true},
        {"record of vars",
         [](ExpressionBuilder& b)
         {
             const std::vector fields{cedar::RecordLiteralField{"a", Principal(b)}};
             return *b.RecordLiteral(fields);
         },
         false},
        {"record, one errs",
         [](ExpressionBuilder& b)
         {
             const std::vector fields{cedar::RecordLiteralField{"a", Principal(b)},
                                      cedar::RecordLiteralField{"b", Access(b)}};
             return *b.RecordLiteral(fields);
         },
         true},
    };
    for (const BinaryOperator operation :
         {BinaryOperator::Contains, BinaryOperator::ContainsAll, BinaryOperator::ContainsAny, BinaryOperator::Equal,
          BinaryOperator::HasTag, BinaryOperator::In, BinaryOperator::Less, BinaryOperator::LessEqual})
    {
        const std::string name{cedar::ToString(operation)};
        rows.push_back({name + " of vars", BinaryOf(operation, Principal), false});
        rows.push_back({name + ", right errs", BinaryOf(operation, Access), true});
    }
    for (const Row& row : rows)
    {
        EXPECT_EQ(Built(row.build).CanErrorAssumingWellFormed(), row.canError) << row.name;
    }
    EXPECT_TRUE(Residual::Error(Type{}).CanErrorAssumingWellFormed());
}

// ---- The error residual and residual policies ----

TEST(Residual, ErrorIsTheCallErrorWithNoArguments)
{
    const Residual residual = Residual::Error(cedar::detail::AnyBooleanType());
    EXPECT_TRUE(residual.IsError());
    EXPECT_EQ(residual.ConcreteValue(), nullptr);
    EXPECT_EQ(residual.GetType(), cedar::detail::AnyBooleanType());
    EXPECT_EQ(Dump(residual), "(call error)");
    const Residual copy = residual;
    EXPECT_EQ(&copy.Arena(), &residual.Arena());
    EXPECT_FALSE(Built([](ExpressionBuilder& b) { return b.Bool(true); }).IsError());
}

TEST(Residual, ErrorPrintsAndReauthorizesToAnExtensionFunctionLookupError)
{
    const auto policies = cedar::ParsePolicySet(R"(@arm("x") @id("p") forbid(principal, action, resource);)");
    ASSERT_TRUE(policies.has_value());
    const cedar::Policy& source = *policies->Policies().front();
    const cedar::Policy policy = Residual::Error(cedar::detail::AnyBooleanType()).ToPolicy(source);
    EXPECT_NE(cedar::ToCedar(policy).find(") when {\n  error()\n};"), std::string::npos) << cedar::ToCedar(policy);

    cedar::PolicySet set;
    ASSERT_TRUE(set.Add(policy));
    const cedar::Request request{Uid("User", "alice"), Uid("Action", "view"), Uid("Doc", "d")};
    const cedar::Entities entities;
    for (const auto selection : {cedar::PolicySelection::EvaluateEveryPolicy, cedar::PolicySelection::ScopeIndex})
    {
        const cedar::Response response = cedar::IsAuthorized(request, set, entities, {selection});
        EXPECT_EQ(response.decision, cedar::Decision::Deny);
        EXPECT_TRUE(response.reason.empty());
        ASSERT_EQ(response.errors.size(), 1U);
        EXPECT_EQ(response.errors.front().policyId, source.Id());
        EXPECT_EQ(response.errors.front().error.Kind(), cedar::EvaluationErrorKind::ExtensionFunctionLookup);
    }
}

TEST(Residual, ToPolicyKeepsIdEffectAndAnnotationsWithAnUnconstrainedScope)
{
    const Fixture fixture;
    auto policies = cedar::ParsePolicySet(
        R"(@arm("x") @flag forbid(principal == ?principal, action == Action::"view", resource is Doc)
           when { context.flag };)");
    ASSERT_TRUE(policies.has_value());
    cedar::SlotBindings bindings;
    bindings.Bind(cedar::SlotId::Principal, Uid("User", "alice"));
    ASSERT_TRUE(policies->Link(policies->Templates().front()->Id().Text(), cedar::PolicyId{"link"}, bindings));
    const cedar::Policy& link = *policies->Policies().front();
    const auto residual = Residual::FromTypedPolicy(link, fixture.Typecheck(link.GetTemplate()));
    ASSERT_TRUE(residual.has_value()) << residual.error().Render();

    const cedar::Policy policy = residual->ToPolicy(link);
    EXPECT_EQ(policy.Id(), cedar::PolicyId{"link"});
    EXPECT_TRUE(policy.IsStatic());
    EXPECT_EQ(policy.GetEffect(), cedar::Effect::Forbid);
    EXPECT_EQ(policy.GetAnnotations(), link.GetAnnotations());
    EXPECT_TRUE(std::holds_alternative<cedar::ScopeAny>(policy.Principal().constraint));
    EXPECT_TRUE(std::holds_alternative<cedar::ActionAny>(policy.Action()));
    EXPECT_TRUE(std::holds_alternative<cedar::ScopeAny>(policy.Resource().constraint));
    ASSERT_TRUE(policy.GetTemplate().NonScopeCondition().has_value());
    EXPECT_EQ(cedar::DumpExpression(policy.Arena(), *policy.GetTemplate().NonScopeCondition()), Dump(*residual));

    // The residual means what the linked policy means.
    cedar::PolicySet set;
    ASSERT_TRUE(set.Add(policy));
    const std::vector<cedar::RecordEntry> entries{{"flag", cedar::Value::FromBool(true)}};
    const cedar::Request request{Uid("User", "alice"), Uid("Action", "view"), Uid("Doc", "d"),
                                 *cedar::Context::FromPairs(entries)};
    const cedar::Entities entities;
    const cedar::Response response = cedar::IsAuthorized(request, set, entities);
    EXPECT_EQ(response.reason, std::vector{cedar::PolicyId{"link"}});
}

} // namespace
