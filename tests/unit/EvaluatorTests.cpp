// The evaluator against the reference table (tests/parity/evaluation/Expressions.tsv, made by
// scripts/generate-evaluation-parity-table.py), plus the policy-condition entry and the depth limit.

#include "evaluation/Evaluator.hpp"

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Json.hpp>
#include <cedar/Request.hpp>
#include <cedar/Syntax.hpp>
#include <cedar/Value.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <expected>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

// The request the table was generated with.
constexpr std::string_view Principal = R"(User::"alice")";
constexpr std::string_view Action = R"(Action::"view")";
constexpr std::string_view Resource = R"(Photo::"vacation.jpg")";

auto Uid(const std::string_view text) -> cedar::EntityUid
{
    auto uid = cedar::ParseEntityUid(text);
    if (!uid) { throw std::runtime_error{"bad entity uid in the test: " + std::string{text}}; }
    return *std::move(uid);
}

// The value of a constant expression (no variables, no entity data).
auto Constant(const std::string_view text) -> std::expected<cedar::OwnedValue, std::string>
{
    const auto expression = cedar::ParseExpression(text);
    if (!expression) { return std::unexpected("parse error:\n" + expression.error().Render()); }
    const cedar::Request request{Uid(Principal), Uid(Action), Uid(Resource)};
    const cedar::Entities none;
    auto value = cedar::EvaluateExpression(request, none, *expression);
    if (!value) { return std::unexpected("evaluation error: " + std::string{value.error().Message()}); }
    return *std::move(value);
}

auto ConstantOrThrow(const std::string_view text) -> cedar::OwnedValue
{
    auto value = Constant(text);
    if (!value) { throw std::runtime_error{value.error()}; }
    return *std::move(value);
}

