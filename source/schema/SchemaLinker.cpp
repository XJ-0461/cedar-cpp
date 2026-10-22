// Linking schema fragments into a ValidatorSchema (Rust's `ValidatorSchema::from_schema_fragments`,
// `cedar-policy-core/src/validator/schema.rs`, with `schema/namespace_def.rs`, `schema/raw_name.rs`
// and the reference resolution in `json_schema.rs`).
//
// Rust converts each fragment through three representations (`RawName`, `ConditionalName`,
// `InternalName`). Here the model is read in place: a reference is resolved, from its written name
// and the namespace it appears in, whenever it is needed, against the set of every declared name.

#include "schema/SchemaLinker.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "model/FuzzyMatch.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Expression.hpp>
#include <cedar/Names.hpp>
#include <cedar/Schema.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

namespace detail
{

namespace
{

using Namespace = std::optional<Name>;

/// What a written type name may resolve to (Rust's `ReferenceType`).
enum class ReferenceKind : std::uint8_t
{
    Common,
    Entity,
    CommonOrEntity,
};

[[nodiscard]] auto ReferenceKindText(const ReferenceKind kind) -> std::string_view
{
    switch (kind)
    {
    case ReferenceKind::Common: return "a common type";
    case ReferenceKind::Entity: return "an entity type";
    case ReferenceKind::CommonOrEntity: return "a common or entity type";
    }
    return {};
}

[[nodiscard]] auto Qualify(const Name& name, const Namespace& where) -> Name
{
    return where ? name.QualifyWith(*where) : name;
}

[[nodiscard]] auto NamespaceText(const Namespace& where) -> std::string
{
    return where ? std::format("namespace `{}`", where->QualifiedText()) : std::string{"the empty namespace"};
}

[[nodiscard]] auto MakeIdentifier(const std::string_view text) -> Identifier { return *Identifier::Parse(text); }

/// `__cedar::<basename>`.
[[nodiscard]] auto BuiltinName(const std::string_view basename) -> Name
{
    return *Name::ParseInternal(std::format("{}::{}", ReservedNamespaceName, basename));
}

/// Where a declaration or reference was written: its namespace and the text it was read from.
struct Origin
{
    Namespace where;
    const SharedSourceText* source;
};

struct CommonDeclarationEntry
{
    Name name;
    const SchemaType* type;
    Origin origin;
    std::optional<SourceSpan> span;
};

struct EntityDeclarationEntry
{
    EntityType name;
    const EntityTypeDeclaration* declaration;
    Origin origin;
};

struct ActionDeclarationEntry
{
    EntityUid name;
    const ActionDeclaration* declaration;
    Origin origin;
};

const SharedSourceText NoSource{};

class Linker
{
public:
    explicit Linker(const std::span<const SchemaFragmentModel* const> fragments)
    {
        for (const SchemaFragmentModel* fragment : fragments)
        {
            for (const NamespaceDefinition& definition : fragment->namespaces)
            {
                Gather(definition, Origin{definition.name, &fragment->source});
            }
        }
        GatherBuiltins();
    }

    auto Link() -> std::expected<std::shared_ptr<const ValidatorSchema>, SchemaError>
    {
        CheckShadowing();
        if (!diagnostics_.empty()) { return Fail(); }
        AddAliases();
        for (const ActionDeclarationEntry& action : actions_) { entityNames_.insert(action.name.Type().GetName()); }

        CheckReferences();
        if (!diagnostics_.empty()) { return Fail(); }
        CheckDuplicates();
        if (!diagnostics_.empty()) { return Fail(); }

        for (const CommonDeclarationEntry& common : commons_)
        {
            if (auto resolved = ResolveCommon(common.name, common.span, *common.origin.source); !resolved)
            {
                diagnostics_.push_back(std::move(resolved).error());
                return Fail();
            }
        }

        std::vector<ValidatorEntityType> entityTypes = MakeEntityTypes();
        if (!diagnostics_.empty()) { return Fail(); }
        std::vector<ValidatorAction> actions = MakeActions();
        if (!diagnostics_.empty()) { return Fail(); }
        CloseEntityTypes(entityTypes);
        if (!diagnostics_.empty()) { return Fail(); }
        CloseActions(actions);
        if (!diagnostics_.empty()) { return Fail(); }
        CheckUndeclaredParents(entityTypes);
        if (!diagnostics_.empty()) { return Fail(); }
        return std::make_shared<const ValidatorSchema>(std::move(entityTypes), std::move(actions));
    }

private:
    // ---- Declarations ----

