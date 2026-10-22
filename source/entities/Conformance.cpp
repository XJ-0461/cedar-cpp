#include "entities/Conformance.hpp"

#include "schema/SchemaAccess.hpp"
#include "schema/ValidatorSchema.hpp"
#include "validation/Types.hpp"

#include <cedar/Expression.hpp>
#include <cedar/Schema.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

namespace detail
{

namespace
{

[[nodiscard]] auto Problem(const DiagnosticCode code, std::string message) -> Diagnostic
{
    return Diagnostic{.code = code, .message = std::move(message)};
}

/// Rust's `Entity::deep_eq`: same uid, attributes, tags and ancestor set.
[[nodiscard]] auto StructurallyEqual(const Entity& left, const Entity& right) noexcept -> bool
{
    return left.Uid() == right.Uid() && Value::FromRecord(left.Attributes()) == Value::FromRecord(right.Attributes())
           && Value::FromRecord(left.Tags()) == Value::FromRecord(right.Tags())
           && std::ranges::equal(left.Ancestors(), right.Ancestors());
}

[[nodiscard]] auto DescribeInvalidUid(const InvalidEntityUid& invalid) -> Diagnostic
{
    if (invalid.problem == EntityUidProblem::UndeclaredAction)
    {
        return Problem(DiagnosticCode::UndeclaredAction,
                       std::format("action `{}` is not declared in the schema", invalid.uid.ToString()));
    }
    return Problem(DiagnosticCode::InvalidEnumEntity,
                   std::format("`{}` is not one of the ids the schema declares for the enumerated entity type `{}`",
                               invalid.uid.Id().Escaped(), invalid.uid.Type().GetName().ToString()));
}

/// The attributes or tags of `entity` against their declared types.
/// An undeclared key fails unless `undeclaredAllowed` (an open entity type), when its value may have
/// any type.
[[nodiscard]] auto CheckValues(const ValidatorSchema& schema, const EntityUid& uid, const Record values,
                               const std::string_view kind, auto&& expectedType, const bool undeclaredAllowed)
    -> std::expected<void, Diagnostic>
{
    for (const RecordEntry& entry : values)
    {
        const Type* expected = expectedType(entry.key);
        if (expected == nullptr && !undeclaredAllowed)
        {
            return std::unexpected{Problem(
                kind == "attribute" ? DiagnosticCode::UnexpectedEntityAttribute : DiagnosticCode::UnexpectedEntityTag,
                std::format("`{}` has the {} `{}`, which the schema does not declare", uid.ToString(), kind, entry.key))};
        }
        if (expected != nullptr && !HasType(entry.value, *expected))
        {
            return std::unexpected{Problem(DiagnosticCode::TypeMismatch,
                                           std::format("the {} `{}` of `{}` is `{}`, not a value of type `{}`", kind,
                                                       entry.key, uid.ToString(), ToCedar(entry.value),
                                                       ToString(*expected)))};
        }
        if (auto checked = CheckEntityUidsIn(schema, entry.value); !checked)
        {
            return std::unexpected{DescribeInvalidUid(checked.error())};
        }
    }
    return {};
}

} // namespace

auto HasType(const Value& value, const Type& type) noexcept -> bool
{
    return std::visit(
        Overloaded{
            [](const types::Never&) { return false; },
            [&](const types::Boolean& boolean) {
                if (value.Kind() != ValueKind::Bool) { return false; }
                if (boolean.kind == BooleanType::True) { return value.AsBool(); }
                if (boolean.kind == BooleanType::False) { return !value.AsBool(); }
                return true;
            },
            [&](const types::Long&) { return value.Kind() == ValueKind::Long; },
            [&](const types::String&) { return value.Kind() == ValueKind::String; },
            [&](const types::AnyEntity&) { return value.Kind() == ValueKind::Entity; },
            [&](const types::Entity& entity) {
                return value.Kind() == ValueKind::Entity &&
                       std::ranges::contains(entity.lub.Types(), value.AsEntity().Type());
            },
            [&](const types::Set& set) {
                if (value.Kind() != ValueKind::Set) { return false; }
                if (set.element == nullptr) { return true; }
                return std::ranges::all_of(value.AsSet(),
                                           [&](const Value& element) { return HasType(element, *set.element); });
            },
            [&](const types::Record& record) {
                if (value.Kind() != ValueKind::Record) { return false; }
                const Record entries = value.AsRecord();
                for (const RecordEntry& entry : entries)
                {
                    const AttributeType* attribute = record.attributes.Find(entry.key);
                    if (attribute == nullptr)
                    {
                        if (record.open == OpenTag::ClosedAttributes) { return false; }
                        continue;
                    }
                    if (!HasType(entry.value, *attribute->type)) { return false; }
                }
                return std::ranges::all_of(record.attributes, [&](const Attribute& attribute) {
                    return !attribute.type.required || entries.Contains(attribute.name);
                });
            },
            [&](const types::Extension& extension) { return value.Kind() == extension.kind; },
        },
        type.kind);
}

auto CheckEntityUid(const ValidatorSchema& schema, const EntityUid& uid) -> std::expected<void, InvalidEntityUid>
{
    if (const ValidatorEntityType* declaration = schema.FindEntityType(uid.Type());
        declaration != nullptr && declaration->kind == ValidatorEntityTypeKind::Enumerated &&
        !declaration->IsEnumChoice(uid.Id()))
    {
        return std::unexpected{InvalidEntityUid{uid, EntityUidProblem::NotAnEnumChoice}};
    }
    if (uid.IsAction() && schema.FindAction(uid) == nullptr)
    {
        return std::unexpected{InvalidEntityUid{uid, EntityUidProblem::UndeclaredAction}};
    }
    return {};
}

auto CheckEntityUidsIn(const ValidatorSchema& schema, const Value& value) -> std::expected<void, InvalidEntityUid>
{
    switch (value.Kind())
    {
    case ValueKind::Entity: return CheckEntityUid(schema, value.AsEntity());
    case ValueKind::Set:
        for (const Value& element : value.AsSet())
        {
            if (auto checked = CheckEntityUidsIn(schema, element); !checked) { return checked; }
        }
        return {};
    case ValueKind::Record:
        for (const RecordEntry& entry : value.AsRecord())
        {
            if (auto checked = CheckEntityUidsIn(schema, entry.value); !checked) { return checked; }
        }
        return {};
    default: return {};
    }
}

auto CheckEntityConformance(const ValidatorSchema& schema, const Entity& entity) -> std::expected<void, Diagnostic>
{
    const EntityUid& uid = entity.Uid();
    if (uid.IsAction())
    {
        const ValidatorAction* action = schema.FindAction(uid);
        if (action == nullptr)
        {
            return std::unexpected{
                Problem(DiagnosticCode::UndeclaredAction, std::format("action `{}` is not declared in the schema",
                                                                      uid.ToString()))};
        }
        if (!StructurallyEqual(entity, *action->entity))
        {
            return std::unexpected{Problem(
                DiagnosticCode::ActionDeclarationMismatch,
                std::format("the entity `{}` differs from the schema's action: an action entity has no attributes or "
                            "tags, and exactly the ancestors the schema declares",
                            uid.ToString()))};
        }
        return {};
    }

    const ValidatorEntityType* declaration = schema.FindEntityType(uid.Type());
    if (declaration == nullptr)
    {
        return std::unexpected{Problem(DiagnosticCode::UnexpectedEntityType,
                                       std::format("entity `{}` has the type `{}`, which the schema does not declare",
                                                   uid.ToString(), uid.Type().GetName().ToString()))};
    }
    if (declaration->kind == ValidatorEntityTypeKind::External)
    {
        return std::unexpected{Problem(
            DiagnosticCode::ExternalEntityType,
            std::format("entity `{}` has the external type `{}`, whose entities are not given as data", uid.ToString(),
                        uid.Type().GetName().ToString()))};
    }
    if (auto checked = CheckEntityUid(schema, uid); !checked) { return std::unexpected{DescribeInvalidUid(checked.error())}; }

    for (const Attribute& attribute : declaration->attributes)
    {
        if (attribute.type.required && !entity.Attributes().Contains(attribute.name))
        {
            return std::unexpected{
                Problem(DiagnosticCode::MissingRequiredEntityAttribute,
                        std::format("`{}` has no attribute `{}`, which the schema requires", uid.ToString(),
                                    attribute.name))};
        }
    }
    if (auto checked = CheckValues(
            schema, uid, entity.Attributes(), "attribute",
            [&](const std::string_view key) -> const Type* {
                const AttributeType* found = declaration->attributes.Find(key);
                return found != nullptr ? found->type.get() : nullptr;
            },
            declaration->openAttributes == OpenTag::OpenAttributes);
        !checked)
    {
        return checked;
    }

    // Rust reports the least offending ancestor; any one will do for a rejection.
    for (const EntityUid& ancestor : entity.Ancestors())
    {
        if (auto checked = CheckEntityUid(schema, ancestor); !checked)
        {
            return std::unexpected{DescribeInvalidUid(checked.error())};
        }
        if (!declaration->AllowsParentType(ancestor.Type()))
        {
            return std::unexpected{
                Problem(DiagnosticCode::InvalidAncestorType,
                        std::format("`{}` is not allowed to have an ancestor of type `{}` according to the schema",
                                    uid.ToString(), ancestor.Type().GetName().ToString()))};
        }
    }

    return CheckValues(
        schema, uid, entity.Tags(), "tag",
        [&](std::string_view) -> const Type* { return declaration->tags ? &*declaration->tags : nullptr; }, false);
}

} // namespace detail

auto Entities::Create(std::vector<Entity> entities, const TransitiveClosureMode mode, const Schema& schema)
    -> std::expected<Entities, EntitiesError>
{
    // Rust's `Entities::from_entities`: entities that are not actions are checked before the
    // closure, actions after it (an action's closed ancestors must be the schema's), and then the
    // schema's action entities join the store.
    const detail::ValidatorSchema& validator = detail::SchemaAccess::Validator(schema);
    for (const Entity& entity : entities)
    {
        if (entity.Uid().IsAction()) { continue; }
        if (auto checked = detail::CheckEntityConformance(validator, entity); !checked)
        {
            return std::unexpected{EntitiesError{std::move(checked).error()}};
        }
    }
    auto store = Entities::Create(std::move(entities), mode);
    if (!store) { return store; }
    for (const Entity& entity : *store)
    {
        if (!entity.Uid().IsAction()) { continue; }
        if (auto checked = detail::CheckEntityConformance(validator, entity); !checked)
        {
            return std::unexpected{EntitiesError{std::move(checked).error()}};
        }
    }
    return std::move(*store).Add(validator.ActionEntities(), TransitiveClosureMode::AssumeAlreadyComputed);
}

} // namespace cedar
