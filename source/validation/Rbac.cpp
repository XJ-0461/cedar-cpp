#include "validation/Rbac.hpp"

#include "model/FuzzyMatch.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cedar::detail
{

namespace
{

// An entity literal or an `is` type found in a policy, with where it was written.
struct EntityLiteralSite
{
    EntityUid uid;
    std::optional<SourceSpan> span;
};

struct EntityTypeSite
{
    EntityType type;
    std::optional<SourceSpan> span;
};

// What a policy names: the entity literals (Rust's `policy_entity_uids`) and the entity types
// (Rust's `policy_entity_type_names`: each literal's type and each `is` type), scope first.
struct PolicyNames
{
    std::vector<EntityLiteralSite> literals;
    std::vector<EntityTypeSite> types;
};

[[nodiscard]] auto ScopeTarget(const PrincipalOrResourceConstraint& constraint) noexcept -> const EntityUid*
{
    const EntityReference* target = std::visit(
        Overloaded{
            [](const ScopeAny&) -> const EntityReference* { return nullptr; },
            [](const ScopeEqual& equal) -> const EntityReference* { return &equal.target; },
            [](const ScopeIn& in) -> const EntityReference* { return &in.target; },
            [](const ScopeIs&) -> const EntityReference* { return nullptr; },
            [](const ScopeIsIn& isIn) -> const EntityReference* { return &isIn.target; },
        },
        constraint);
    return target != nullptr ? std::get_if<EntityUid>(target) : nullptr;
}

[[nodiscard]] auto ScopeType(const PrincipalOrResourceConstraint& constraint) noexcept -> const EntityType*
{
    if (const auto* is = std::get_if<ScopeIs>(&constraint)) { return &is->type; }
    if (const auto* isIn = std::get_if<ScopeIsIn>(&constraint)) { return &isIn->type; }
    return nullptr;
}

[[nodiscard]] auto ActionLiterals(const ActionConstraint& constraint) noexcept -> std::span<const EntityUid>
{
    if (const auto* equal = std::get_if<ActionEqual>(&constraint)) { return {&equal->action, 1}; }
    if (const auto* in = std::get_if<ActionIn>(&constraint)) { return in->actions; }
    return {};
}

// Rust's `ExprIterator` over the non-scope condition: a node, then its children last to first. A
// folded node stands for its written form; a node shared by two parents is visited per parent.
void CollectConditionNames(const ExpressionArena& arena, const ExpressionIndex root, PolicyNames& names)
{
    std::vector<ExpressionIndex> pending{root};
    const auto pushAll = [&](const std::span<const ExpressionIndex> children) {
        pending.insert(pending.end(), children.begin(), children.end());
    };
    while (!pending.empty())
    {
        const ExpressionIndex index = pending.back();
        pending.pop_back();
        arena.Visit(index, Overloaded{
            [&](const node::Literal& literal) {
                if (literal.value.Kind() != ValueKind::Entity) { return; }
                const EntityUid& uid = literal.value.AsEntity();
                names.literals.push_back(EntityLiteralSite{uid, arena.Span(index)});
                names.types.push_back(EntityTypeSite{uid.Type(), arena.Span(index)});
            },
            [&](const node::Folded& folded) { pending.push_back(folded.written); },
            [](const node::Variable&) {},
            [](const node::Slot&) {},
            [&](const node::If& branch) {
                pending.push_back(branch.condition);
                pending.push_back(branch.thenBranch);
                pending.push_back(branch.elseBranch);
            },
            [&](const node::And& conjunction) {
                pending.push_back(conjunction.left);
                pending.push_back(conjunction.right);
            },
            [&](const node::Or& disjunction) {
                pending.push_back(disjunction.left);
                pending.push_back(disjunction.right);
            },
            [&](const node::Unary& unary) { pending.push_back(unary.operand); },
            [&](const node::Binary& binary) {
                pending.push_back(binary.left);
                pending.push_back(binary.right);
            },
            [&](const node::ExtensionCall& call) { pushAll(arena.Elements(call.arguments)); },
            [&](const node::GetAttribute& get) { pending.push_back(get.object); },
            [&](const node::HasAttribute& has) { pending.push_back(has.object); },
            [&](const node::Like& like) { pending.push_back(like.operand); },
            [&](const node::Is& is) {
                names.types.push_back(EntityTypeSite{is.type, arena.Span(index)});
                pending.push_back(is.operand);
            },
            [&](const node::SetLiteral& set) { pushAll(arena.Elements(set.elements)); },
            [&](const node::RecordLiteral& record) {
                for (const RecordLiteralField& field : arena.Elements(record.fields)) { pending.push_back(field.value); }
            },
        });
    }
}

[[nodiscard]] auto CollectNames(const Template& body) -> PolicyNames
{
    PolicyNames names;
    const std::optional<SourceSpan> policySpan = body.SourceExtent();
    const auto addScope = [&](const PrincipalOrResourceConstraint& constraint) {
        if (const EntityUid* uid = ScopeTarget(constraint))
        {
            names.literals.push_back(EntityLiteralSite{*uid, policySpan});
            names.types.push_back(EntityTypeSite{uid->Type(), policySpan});
        }
        if (const EntityType* type = ScopeType(constraint)) { names.types.push_back(EntityTypeSite{*type, policySpan}); }
    };
    addScope(body.Principal().constraint);
    for (const EntityUid& action : ActionLiterals(body.Action()))
    {
        names.literals.push_back(EntityLiteralSite{action, policySpan});
        names.types.push_back(EntityTypeSite{action.Type(), policySpan});
    }
    addScope(body.Resource().constraint);
    if (const std::optional<ExpressionIndex> condition = body.NonScopeCondition())
    {
        CollectConditionNames(body.Arena(), *condition, names);
    }
    return names;
}

[[nodiscard]] auto MakeDiagnostic(const DiagnosticCode code, const Severity severity, std::string message,
                                  std::optional<std::string> help, const std::optional<SourceSpan> span,
                                  const Template& body) -> Diagnostic
{
    return Diagnostic{.code = code,
                      .severity = severity,
                      .message = std::move(message),
                      .help = std::move(help),
                      .primary = span,
                      .labels = {},
                      .source = body.Source()};
}

// Rust's `get_suggested_entity_type`: the declared entity type nearest to `unknown` by its written name.
[[nodiscard]] auto SuggestedEntityTypeHelp(const ValidatorSchema& schema, const EntityType& unknown)
    -> std::optional<std::string>
{
    std::vector<std::string> known;
    known.reserve(schema.EntityTypes().size());
    for (const ValidatorEntityType& type : schema.EntityTypes()) { known.push_back(type.name.ToString()); }
    const std::optional<std::string> nearest = FuzzySearch(unknown.ToString(), known);
    if (!nearest) { return std::nullopt; }
    return std::format("did you mean `{}`?", *nearest);
}

// Rust's `unrecognized_action_id_help`: an action whose id contains `Action::<id>` or
// `Action::"<id>"` suggests that the type was written into the id; otherwise the declared action
// nearest to the id (its bare text compared with each action's written `Action::"id"`) is suggested.
[[nodiscard]] auto UnrecognizedActionHelp(const ValidatorSchema& schema, const EntityUid& uid)
    -> std::optional<std::string>
{
    const std::string withType = std::format("Action::{}", uid.Id().Text());
    const std::string withQuotedType = std::format("Action::\"{}\"", uid.Id().Text());
    for (const ValidatorAction& action : schema.Actions())
    {
        const std::string_view id = action.name.Id().Text();
        if (id.contains(withType) || id.contains(withQuotedType))
        {
            return std::format("did you intend to include the type in action `{}`?", action.name.ToString());
        }
    }
    std::vector<std::string> known;
    known.reserve(schema.Actions().size());
    for (const ValidatorAction& action : schema.Actions()) { known.push_back(action.name.ToString()); }
    const std::optional<std::string> nearest = FuzzySearch(uid.Id().Text(), known);
    if (!nearest) { return std::nullopt; }
    return std::format("did you mean `{}`?", *nearest);
}

[[nodiscard]] auto EnumChoicesHelp(const ValidatorEntityType& type) -> std::string
{
    std::string help = "valid entity ids: ";
    for (std::size_t position = 0; position < type.enumChoices.size(); ++position)
    {
        if (position > 0) { help += ", "; }
        help += std::format("\"{}\"", type.enumChoices[position].Escaped());
    }
    return help;
}

// ---- Action application ----

using TypeSet = std::vector<EntityType>;

auto AddUnique(TypeSet& types, const EntityType& type) -> void
{
    if (std::ranges::find(types, type) == types.end()) { types.push_back(type); }
}

// Rust's `get_entity_types_in`: the declared descendants of `type`, and `type` itself.
[[nodiscard]] auto EntityTypesIn(const ValidatorSchema& schema, const EntityType& type) -> TypeSet
{
    TypeSet types;
    if (const ValidatorEntityType* declared = schema.FindEntityType(type)) { types = declared->descendants; }
    AddUnique(types, type);
    return types;
}

[[nodiscard]] auto AllEntityTypes(const ValidatorSchema& schema) -> TypeSet
{
    TypeSet types;
    types.reserve(schema.EntityTypes().size());
    for (const ValidatorEntityType& type : schema.EntityTypes()) { types.push_back(type.name); }
    return types;
}

// Rust's `get_entity_types_satisfying_constraint`.
[[nodiscard]] auto TypesSatisfying(const ValidatorSchema& schema, const PrincipalOrResourceConstraint& constraint)
    -> TypeSet
{
    return std::visit(Overloaded{
                          [&](const ScopeAny&) { return AllEntityTypes(schema); },
                          [&](const ScopeEqual& equal) {
                              const auto* uid = std::get_if<EntityUid>(&equal.target);
                              return uid != nullptr ? TypeSet{uid->Type()} : AllEntityTypes(schema);
                          },
                          [&](const ScopeIn& in) {
                              const auto* uid = std::get_if<EntityUid>(&in.target);
                              return uid != nullptr ? EntityTypesIn(schema, uid->Type()) : AllEntityTypes(schema);
                          },
                          [&](const ScopeIs& is) {
                              return schema.IsKnownEntityType(is.type) ? TypeSet{is.type} : TypeSet{};
                          },
                          [&](const ScopeIsIn& isIn) {
                              const auto* uid = std::get_if<EntityUid>(&isIn.target);
                              if (uid == nullptr)
                              {
                                  return schema.IsKnownEntityType(isIn.type) ? TypeSet{isIn.type} : TypeSet{};
                              }
                              const TypeSet in = EntityTypesIn(schema, uid->Type());
                              return std::ranges::find(in, isIn.type) != in.end() ? TypeSet{isIn.type} : TypeSet{};
                          },
                      },
                      constraint);
}

// The actions the constraint admits that are not external (Rust's `get_apply_specs_for_action`):
// every action; the one action; or each listed action and its descendants, none when one of the
// listed actions is not declared.
[[nodiscard]] auto ActionsSatisfying(const ValidatorSchema& schema, const ActionConstraint& constraint)
    -> std::vector<const ValidatorAction*>
{
    std::vector<const ValidatorAction*> actions;
    const auto addDeclared = [&](const EntityUid& uid) {
        const ValidatorAction* action = schema.FindAction(uid);
        if (action != nullptr && !action->external) { actions.push_back(action); }
    };
    std::visit(Overloaded{
                   [&](const ActionAny&) {
                       for (const ValidatorAction& action : schema.Actions())
                       {
                           if (!action.external) { actions.push_back(&action); }
                       }
                   },
                   [&](const ActionEqual& equal) { addDeclared(equal.action); },
                   [&](const ActionIn& in) {
                       std::vector<EntityUid> admitted;
                       for (const EntityUid& uid : in.actions)
                       {
                           const ValidatorAction* action = schema.FindAction(uid);
                           if (action == nullptr) { return; }
                           admitted.insert(admitted.end(), action->descendants.begin(), action->descendants.end());
                           admitted.push_back(action->name);
                       }
                       for (const EntityUid& uid : admitted) { addDeclared(uid); }
                   },
               },
               constraint);
    return actions;
}

[[nodiscard]] auto Intersects(const TypeSet& admitted, const std::span<const EntityType> allowed) -> bool
{
    return std::ranges::any_of(allowed, [&](const EntityType& type) {
        return std::ranges::find(admitted, type) != admitted.end();
    });
}

// Rust's `check_if_in_fixes`: the constraint is `== E`, no admitted action applies to E's type,
// and some admitted action applies to a type E's type may descend from.
[[nodiscard]] auto InWouldFix(const ValidatorSchema& schema, const PrincipalOrResourceConstraint& constraint,
                              const std::span<const ValidatorAction* const> actions, const bool principal) -> bool
{
    const auto* equal = std::get_if<ScopeEqual>(&constraint);
    const auto* uid = equal != nullptr ? std::get_if<EntityUid>(&equal->target) : nullptr;
    if (uid == nullptr) { return false; }
    const auto allowed = [&](const ValidatorAction* action) -> std::span<const EntityType> {
        return principal ? action->principalTypes : action->resourceTypes;
    };
    const bool noneEqual = std::ranges::none_of(actions, [&](const ValidatorAction* action) {
        return std::ranges::find(allowed(action), uid->Type()) != allowed(action).end();
    });
    const ValidatorEntityType* declared = schema.FindEntityType(uid->Type());
    if (!noneEqual || declared == nullptr) { return false; }
    return std::ranges::any_of(actions, [&](const ValidatorAction* action) {
        return std::ranges::any_of(allowed(action),
                                   [&](const EntityType& type) { return declared->HasDescendant(type); });
    });
}

} // namespace

auto ScopeAndLiteralErrors(const ValidatorSchema& schema, const Template& body) -> std::vector<Diagnostic>
{
    const PolicyNames names = CollectNames(body);
    std::vector<Diagnostic> errors;
    for (const EntityTypeSite& site : names.types)
    {
        if (schema.IsKnownEntityType(site.type)) { continue; }
        errors.push_back(MakeDiagnostic(DiagnosticCode::UnrecognizedEntityType, Severity::Error,
                                        std::format("unrecognized entity type `{}`", site.type.ToString()),
                                        SuggestedEntityTypeHelp(schema, site.type), site.span, body));
    }
    for (const EntityLiteralSite& site : names.literals)
    {
        if (site.uid.IsAction()) { continue; }
        const ValidatorEntityType* type = schema.FindEntityType(site.uid.Type());
        if (type == nullptr || type->kind != ValidatorEntityTypeKind::Enumerated || type->IsEnumChoice(site.uid.Id()))
        {
            continue;
        }
        errors.push_back(MakeDiagnostic(
            DiagnosticCode::ValidationInvalidEnumEntity, Severity::Error,
            std::format("entity `{}` is of an enumerated entity type, but `\"{}\"` is not one of its ids",
                        site.uid.ToString(), site.uid.Id().Escaped()),
            EnumChoicesHelp(*type), site.span, body));
    }
    for (const EntityLiteralSite& site : names.literals)
    {
        if (!site.uid.IsAction() || schema.FindAction(site.uid) != nullptr) { continue; }
        errors.push_back(MakeDiagnostic(DiagnosticCode::UnrecognizedActionId, Severity::Error,
                                        std::format("unrecognized action `{}`", site.uid.ToString()),
                                        UnrecognizedActionHelp(schema, site.uid), site.span, body));
    }
    return errors;
}

auto ActionApplicationWarning(const ValidatorSchema& schema, const PrincipalConstraint& principal,
                              const ActionConstraint& action, const ResourceConstraint& resource,
                              const Template& body) -> std::optional<Diagnostic>
{
    const std::vector<const ValidatorAction*> actions = ActionsSatisfying(schema, action);
    const TypeSet principals = TypesSatisfying(schema, principal.constraint);
    const TypeSet resources = TypesSatisfying(schema, resource.constraint);
    const bool applies = std::ranges::any_of(actions, [&](const ValidatorAction* candidate) {
        return Intersects(principals, candidate->principalTypes) && Intersects(resources, candidate->resourceTypes);
    });
    if (applies) { return std::nullopt; }

    const bool fixPrincipal = InWouldFix(schema, principal.constraint, actions, true);
    const bool fixResource = InWouldFix(schema, resource.constraint, actions, false);
    std::optional<std::string> help;
    if (fixPrincipal && fixResource)
    {
        help = "try replacing `==` with `in` in the principal clause and the resource clause";
    }
    else if (fixPrincipal) { help = "try replacing `==` with `in` in the principal clause"; }
    else if (fixResource) { help = "try replacing `==` with `in` in the resource clause"; }
    return MakeDiagnostic(DiagnosticCode::InvalidActionApplication, Severity::Warning,
                          "unable to find an applicable action given the policy scope constraints", std::move(help),
                          body.SourceExtent(), body);
}

auto SlotEntityTypeErrors(const ValidatorSchema& schema, const Policy& policy) -> std::vector<Diagnostic>
{
    std::vector<Diagnostic> errors;
    for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
    {
        const EntityUid* bound = policy.Binding(slot);
        if (bound == nullptr || schema.IsKnownEntityType(bound->Type())) { continue; }
        errors.push_back(Diagnostic{
            .code = DiagnosticCode::UnrecognizedEntityType,
            .severity = Severity::Error,
            .message = std::format("unrecognized entity type `{}` of the entity `{}` linked to {}",
                                   bound->Type().ToString(), bound->ToString(), ToString(slot)),
            .help = SuggestedEntityTypeHelp(schema, bound->Type()),
        });
    }
    return errors;
}

auto IncompleteSchemaError(const ValidatorSchema& schema) -> std::optional<Diagnostic>
{
    if (schema.IsComplete()) { return std::nullopt; }
    std::vector<std::string> names;
    for (const ValidatorEntityType& type : schema.EntityTypes())
    {
        if (type.kind == ValidatorEntityTypeKind::External) { names.push_back(std::format("`{}`", type.name.ToString())); }
    }
    for (const ValidatorAction& action : schema.Actions())
    {
        if (action.external) { names.push_back(std::format("`{}`", action.name.ToString())); }
    }
    std::string list;
    for (std::size_t position = 0; position < names.size(); ++position)
    {
        if (position > 0) { list += position + 1 == names.size() ? (names.size() > 2 ? ", and " : " and ") : ", "; }
        list += names[position];
    }
    return Diagnostic{
        .code = DiagnosticCode::IncompleteSchema,
        .severity = Severity::Error,
        .message = std::format("schema is incomplete: {} {} declared `external` but not defined", list,
                               names.size() == 1 ? "is" : "are"),
    };
}

auto IncompleteSchemaError(const ValidatorSchema& schema, const Template& body) -> std::optional<Diagnostic>
{
    std::optional<Diagnostic> error = IncompleteSchemaError(schema);
    if (error)
    {
        error->primary = body.SourceExtent();
        error->source = body.Source();
    }
    return error;
}

} // namespace cedar::detail