    void Gather(const NamespaceDefinition& definition, const Origin origin)
    {
        for (const CommonTypeDeclaration& common : definition.commonTypes)
        {
            const Name name = Qualify(Name::Unqualified(common.name), origin.where);
            commons_.push_back(CommonDeclarationEntry{name, &common.type, origin, common.span.value});
            commonNames_.insert(name);
        }
        for (const EntityTypeDeclaration& entity : definition.entityTypes)
        {
            const Name name = Qualify(Name::Unqualified(entity.name), origin.where);
            entities_.push_back(EntityDeclarationEntry{EntityType{name}, &entity, origin});
            entityNames_.insert(name);
        }
        const EntityType actionType{Qualify(Name::Unqualified(MakeIdentifier("Action")), origin.where)};
        for (const ActionDeclaration& action : definition.actions)
        {
            const EntityUid name{actionType, Eid{action.name}};
            actions_.push_back(ActionDeclarationEntry{name, &action, origin});
            actionNames_.insert(name);
        }
    }

    /// The `__cedar` namespace (Rust's `cedar_fragment`): `Bool`, `Long`, `String` and each
    /// extension type, as common types.
    void GatherBuiltins()
    {
        const Name reserved = *Name::ParseInternal(ReservedNamespaceName);
        const auto add = [&](const std::string_view basename, SchemaType::Kind kind)
        {
            const Name name = BuiltinName(basename);
            const SchemaType& type = builtinTypes_.emplace_back(SchemaType{std::move(kind)});
            commons_.push_back(CommonDeclarationEntry{name, &type, Origin{reserved, &NoSource}, std::nullopt});
            commonNames_.insert(name);
            builtinBasenames_.push_back(basename);
        };
        add("Bool", schema_type::Boolean{});
        add("Long", schema_type::Long{});
        add("String", schema_type::String{});
        for (const ValueKind kind : ExtensionTypes)
        {
            add(ExtensionTypeName(kind), schema_type::Extension{MakeIdentifier(ExtensionTypeName(kind))});
        }
    }

    /// RFC 70 (`AllDefs::rfc_70_shadowing_checks`): an unqualified entity or common type name, or
    /// action id, may not also be declared in a namespace. `__cedar` names do not count.
    void CheckShadowing()
    {
        std::unordered_map<Symbol, Name> namespaced;
        const auto collect = [&](const Name& name)
        {
            if (!name.IsUnqualified() && !name.IsReserved()) { namespaced.emplace(name.Basename().GetSymbol(), name); }
        };
        for (const Name& name : entityNames_) { collect(name); }
        for (const Name& name : commonNames_) { collect(name); }
        const auto check = [&](const Name& name)
        {
            if (!name.IsUnqualified()) { return; }
            const auto found = namespaced.find(name.Basename().GetSymbol());
            if (found == namespaced.end()) { return; }
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::TypeShadowing,
                .message = std::format("definition of `{}` illegally shadows the existing definition of `{}`",
                                       found->second.QualifiedText(), name.QualifiedText()),
                .help = "try renaming one of the definitions, or moving `" + name.ToString() +
                        "` to a different namespace",
            });
        };
        for (const Name& name : entityNames_) { check(name); }
        for (const Name& name : commonNames_) { check(name); }
        if (!diagnostics_.empty()) { return; }

