#include <cedar/Cedar.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace
{

// The schema of cedar-policy-symcc/tests/data/cedar-examples/tinytodo/policies.cedarschema.
// Expected values are read off it with the semantics of cedar-policy/src/api.rs
// (`Schema::principals`, `resources`, `principals_for_action`, `resources_for_action`,
// `actions_for_principal_and_resource`, `ancestors`, `action_groups`, `entity_types`, `actions`,
// `request_envs`), which delegate to cedar-policy-core/src/validator/schema.rs.
constexpr const char* TinyTodoSchema = R"(
type Task = {
    "id": Long,
    "name": String,
    "state": String,
};

type Tasks = Set<Task>;
entity List in [Application] = {
  "editors": Team,
  "name": String,
  "owner": User,
  "readers": Team,
  "tasks": Tasks,
};
entity Application enum ["TinyTodo"];
entity User in [Team, Application] = {
  "joblevel": Long,
  "location": String,
};
entity Team in [Team, Application];

action DeleteList, GetList, UpdateList appliesTo {
  principal: [User],
  resource: [List]
};
action CreateList, GetLists appliesTo {
  principal: [User],
  resource: [Application]
};
action CreateTask, UpdateTask, DeleteTask appliesTo {
  principal: [User],
  resource: [List]
};
action EditShare appliesTo {
  principal: [User],
  resource: [List]
};
)";

auto Texts(const std::vector<cedar::EntityType>& types) -> std::vector<std::string>
{
    std::vector<std::string> texts;
    for (const cedar::EntityType& type : types) { texts.push_back(type.ToString()); }
    return texts;
}

auto Texts(const std::vector<cedar::EntityUid>& uids) -> std::vector<std::string>
{
    std::vector<std::string> texts;
    for (const cedar::EntityUid& uid : uids) { texts.push_back(uid.ToString()); }
    return texts;
}

auto Action(const char* id) -> cedar::EntityUid { return *cedar::ParseEntityUid(std::string{"Action::\""} + id + "\""); }

auto Type(const char* name) -> cedar::EntityType { return *cedar::EntityType::Parse(name); }

using Strings = std::vector<std::string>;

class SchemaIntrospection : public testing::Test
{
protected:
    void SetUp() override
    {
        auto parsed = cedar::ParseSchema(TinyTodoSchema);
        ASSERT_TRUE(parsed.has_value()) << parsed.error().Render();
        schema.emplace(parsed->value);
    }

    std::optional<cedar::Schema> schema;
};

TEST_F(SchemaIntrospection, EntityTypesIncludeEnumeratedTypes)
{
    EXPECT_EQ(Texts(schema->EntityTypes()), (Strings{"Application", "List", "Team", "User"}));
}

TEST_F(SchemaIntrospection, Actions)
{
    EXPECT_EQ(Texts(schema->Actions()),
              (Strings{"Action::\"CreateList\"", "Action::\"CreateTask\"", "Action::\"DeleteList\"",
                       "Action::\"DeleteTask\"", "Action::\"EditShare\"", "Action::\"GetList\"",
                       "Action::\"GetLists\"", "Action::\"UpdateList\"", "Action::\"UpdateTask\""}));
    EXPECT_TRUE(schema->ActionGroups().empty());
}

TEST_F(SchemaIntrospection, PrincipalsAndResources)
{
    EXPECT_EQ(Texts(schema->Principals()), (Strings{"User"}));
    EXPECT_EQ(Texts(schema->Resources()), (Strings{"Application", "List"}));
}

TEST_F(SchemaIntrospection, PerAction)
{
    EXPECT_EQ(Texts(*schema->PrincipalsForAction(Action("CreateList"))), (Strings{"User"}));
    EXPECT_EQ(Texts(*schema->ResourcesForAction(Action("CreateList"))), (Strings{"Application"}));
    EXPECT_EQ(Texts(*schema->ResourcesForAction(Action("DeleteList"))), (Strings{"List"}));
    EXPECT_FALSE(schema->PrincipalsForAction(Action("Nope")).has_value());
    EXPECT_FALSE(schema->ResourcesForAction(Action("Nope")).has_value());
}