auto ReadFile(const std::string& path) -> std::string
{
    std::ifstream input{path, std::ios::binary};
    if (!input) { throw std::runtime_error{"cannot read " + path}; }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// The entities and context the table was generated with (tests/parity/evaluation/).
auto BuildEntities() -> cedar::Entities
{
    auto store = cedar::EntitiesFromJson(ReadFile(CEDAR_EVALUATION_ENTITIES), "Entities.json");
    if (!store) { throw std::runtime_error{"the test entities do not load: " + store.error().Render()}; }
    return *std::move(store);
}

auto BuildRequest() -> cedar::Request
{
    auto context = cedar::ContextFromJson(ReadFile(CEDAR_EVALUATION_CONTEXT), "Context.json");
    if (!context) { throw std::runtime_error{"the test context does not load: " + context.error().Render()}; }
    return cedar::Request{Uid(Principal), Uid(Action), Uid(Resource), *std::move(context)};
}

auto KindName(const cedar::EvaluationErrorKind kind) -> std::string_view
{
    using enum cedar::EvaluationErrorKind;
    switch (kind)
    {
    case EntityDoesNotExist: return "EntityDoesNotExist";
    case EntityAttributeDoesNotExist: return "EntityAttributeDoesNotExist";
    case RecordAttributeDoesNotExist: return "RecordAttributeDoesNotExist";
    case EntityTagDoesNotExist: return "EntityTagDoesNotExist";
    case ExtensionFunctionLookup: return "ExtensionFunctionLookup";
    case TypeError: return "TypeError";
    case WrongArgumentCount: return "WrongArgumentCount";
    case IntegerOverflow: return "IntegerOverflow";
    case UnlinkedSlot: return "UnlinkedSlot";
    case ExtensionFunctionExecution: return "ExtensionFunctionExecution";
    case RecursionLimit: return "RecursionLimit";
    }
    return "?";
}

struct TableRow
{
    std::size_t line{};
    std::vector<std::string> cells;
};

auto ReadTable(const std::string& path) -> std::vector<TableRow>
{
    std::ifstream input{path};
    std::vector<TableRow> rows;
    std::string line;
    std::size_t number = 0;
    while (std::getline(input, line))
    {
        ++number;
        TableRow row{number, {}};
        std::size_t begin = 0;
        for (;;)
        {
            const std::size_t tab = line.find('\t', begin);
            row.cells.push_back(line.substr(begin, tab == std::string::npos ? std::string::npos : tab - begin));
            if (tab == std::string::npos) { break; }
            begin = tab + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

auto SpanText(const std::optional<cedar::SourceSpan> span) -> std::string
{
    if (!span) { return ""; }
    return std::to_string(span->begin) + ":" + std::to_string(span->Length());
}

TEST(Evaluator, AgreesWithTheReferenceTable)
{
    const cedar::Entities entities = BuildEntities();
    const cedar::Request request = BuildRequest();
    const std::vector<TableRow> rows = ReadTable(CEDAR_EVALUATION_PARITY_TABLE);
    ASSERT_GT(rows.size(), 200U);
    std::size_t spansCompared = 0;
    for (const TableRow& row : rows)
    {
        ASSERT_GE(row.cells.size(), 4U) << "table line " << row.line;
        const std::string& expressionText = row.cells[1];
        SCOPED_TRACE(testing::Message() << "table line " << row.line << ": " << expressionText);
        const auto expression = cedar::ParseExpression(expressionText);
        if (!expression)
        {
            ADD_FAILURE() << "does not parse:\n" << expression.error().Render();
            continue;
        }
        const auto result = cedar::EvaluateExpression(request, entities, *expression);
        if (row.cells[0] == "value")
        {
            if (!result)
            {
                ADD_FAILURE() << "error " << KindName(result.error().Kind()) << ": " << result.error().Message()
                              << "; expected " << row.cells[2];
                continue;
            }
            const auto expected = Constant(row.cells[2]);
            if (!expected)
            {
                ADD_FAILURE() << "the expected value " << row.cells[2] << " does not evaluate: " << expected.error();
                continue;
            }
            EXPECT_EQ(result->Get(), expected->Get())
                << "got " << cedar::ToCedar(result->Get()) << ", expected " << row.cells[2];
            continue;
        }
        ASSERT_EQ(row.cells[0], "error");
        ASSERT_GE(row.cells.size(), 5U);
        if (result)
        {
            ADD_FAILURE() << "got " << cedar::ToCedar(result->Get()) << ", expected error " << row.cells[2];
            continue;
        }
        EXPECT_EQ(KindName(result.error().Kind()), row.cells[2]) << result.error().Message();
        if (!row.cells[4].starts_with("span:"))
        {
            ++spansCompared;
            EXPECT_EQ(SpanText(result.error().Span()), row.cells[3]) << result.error().Message();
        }
    }
    EXPECT_GT(spansCompared, 100U);
}

TEST(Evaluator, EvaluatesPolicyConditionsWithSlotBindings)
{
    const cedar::Entities entities = BuildEntities();
    const cedar::Request request = BuildRequest();

    const auto body = cedar::ParseTemplate(R"(permit(principal in ?principal, action, resource == ?resource)
        when { principal.age > 18 } unless { context.flag && resource.private };)");
    ASSERT_TRUE(body) << body.error().Render();
    const auto bind = [&](const std::string_view principal, const std::string_view resource) {
        auto linked = cedar::Policy::Link(*body, cedar::PolicyId{"link"},
                                          cedar::SlotBindings{}.Bind(cedar::SlotId::Principal, Uid(principal))
                                              .Bind(cedar::SlotId::Resource, Uid(resource)));
        if (!linked) { throw std::runtime_error{"link failed"}; }
        return *std::move(linked);
    };
    EXPECT_EQ(cedar::detail::EvaluateCondition(bind(R"(Group::"users")", Resource), request, entities), true);
    EXPECT_EQ(cedar::detail::EvaluateCondition(bind(R"(Group::"nobody")", Resource), request, entities), false);
    EXPECT_EQ(cedar::detail::EvaluateCondition(bind(R"(Group::"users")", R"(Photo::"other")"), request, entities),
              false);

    // The scope's action list is a folded set literal.
    const auto actions = cedar::ParsePolicy(R"(permit(principal, action in [Action::"edit", Action::"read"], resource);)");
    ASSERT_TRUE(actions) << actions.error().Render();
    EXPECT_EQ(cedar::detail::EvaluateCondition(*actions, request, entities), true);
    const auto otherActions = cedar::ParsePolicy(R"(permit(principal, action in [Action::"edit"], resource);)");
    ASSERT_TRUE(otherActions) << otherActions.error().Render();
    EXPECT_EQ(cedar::detail::EvaluateCondition(*otherActions, request, entities), false);

    const auto failing = cedar::ParsePolicy(R"(permit(principal, action, resource) when { principal.missing };)");
    ASSERT_TRUE(failing) << failing.error().Render();
    const auto error = cedar::detail::EvaluateCondition(*failing, request, entities);
    ASSERT_FALSE(error);
    EXPECT_EQ(error.error().Kind(), cedar::EvaluationErrorKind::EntityAttributeDoesNotExist);

    const auto notBool = cedar::ParsePolicy(R"(permit(principal, action, resource) when { 1 };)");
    ASSERT_TRUE(notBool) << notBool.error().Render();
    const auto typeError = cedar::detail::EvaluateCondition(*notBool, request, entities);
    ASSERT_FALSE(typeError);
    EXPECT_EQ(typeError.error().Kind(), cedar::EvaluationErrorKind::TypeError);
}

TEST(Evaluator, LiteralsLargerThanTheStackBufferSpillToTheHeap)
{
    std::string text{"["};
    for (int element = 0; element < 500; ++element) { text += "{a: [" + std::to_string(element) + "]}, "; }
    text += "{a: [0]}]";
    const auto value = ConstantOrThrow(text);
    EXPECT_EQ(value.Get().AsSet().Size(), 500U);
}

auto Chain(const std::size_t terms) -> std::string
{
    std::string text{"1"};
    for (std::size_t term = 1; term < terms; ++term) { text += " + 1"; }
    return text;
}

TEST(Evaluator, StopsAtTheDepthLimit)
{
    // Rust evaluates 2,000 nested additions and stops before 2,500.
    const auto deep = Constant(Chain(2500));
    ASSERT_TRUE(deep) << deep.error();
    EXPECT_EQ(deep->Get().AsLong(), 2500);

    const auto expression = cedar::ParseExpression(Chain(cedar::detail::MaximumEvaluationDepth + 1));
    ASSERT_TRUE(expression);
    const cedar::Request request{Uid(Principal), Uid(Action), Uid(Resource)};
    const cedar::Entities none;
    const auto result = cedar::EvaluateExpression(request, none, *expression);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().Kind(), cedar::EvaluationErrorKind::RecursionLimit);
}

} // namespace