        std::unordered_map<std::string_view, const EntityUid*> namespacedActions;
        for (const EntityUid& action : actionNames_)
        {
            if (!action.Type().GetName().IsUnqualified()) { namespacedActions.emplace(action.Id().Text(), &action); }
        }
        for (const EntityUid& action : actionNames_)
        {
            if (!action.Type().GetName().IsUnqualified()) { continue; }
            const auto found = namespacedActions.find(action.Id().Text());
            if (found == namespacedActions.end()) { continue; }
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::ActionShadowing,
                .message = std::format("definition of `{}` illegally shadows the existing definition of `{}`",
                                       found->second->ToString(), action.ToString()),
                .help = "try renaming one of the actions, or moving `" + action.ToString() + "` to a different namespace",
            });
        }
    }

    /// The built-in type names in the empty namespace, each an alias of its `__cedar` definition,
    /// except where the empty namespace declares that name itself.
    void AddAliases()
    {
        for (const std::string_view basename : builtinBasenames_)
        {
            const Name alias = Name::Unqualified(MakeIdentifier(basename));
            if (entityNames_.contains(alias) || commonNames_.contains(alias)) { continue; }
            const SchemaType& type =
                builtinTypes_.emplace_back(SchemaType{schema_type::EntityOrCommon{TypeName{BuiltinName(basename)}}});
            commons_.push_back(CommonDeclarationEntry{alias, &type, Origin{std::nullopt, &NoSource}, std::nullopt});
            commonNames_.insert(alias);
        }
    }

    // ---- Resolution ----

    [[nodiscard]] auto IsDefined(const Name& name, const ReferenceKind kind) const -> bool
    {
        return (kind != ReferenceKind::Entity && commonNames_.contains(name)) ||
               (kind != ReferenceKind::Common && entityNames_.contains(name));
    }

    /// The declaration `written` names in namespace `where` (Rust's `ConditionalName::resolve`):
    /// an unqualified name is tried in `where` first, then in the empty namespace; a qualified name
    /// means itself.
    [[nodiscard]] auto Resolve(const Name& written, const Namespace& where, const ReferenceKind kind) const
        -> std::optional<Name>
    {
        if (written.IsUnqualified() && where)
        {
            const Name qualified = written.QualifyWith(*where);
            if (IsDefined(qualified, kind)) { return qualified; }
        }
        if (IsDefined(written, kind)) { return written; }
        return std::nullopt;
    }

    [[nodiscard]] auto NotDefined(const TypeName& written, const Origin& origin, const ReferenceKind kind) const
        -> Diagnostic
    {
        std::string help;
        if (written.name.IsUnqualified() && origin.where)
        {
            help = std::format("neither `{}` nor `{}` refers to anything that has been declared as {}",
                               written.name.QualifyWith(*origin.where).QualifiedText(), written.name.QualifiedText(),
                               ReferenceKindText(kind));
        }
        else
        {
            help = std::format("`{}` has not been declared as {}", written.name.QualifiedText(), ReferenceKindText(kind));
        }
        return Diagnostic{
            .code = DiagnosticCode::TypeNotDefined,
            .message = std::format("failed to resolve type: {}", written.name.QualifiedText()),
            .help = std::move(help),
            .primary = written.span.value,
            .source = *origin.source,
        };
    }

    /// The action `reference` names in namespace `where`: its type (`Action` when not written)
    /// resolves as an entity type name would, and the action must be declared.
    [[nodiscard]] auto ResolveAction(const ActionReference& reference, const Namespace& where) const
        -> std::optional<EntityUid>
    {
        const Name written = reference.type ? reference.type->name : Name::Unqualified(MakeIdentifier("Action"));
        const auto declared = [&](const Name& type) -> std::optional<EntityUid>
        {
            // A `__cedar` name is no entity type, so no action has it.
            if (type.IsReserved()) { return std::nullopt; }
            EntityUid action{EntityType{type}, Eid{reference.id}};
            if (!actionNames_.contains(action)) { return std::nullopt; }
            return action;
        };
        if (written.IsUnqualified() && where)
        {
            if (auto action = declared(written.QualifyWith(*where))) { return action; }
        }
        return declared(written);
    }

    void CheckReference(const TypeName& written, const Origin& origin, const ReferenceKind kind)
    {
        if (!Resolve(written.name, origin.where, kind)) { diagnostics_.push_back(NotDefined(written, origin, kind)); }
    }

    void CheckType(const SchemaType& type, const Origin& origin)
    {
        std::visit(Overloaded{
                       [](const schema_type::String&) {},
                       [](const schema_type::Long&) {},
                       [](const schema_type::Boolean&) {},
                       [](const schema_type::Extension&) {},
                       [&](const schema_type::Set& set) { CheckType(*set.element, origin); },
                       [&](const schema_type::Record& record)
                       {
                           for (const RecordAttribute& attribute : record.attributes) { CheckType(*attribute.type, origin); }
                       },
                       [&](const schema_type::Entity& entity) { CheckReference(entity.name, origin, ReferenceKind::Entity); },
                       [&](const schema_type::EntityOrCommon& name)
                       { CheckReference(name.name, origin, ReferenceKind::CommonOrEntity); },
                       [&](const schema_type::CommonTypeReference& name)
                       { CheckReference(name.name, origin, ReferenceKind::Common); },
                   },
                   type.kind);
    }

    /// Every reference resolves (Rust's `fully_qualify_type_references`).
    void CheckReferences()
    {
        for (const CommonDeclarationEntry& common : commons_) { CheckType(*common.type, common.origin); }
        for (const EntityDeclarationEntry& entity : entities_)
        {
            const auto* standard = std::get_if<StandardEntityType>(&entity.declaration->kind);
            if (standard == nullptr) { continue; }
            for (const TypeName& parent : standard->memberOfTypes)
            {
                CheckReference(parent, entity.origin, ReferenceKind::Entity);
            }
            if (entity.declaration->external) { continue; }
            CheckType(standard->shape, entity.origin);
            if (standard->tags) { CheckType(*standard->tags, entity.origin); }
        }
        for (const ActionDeclarationEntry& action : actions_)
        {
            const ActionDeclaration& declaration = *action.declaration;
            if (!declaration.external && declaration.appliesTo)
            {
                CheckType(declaration.appliesTo->context, action.origin);
                for (const TypeName& type : declaration.appliesTo->principalTypes)
                {
                    CheckReference(type, action.origin, ReferenceKind::Entity);
                }
                for (const TypeName& type : declaration.appliesTo->resourceTypes)
                {
                    CheckReference(type, action.origin, ReferenceKind::Entity);
                }
            }
            if (!declaration.memberOf) { continue; }
            for (const ActionReference& parent : *declaration.memberOf)
            {
                if (ResolveAction(parent, action.origin.where)) { continue; }
                const std::string written = std::format(
                    "{}::\"{}\"", parent.type ? parent.type->name.QualifiedText() : std::string_view{"Action"},
                    Eid{parent.id}.Escaped());
                diagnostics_.push_back(Diagnostic{
                    .code = DiagnosticCode::ActionNotDefined,
                    .message = std::format("undeclared action: {}", written),
                    .help = std::format("any actions appearing as parents need to be declared as actions (in {})",
                                        NamespaceText(action.origin.where)),
                    .primary = parent.span.value,
                    .source = *action.origin.source,
                });
            }
        }
    }

    /// No name declared twice across fragments (a fragment itself never repeats one).
    void CheckDuplicates()
    {
        std::unordered_set<Name> seenCommons;
        for (const CommonDeclarationEntry& common : commons_)
        {
            if (seenCommons.insert(common.name).second) { continue; }
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::DuplicateCommonType,
                .message = std::format("duplicate common type `{}`", common.name.QualifiedText()),
                .primary = common.span,
                .source = *common.origin.source,
            });
        }
        std::unordered_set<EntityType> seenEntities;
        for (const EntityDeclarationEntry& entity : entities_)
        {
            if (seenEntities.insert(entity.name).second) { continue; }
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::DuplicateEntityType,
                .message = std::format("duplicate entity type `{}`", entity.name.GetName().QualifiedText()),
                .primary = entity.declaration->span.value,
                .source = *entity.origin.source,
            });
        }
        std::unordered_set<EntityUid> seenActions;
        for (const ActionDeclarationEntry& action : actions_)
        {
            if (seenActions.insert(action.name).second) { continue; }
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::DuplicateAction,
                .message = std::format("duplicate action `{}`", action.name.ToString()),
                .primary = action.declaration->span.value,
                .source = *action.origin.source,
            });
        }
    }

    // ---- Types ----

    /// The common type `name`, inlined (Rust's `CommonTypeResolver`). A type that reaches itself
    /// is a cycle.
    auto ResolveCommon(const Name& name, const std::optional<SourceSpan> span, const SharedSourceText& source)
        -> std::expected<SharedType, Diagnostic>
    {
        auto& state = commonStates_[name];
        if (state.type) { return state.type; }
        if (state.resolving)
        {
            return std::unexpected{Diagnostic{
                .code = DiagnosticCode::CycleInCommonTypeReferences,
                .message = std::format("a cycle was detected in the common type definitions: `{}` refers to itself",
                                       name.QualifiedText()),
                .primary = span,
                .source = source,
            }};
        }
        const auto declaration = std::ranges::find(commons_, name, &CommonDeclarationEntry::name);
        state.resolving = true;
        auto type = ConvertType(*declaration->type, declaration->origin);
        if (!type) { return type; }
        state = CommonState{.type = *type, .resolving = false};
        return type;
    }

    /// `type` as a validator type (Rust's `try_jsonschema_type_into_validator_type`), with its
    /// references resolved in `origin`'s namespace. Every reference is known to resolve.
    auto ConvertType(const SchemaType& type, const Origin& origin) -> std::expected<SharedType, Diagnostic>
    {
        using Result = std::expected<SharedType, Diagnostic>;
        const auto entity = [](const Name& name) -> Result
        { return MakeShared(Type{types::Entity{EntityLub{EntityType{name}}}}); };
        return std::visit(
            Overloaded{
                [&](const schema_type::String&) -> Result { return StringType(); },
                [&](const schema_type::Long&) -> Result { return LongType(); },
                [&](const schema_type::Boolean&) -> Result { return BooleanType(); },
                [&](const schema_type::Set& set) -> Result
                {
                    auto element = ConvertType(*set.element, origin);
                    if (!element) { return element; }
                    return MakeShared(Type{types::Set{*std::move(element)}});
                },
                [&](const schema_type::Record& record) -> Result
                {
                    if (record.additionalAttributes)
                    {
                        return std::unexpected{Diagnostic{
                            .code = DiagnosticCode::UnsupportedSchemaFeature,
                            .message = "records and entities with `additionalAttributes` are experimental, but the "
                                       "experimental `partial-validate` feature is not enabled",
                            .primary = type.span.value,
                            .source = *origin.source,
                        }};
                    }
                    std::vector<Attribute> attributes;
                    attributes.reserve(record.attributes.size());
                    for (const RecordAttribute& attribute : record.attributes)
                    {
                        auto attributeType = ConvertType(*attribute.type, origin);
                        if (!attributeType) { return attributeType; }
                        attributes.push_back(Attribute{attribute.name, AttributeType{*std::move(attributeType),
                                                                                     attribute.required}});
                    }
                    return MakeShared(Type{types::Record{Attributes{std::move(attributes)}}});
                },
                [&](const schema_type::Entity& reference) -> Result
                { return entity(*Resolve(reference.name.name, origin.where, ReferenceKind::Entity)); },
                [&](const schema_type::EntityOrCommon& reference) -> Result
                {
                    const Name name = *Resolve(reference.name.name, origin.where, ReferenceKind::CommonOrEntity);
                    // A common type wins over an entity type of the same name (RFC 24).
                    if (commonNames_.contains(name))
                    {
                        return ResolveCommon(name, reference.name.span.value, *origin.source);
                    }
                    return entity(name);
                },
                [&](const schema_type::CommonTypeReference& reference) -> Result
                {
                    return ResolveCommon(*Resolve(reference.name.name, origin.where, ReferenceKind::Common),
                                         reference.name.span.value, *origin.source);
                },
                [&](const schema_type::Extension& extension) -> Result
                {
                    const std::optional<ValueKind> kind = FindExtensionType(extension.name.Text());
                    if (kind) { return MakeShared(Type{types::Extension{*kind}}); }
                    std::vector<std::string_view> names;
                    for (const ValueKind each : ExtensionTypes) { names.push_back(ExtensionTypeName(each)); }
                    std::optional<std::string> nearest = FuzzySearch(extension.name.Text(), names);
                    return std::unexpected{Diagnostic{
                        .code = DiagnosticCode::UnknownExtensionType,
                        .message = std::format("unknown extension type `{}`", extension.name.Text()),
                        .help = nearest ? std::optional{std::format("did you mean `{}`?", *nearest)} : std::nullopt,
                        .primary = type.span.value,
                        .source = *origin.source,
                    }};
                },
            },
            type.kind);
    }

    /// `type` converted, which must be a record: an entity type's shape or an action's context.
    auto ConvertRecord(const SchemaType& type, const Origin& origin, const std::string_view what)
        -> std::optional<types::Record>
    {
        auto converted = ConvertType(type, origin);
        if (!converted)
        {
            diagnostics_.push_back(std::move(converted).error());
            return std::nullopt;
        }
        const types::Record* record = AsRecord(**converted);
        if (record == nullptr)
        {
            diagnostics_.push_back(Diagnostic{
                .code = DiagnosticCode::ContextOrShapeNotRecord,
                .message = std::format("{} is declared with a type other than `Record`", what),
                .help = "the type is " + ToString(**converted),
                .primary = type.span.value,
                .source = *origin.source,
            });
            return std::nullopt;
        }
        return *record;
    }

    // ---- Entity types and actions ----

    auto MakeEntityTypes() -> std::vector<ValidatorEntityType>
    {
        std::vector<ValidatorEntityType> entityTypes;
        entityTypes.reserve(entities_.size());
        for (const EntityDeclarationEntry& entity : entities_)
        {
            const EntityTypeDeclaration& declaration = *entity.declaration;
            ValidatorEntityType& made = entityTypes.emplace_back(ValidatorEntityType{
                .name = entity.name,
                .span = declaration.span.value,
            });
            if (const auto* enumerated = std::get_if<EnumEntityType>(&declaration.kind))
            {
                made.kind = ValidatorEntityTypeKind::Enumerated;
                made.enumChoices = enumerated->choices;
                continue;
            }
            const auto& standard = std::get<StandardEntityType>(declaration.kind);
            for (const TypeName& parent : standard.memberOfTypes)
            {
                const Name parentName = *Resolve(parent.name, entity.origin.where, ReferenceKind::Entity);
                children_[parentName].push_back(entity.name);
            }
            if (declaration.external)
            {
                made.kind = ValidatorEntityTypeKind::External;
                continue;
            }
            auto shape = ConvertRecord(standard.shape, entity.origin,
                                       std::format("the shape of entity type `{}`", entity.name.GetName().QualifiedText()));
            if (!shape) { continue; }
            made.attributes = std::move(shape->attributes);
            made.openAttributes = shape->open;
            if (standard.tags)
            {
                auto tags = ConvertType(*standard.tags, entity.origin);
                if (!tags)
                {
                    diagnostics_.push_back(std::move(tags).error());
                    continue;
                }
                made.tags = **tags;
            }
        }
        return entityTypes;
    }

    auto MakeActions() -> std::vector<ValidatorAction>
    {
        std::vector<ValidatorAction> actions;
        actions.reserve(actions_.size());
        for (const ActionDeclarationEntry& action : actions_)
        {
            const ActionDeclaration& declaration = *action.declaration;
            ValidatorAction& made = actions.emplace_back(ValidatorAction{
                .name = action.name,
                .external = declaration.external,
                .span = declaration.span.value,
            });
            if (declaration.memberOf)
            {
                for (const ActionReference& parent : *declaration.memberOf)
                {
                    actionChildren_[*ResolveAction(parent, action.origin.where)].push_back(action.name);
                }
            }
            if (declaration.external) { continue; }
            const AppliesTo noAppliesTo{};
            const AppliesTo& appliesTo = declaration.appliesTo ? *declaration.appliesTo : noAppliesTo;
            const auto resolveTypes = [&](const std::vector<TypeName>& written)
            {
                std::vector<EntityType> resolved;
                resolved.reserve(written.size());
                for (const TypeName& type : written)
                {
                    resolved.emplace_back(*Resolve(type.name, action.origin.where, ReferenceKind::Entity));
                    appliesToTypes_.push_back(TypeUse{resolved.back(), type.span.value, action.origin.source});
                }
                std::ranges::sort(resolved);
                const auto [first, last] = std::ranges::unique(resolved);
                resolved.erase(first, last);
                return resolved;
            };
            made.principalTypes = resolveTypes(appliesTo.principalTypes);
            made.resourceTypes = resolveTypes(appliesTo.resourceTypes);
            auto context = ConvertRecord(appliesTo.context, action.origin,
                                         std::format("the context of action `{}`", action.name.ToString()));
            if (context) { made.context = Type{*std::move(context)}; }
        }
        return actions;
    }

    /// Closes the entity type hierarchy (Rust's `compute_tc` without the acyclicity check) and
    /// refuses an external type below an enumerated one.
    void CloseEntityTypes(std::vector<ValidatorEntityType>& entityTypes)
    {
        std::unordered_map<EntityType, std::size_t> index;
        for (std::size_t position = 0; position < entityTypes.size(); ++position)
        {
            index.emplace(entityTypes[position].name, position);
        }
        for (ValidatorEntityType& entityType : entityTypes)
        {
            entityType.descendants = Reachable(entityType.name.GetName(), children_,
                                               [](const EntityType& type) { return type.GetName(); });
        }
        for (const ValidatorEntityType& ancestor : entityTypes)
        {
            for (const EntityType& descendant : ancestor.descendants)
            {
                entityTypes[index.at(descendant)].ancestors.push_back(ancestor.name);
            }
        }
        for (ValidatorEntityType& entityType : entityTypes) { std::ranges::sort(entityType.ancestors); }

        for (const ValidatorEntityType& ancestor : entityTypes)
        {
            if (ancestor.kind != ValidatorEntityTypeKind::Enumerated) { continue; }
            for (const EntityType& descendant : ancestor.descendants)
            {
                const ValidatorEntityType& below = entityTypes[index.at(descendant)];
                if (below.kind != ValidatorEntityTypeKind::External) { continue; }
                diagnostics_.push_back(Diagnostic{
                    .code = DiagnosticCode::ExternalWithEnumAncestor,
                    .message = std::format("external entity type `{}` has the enumerated entity type `{}` as an ancestor",
                                           descendant.GetName().QualifiedText(), ancestor.name.GetName().QualifiedText()),
                    .help = "an enumerated entity type lists every entity of the types below it, which an external "
                            "declaration cannot",
                    .primary = below.span,
                });
            }
        }
    }

    /// Closes the action hierarchy, which must have no cycle.
    void CloseActions(std::vector<ValidatorAction>& actions)
    {
        std::unordered_map<EntityUid, std::size_t> index;
        for (std::size_t position = 0; position < actions.size(); ++position)
        {
            index.emplace(actions[position].name, position);
        }
        for (ValidatorAction& action : actions)
        {
            action.descendants = Reachable(action.name, actionChildren_, [](const EntityUid& uid) { return uid; });
            if (action.HasDescendant(action.name))
            {
                diagnostics_.push_back(Diagnostic{
                    .code = DiagnosticCode::CycleInActionHierarchy,
                    .message = std::format("cycle in action hierarchy containing `{}`", action.name.ToString()),
                    .primary = action.span,
                });
            }
        }
        for (const ValidatorAction& ancestor : actions)
        {
            for (const EntityUid& descendant : ancestor.descendants)
            {
                actions[index.at(descendant)].ancestors.push_back(ancestor.name);
            }
        }
        for (ValidatorAction& action : actions) { std::ranges::sort(action.ancestors); }
    }

    /// Everything reachable from `start` along `edges`, sorted, each once. `start` is included
    /// only when a cycle leads back to it.
    template <typename Key, typename Node, typename KeyOf>
    static auto Reachable(const Key& start, const std::unordered_map<Key, std::vector<Node>>& edges, KeyOf keyOf)
        -> std::vector<Node>
    {
        std::vector<Node> reached;
        std::unordered_set<Key> seen;
        std::vector<Key> pending{start};
        while (!pending.empty())
        {
            const Key current = pending.back();
            pending.pop_back();
            const auto found = edges.find(current);
            if (found == edges.end()) { continue; }
            for (const Node& next : found->second)
            {
                const Key key = keyOf(next);
                if (!seen.insert(key).second) { continue; }
                reached.push_back(next);
                pending.push_back(key);
            }
        }
        std::ranges::sort(reached);
        return reached;
    }

    /// A parent type or an `appliesTo` type must be a declared entity type, not an action type
    /// (Rust's `check_for_undeclared`).
    void CheckUndeclaredParents(const std::vector<ValidatorEntityType>& entityTypes)
    {
        std::unordered_set<Name> declared;
        for (const ValidatorEntityType& entityType : entityTypes) { declared.insert(entityType.name.GetName()); }
        std::vector<std::string> undeclared;
        for (const auto& [parent, children] : children_)
        {
            if (!declared.contains(parent)) { undeclared.emplace_back(parent.QualifiedText()); }
        }
        std::optional<TypeUse> first;
        for (const TypeUse& use : appliesToTypes_)
        {
            if (declared.contains(use.type.GetName())) { continue; }
            undeclared.emplace_back(use.type.GetName().QualifiedText());
            if (!first) { first = use; }
        }
        if (undeclared.empty()) { return; }
        std::ranges::sort(undeclared);
        const auto [last, end] = std::ranges::unique(undeclared);
        undeclared.erase(last, end);
        std::string list;
        for (const std::string& name : undeclared) { list += std::format("{}`{}`", list.empty() ? "" : ", ", name); }
        diagnostics_.push_back(Diagnostic{
            .code = DiagnosticCode::UndeclaredEntityTypes,
            .message = "undeclared entity type(s): " + list,
            .help = "any entity types appearing anywhere in a schema need to be declared in `entityTypes`; an "
                    "action type is not an entity type here",
            .primary = first ? first->span : std::nullopt,
            .source = first ? *first->source : SharedSourceText{},
        });
    }

    // ---- Results ----

    auto Fail() -> std::unexpected<SchemaError>
    {
        return std::unexpected{*SchemaError::FromDiagnostics(std::move(diagnostics_))};
    }

    static auto StringType() -> SharedType
    {
        static const SharedType type = MakeShared(Type{types::String{}});
        return type;
    }

    static auto LongType() -> SharedType
    {
        static const SharedType type = MakeShared(Type{types::Long{}});
        return type;
    }

    static auto BooleanType() -> SharedType
    {
        static const SharedType type = MakeShared(Type{types::Boolean{}});
        return type;
    }

    struct CommonState
    {
        SharedType type{};
        bool resolving = false;
    };

    struct TypeUse
    {
        EntityType type;
        std::optional<SourceSpan> span;
        const SharedSourceText* source;
    };

    std::vector<CommonDeclarationEntry> commons_;
    std::vector<EntityDeclarationEntry> entities_;
    std::vector<ActionDeclarationEntry> actions_;
    std::deque<SchemaType> builtinTypes_;
    std::vector<std::string_view> builtinBasenames_;
    std::unordered_set<Name> commonNames_;
    std::unordered_set<Name> entityNames_;
    std::unordered_set<EntityUid> actionNames_;
    std::unordered_map<Name, CommonState> commonStates_;
    std::unordered_map<Name, std::vector<EntityType>> children_;
    std::unordered_map<EntityUid, std::vector<EntityUid>> actionChildren_;
    std::vector<TypeUse> appliesToTypes_;
    std::vector<Diagnostic> diagnostics_;
};

} // namespace