TEST_F(SchemaIntrospection, ActionsForPrincipalAndResource)
{
    EXPECT_EQ(Texts(schema->ActionsForPrincipalAndResource(Type("User"), Type("List"))),
              (Strings{"Action::\"CreateTask\"", "Action::\"DeleteList\"", "Action::\"DeleteTask\"",
                       "Action::\"EditShare\"", "Action::\"GetList\"", "Action::\"UpdateList\"",
                       "Action::\"UpdateTask\""}));
    EXPECT_EQ(Texts(schema->ActionsForPrincipalAndResource(Type("User"), Type("Application"))),
              (Strings{"Action::\"CreateList\"", "Action::\"GetLists\""}));
    EXPECT_TRUE(schema->ActionsForPrincipalAndResource(Type("List"), Type("User")).empty());
    EXPECT_TRUE(schema->ActionsForPrincipalAndResource(Type("Nope"), Type("List")).empty());
}

// Rust's `ancestors`: every declared type whose descendants contain the type, so `Team in [Team]`
// is its own ancestor and an undeclared type has none to list.
TEST_F(SchemaIntrospection, Ancestors)
{
    EXPECT_EQ(Texts(*schema->Ancestors(Type("User"))), (Strings{"Application", "Team"}));
    EXPECT_EQ(Texts(*schema->Ancestors(Type("Team"))), (Strings{"Application", "Team"}));
    EXPECT_EQ(Texts(*schema->Ancestors(Type("List"))), (Strings{"Application"}));
    EXPECT_TRUE(schema->Ancestors(Type("Application"))->empty());
    EXPECT_FALSE(schema->Ancestors(Type("Nope")).has_value());
}

// One environment per action and each principal and resource type it applies to: seven User/List
// actions and two User/Application actions.
TEST_F(SchemaIntrospection, RequestEnvironments)
{
    const std::vector<cedar::RequestEnvironment> environments = schema->RequestEnvironments();
    ASSERT_EQ(environments.size(), 9U);
    Strings seen;
    for (const cedar::RequestEnvironment& environment : environments)
    {
        EXPECT_EQ(environment.principal.ToString(), "User");
        EXPECT_FALSE(environment.principalSlot.has_value());
        EXPECT_FALSE(environment.resourceSlot.has_value());
        seen.push_back(environment.action.ToString() + " -> " + environment.resource.ToString());
    }
    std::ranges::sort(seen);
    EXPECT_EQ(seen, (Strings{"Action::\"CreateList\" -> Application", "Action::\"CreateTask\" -> List",
                             "Action::\"DeleteList\" -> List", "Action::\"DeleteTask\" -> List",
                             "Action::\"EditShare\" -> List", "Action::\"GetList\" -> List",
                             "Action::\"GetLists\" -> Application", "Action::\"UpdateList\" -> List",
                             "Action::\"UpdateTask\" -> List"}));
}

// Rust's `action_groups`: the actions that have descendants. An action with no `appliesTo` applies
// to nothing and contributes no environment.
TEST(SchemaIntrospectionGroups, ActionGroupsAndActionsWithoutAppliesTo)
{
    auto parsed = cedar::ParseSchema(R"(
entity User; entity Photo;
action read;
action view in [read] appliesTo { principal: User, resource: Photo };
action edit in [read] appliesTo { principal: User, resource: Photo };
)");
    ASSERT_TRUE(parsed.has_value()) << parsed.error().Render();
    EXPECT_EQ(Texts(parsed->value.ActionGroups()), (Strings{"Action::\"read\""}));
    EXPECT_EQ(Texts(*parsed->value.PrincipalsForAction(Action("read"))), (Strings{}));
    EXPECT_EQ(parsed->value.RequestEnvironments().size(), 2U);
}

} // namespace