auto LinkSchema(const std::span<const SchemaFragmentModel* const> fragments)
    -> std::expected<std::shared_ptr<const ValidatorSchema>, SchemaError>
{
    return Linker{fragments}.Link();
}

} // namespace detail

auto Schema::FromFragments(const std::span<const SchemaFragment> fragments) -> std::expected<Schema, SchemaError>
{
    std::vector<const detail::SchemaFragmentModel*> models;
    models.reserve(fragments.size());
    for (const SchemaFragment& fragment : fragments) { models.push_back(&detail::SchemaAccess::Model(fragment)); }
    auto linked = detail::LinkSchema(models);
    if (!linked) { return std::unexpected{std::move(linked).error()}; }
    return detail::SchemaAccess::MakeSchema(*std::move(linked));
}

auto SchemaFromJson(const std::string_view text, const std::string_view sourceName) -> std::expected<Schema, SchemaError>
{
    auto fragment = SchemaFragmentFromJson(text, sourceName);
    if (!fragment) { return std::unexpected{std::move(fragment).error()}; }
    return Schema::FromFragments(std::span<const SchemaFragment>{&*fragment, 1});
}

auto ParseSchema(const std::string_view text, const std::string_view sourceName)
    -> std::expected<WithWarnings<Schema>, SchemaError>
{
    auto fragment = ParseSchemaFragment(text, sourceName);
    if (!fragment) { return std::unexpected{std::move(fragment).error()}; }
    auto schema = Schema::FromFragments(std::span<const SchemaFragment>{&fragment->value, 1});
    if (!schema) { return std::unexpected{std::move(schema).error()}; }
    return WithWarnings<Schema>{*std::move(schema), std::move(fragment->warnings)};
}

} // namespace cedar
