// The fragment model in the JSON schema format: serde's serialization of `json_schema::Fragment`
// (`cedar-policy-core/src/validator/json_schema.rs`), field by field in declaration order with
// serde's `skip_serializing_if` defaults left out, and the variant with every type reference
// resolved (`Fragment::to_internal_name_fragment_with_resolved_types`, reached through
// `cedar-policy/src/api.rs` `schema_str_to_json_with_resolved_types`).

#include "json/JsonReader.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"

#include <cedar/Schema.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cedar
{

namespace
{

using detail::ActionDeclaration;
using detail::ActionReference;
using detail::EntityTypeDeclaration;
using detail::NamespaceDefinition;
using detail::SchemaFragmentModel;
using detail::SchemaType;
using detail::TypeName;
namespace schema_type = detail::schema_type;

/// JSON text in serde_json's compact layout, or its pretty layout (two-space indent, `": "` after
/// keys, an empty object or array as `{}` or `[]`).
class JsonOutput final
{
public:
    explicit JsonOutput(const bool pretty) noexcept : pretty_{pretty} {}

    void BeginObject() { Begin('{', false); }
    void EndObject() { End('}'); }
    void BeginArray() { Begin('[', true); }
    void EndArray() { End(']'); }

    void Key(const std::string_view key)
    {
        Separate();
        detail::AppendJsonString(text_, key);
        text_ += pretty_ ? ": " : ":";
    }

    void String(const std::string_view value)
    {
        BeforeValue();
        detail::AppendJsonString(text_, value);
    }

    void Null()
    {
        BeforeValue();
        text_ += "null";
    }

    void Boolean(const bool value)
    {
        BeforeValue();
        text_ += value ? "true" : "false";
    }

    [[nodiscard]] auto Take() -> std::string { return std::move(text_); }

private:
    struct Frame
    {
        bool array;
        bool empty = true;
    };

    void Begin(const char open, const bool array)
    {
        BeforeValue();
        text_.push_back(open);
        frames_.push_back(Frame{array});
    }

    void End(const char close)
    {
        const bool empty = frames_.back().empty;
        frames_.pop_back();
        if (pretty_ && !empty) { NewLine(); }
        text_.push_back(close);
    }

    void BeforeValue()
    {
        if (!frames_.empty() && frames_.back().array) { Separate(); }
    }

    void Separate()
    {
        Frame& frame = frames_.back();
        if (!frame.empty) { text_.push_back(','); }
        frame.empty = false;
        if (pretty_) { NewLine(); }
    }

    void NewLine()
    {
        text_.push_back('\n');
        text_.append(2 * frames_.size(), ' ');
    }

    bool pretty_;
    std::string text_;
    std::vector<Frame> frames_;
};

enum class ReferenceKind : std::uint8_t
{
    Common,
    Entity,
    CommonOrEntity,
};

/// Rust's `ExtensionType`s, which resolve as common types in `__cedar` and, unless declared there,
/// in the empty namespace.
constexpr std::array<std::string_view, 4> ExtensionTypeNames{"datetime", "decimal", "duration", "ipaddr"};

/// Resolves the type and action references of one fragment against its own declarations (Rust's
/// `AllDefs::single_fragment` plus the built-ins `to_internal_name_fragment_with_resolved_types`
/// adds, then `ConditionalName::resolve`): an unqualified name in namespace `N` is `N::name` if
/// that is declared, else `name` in the empty namespace.
class Resolver final
{
public:
    explicit Resolver(const SchemaFragmentModel& model) : source_{model.source}
    {
        for (const NamespaceDefinition& definition : model.namespaces)
        {
            const std::string prefix = Prefix(definition.name);
            for (const auto& entityType : definition.entityTypes) { entities_.insert(prefix + std::string{entityType.name.Text()}); }
            for (const auto& common : definition.commonTypes) { commonTypes_.insert(prefix + std::string{common.name.Text()}); }
            for (const ActionDeclaration& action : definition.actions) { actions_.emplace(prefix + "Action", action.name); }
        }
        const auto addBuiltin = [&](const std::string_view name)
        {
            commonTypes_.insert(std::format("{}::{}", ReservedNamespaceName, name));
            if (!commonTypes_.contains(name) && !entities_.contains(name)) { commonTypes_.emplace(name); }
        };
        for (const std::string_view primitive : {"Bool", "Long", "String"}) { addBuiltin(primitive); }
        for (const std::string_view extension : ExtensionTypeNames) { addBuiltin(extension); }
        // `AllDefs::add_action_entity_types`: each namespace that declares an action has its
        // `Action` entity type.
        for (const NamespaceDefinition& definition : model.namespaces)
        {
            if (!definition.actions.empty()) { entities_.insert(Prefix(definition.name) + "Action"); }
        }
    }

    void Enter(const std::optional<Name>& name) { namespace_ = name; }

    [[nodiscard]] auto IsCommonType(const std::string_view name) const -> bool { return commonTypes_.contains(name); }

    /// The fully qualified name `name` refers to, or nothing (and an error) when nothing of the
    /// kind is declared under any of its possible names.
    [[nodiscard]] auto Resolve(const TypeName& name, const ReferenceKind kind) -> std::optional<std::string>
    {
        const std::vector<std::string> possibilities = Possibilities(name.name);
        for (const std::string& possibility : possibilities)
        {
            if (kind != ReferenceKind::Entity && commonTypes_.contains(possibility)) { return possibility; }
            if (kind != ReferenceKind::Common && entities_.contains(possibility)) { return possibility; }
        }
        const std::string_view what = kind == ReferenceKind::Common   ? "a common type"
                                      : kind == ReferenceKind::Entity ? "an entity type"
                                                                      : "a common or entity type";
        errors_.push_back(Diagnostic{.code = DiagnosticCode::TypeNotDefined,
                                     .message = std::format("`{}` is not declared as {}", name.name.QualifiedText(), what),
                                     .help = Help(possibilities),
                                     .primary = name.span.value,
                                     .source = source_});
        return std::nullopt;
    }

    /// The fully qualified type of the action `reference` refers to (Rust's
    /// `ActionEntityUID::fully_qualify_type_references`), or nothing (and an error).
    [[nodiscard]] auto ResolveAction(const ActionReference& reference) -> std::optional<std::string>
    {
        const Name type = reference.type ? reference.type->name : Name::Unqualified(*Identifier::Parse("Action"));
        const std::vector<std::string> possibilities = Possibilities(type);
        for (const std::string& possibility : possibilities)
        {
            // An action's type is a `Name`, so a possibility inside `__cedar` names no action.
            if (Name::Parse(possibility) && actions_.contains(std::pair{possibility, reference.id})) { return possibility; }
        }
        errors_.push_back(Diagnostic{
            .code = DiagnosticCode::ActionNotDefined,
            .message = std::format("action `{}::\"{}\"` is not declared", type.QualifiedText(), reference.id),
            .help = Help(possibilities),
            .primary = reference.span.value,
            .source = source_});
        return std::nullopt;
    }

    [[nodiscard]] auto TakeErrors() -> std::vector<Diagnostic> { return std::move(errors_); }

private:
    [[nodiscard]] static auto Prefix(const std::optional<Name>& name) -> std::string
    {
        return name ? std::format("{}::", name->QualifiedText()) : std::string{};
    }

    [[nodiscard]] auto Possibilities(const Name& name) const -> std::vector<std::string>
    {
        std::vector<std::string> possibilities;
        if (name.IsUnqualified() && namespace_) { possibilities.push_back(Prefix(namespace_) + name.ToString()); }
        possibilities.push_back(name.ToString());
        return possibilities;
    }

    [[nodiscard]] static auto Help(const std::vector<std::string>& possibilities) -> std::string
    {
        std::string help = "looked for ";
        for (std::size_t index = 0; index < possibilities.size(); ++index)
        {
            help += std::format("{}`{}`", index == 0 ? "" : " and ", possibilities[index]);
        }
        return help;
    }

    SharedSourceText source_;
    std::set<std::string, std::less<>> entities_;
    std::set<std::string, std::less<>> commonTypes_;
    std::set<std::pair<std::string, std::string>> actions_;
    std::optional<Name> namespace_;
    std::vector<Diagnostic> errors_;
};

/// Writes a fragment. With a resolver, every reference is written fully qualified and every
/// `EntityOrCommon` type as the `Entity` or common-type reference it resolves to.
class FragmentWriter final
{
public:
    FragmentWriter(JsonOutput& output, Resolver* resolver) noexcept : output_{output}, resolver_{resolver} {}

    void WriteFragment(const SchemaFragmentModel& model)
    {
        output_.BeginObject();
        for (const NamespaceDefinition& definition : model.namespaces)
        {
            if (resolver_ != nullptr) { resolver_->Enter(definition.name); }
            output_.Key(definition.name ? definition.name->QualifiedText() : std::string_view{});
            WriteNamespace(definition);
        }
        output_.EndObject();
    }

private:
    void WriteNamespace(const NamespaceDefinition& definition)
    {
        output_.BeginObject();
        if (!definition.commonTypes.empty())
        {
            output_.Key("commonTypes");
            output_.BeginObject();
            for (const auto& common : definition.commonTypes)
            {
                output_.Key(common.name.Text());
                output_.BeginObject();
                WriteTypeMembers(common.type);
                WriteAnnotations(common.annotations);
                output_.EndObject();
            }
            output_.EndObject();
        }
        output_.Key("entityTypes");
        output_.BeginObject();
        for (const EntityTypeDeclaration& entityType : definition.entityTypes)
        {
            output_.Key(entityType.name.Text());
            WriteEntityType(entityType);
        }
        output_.EndObject();
        output_.Key("actions");
        output_.BeginObject();
        for (const ActionDeclaration& action : definition.actions)
        {
            output_.Key(action.name);
            WriteAction(action);
        }
        output_.EndObject();
        WriteAnnotations(definition.annotations);
        output_.EndObject();
    }

    void WriteEntityType(const EntityTypeDeclaration& entityType)
    {
        output_.BeginObject();
        if (const auto* standard = std::get_if<detail::StandardEntityType>(&entityType.kind))
        {
            if (!standard->memberOfTypes.empty())
            {
                output_.Key("memberOfTypes");
                WriteEntityTypeNames(standard->memberOfTypes);
            }
            if (!detail::IsEmptyRecord(standard->shape))
            {
                output_.Key("shape");
                WriteType(standard->shape);
            }
            if (standard->tags)
            {
                output_.Key("tags");
                WriteType(*standard->tags);
            }
        }
        else
        {
            output_.Key("enum");
            output_.BeginArray();
            for (const Eid& choice : std::get<detail::EnumEntityType>(entityType.kind).choices) { output_.String(choice.Text()); }
            output_.EndArray();
        }
        if (entityType.external)
        {
            output_.Key("external");
            output_.Boolean(true);
        }
        WriteAnnotations(entityType.annotations);
        output_.EndObject();
    }

    void WriteAction(const ActionDeclaration& action)
    {
        output_.BeginObject();
        if (action.appliesTo)
        {
            output_.Key("appliesTo");
            output_.BeginObject();
            output_.Key("resourceTypes");
            WriteEntityTypeNames(action.appliesTo->resourceTypes);
            output_.Key("principalTypes");
            WriteEntityTypeNames(action.appliesTo->principalTypes);
            if (!detail::IsEmptyRecord(action.appliesTo->context))
            {
                output_.Key("context");
                WriteType(action.appliesTo->context);
            }
            output_.EndObject();
        }
        if (action.memberOf)
        {
            output_.Key("memberOf");
            output_.BeginArray();
            for (const ActionReference& parent : *action.memberOf) { WriteActionReference(parent); }
            output_.EndArray();
        }
        if (action.external)
        {
            output_.Key("external");
            output_.Boolean(true);
        }
        WriteAnnotations(action.annotations);
        output_.EndObject();
    }

    void WriteActionReference(const ActionReference& reference)
    {
        output_.BeginObject();
        output_.Key("id");
        output_.String(reference.id);
        if (resolver_ != nullptr)
        {
            const std::optional<std::string> type = resolver_->ResolveAction(reference);
            output_.Key("type");
            output_.String(type ? *type : std::string{});
        }
        else if (reference.type)
        {
            output_.Key("type");
            output_.String(reference.type->name.QualifiedText());
        }
        output_.EndObject();
    }

    void WriteEntityTypeNames(const std::vector<TypeName>& names)
    {
        output_.BeginArray();
        for (const TypeName& name : names) { output_.String(Reference(name, ReferenceKind::Entity)); }
        output_.EndArray();
    }

    /// The name as written, or as resolved.
    [[nodiscard]] auto Reference(const TypeName& name, const ReferenceKind kind) -> std::string
    {
        if (resolver_ == nullptr) { return name.name.ToString(); }
        return resolver_->Resolve(name, kind).value_or(name.name.ToString());
    }

    void WriteType(const SchemaType& type)
    {
        output_.BeginObject();
        WriteTypeMembers(type);
        output_.EndObject();
    }

    /// A type's members, written into the object being written (serde's `flatten`).
    void WriteTypeMembers(const SchemaType& type)
    {
        std::visit([&](const auto& kind) { WriteKind(kind); }, type.kind);
    }

    void WriteTag(const std::string_view tag)
    {
        output_.Key("type");
        output_.String(tag);
    }

    void WriteKind(const schema_type::String&) { WriteTag("String"); }
    void WriteKind(const schema_type::Long&) { WriteTag("Long"); }
    void WriteKind(const schema_type::Boolean&) { WriteTag("Boolean"); }

    void WriteKind(const schema_type::Set& set)
    {
        WriteTag("Set");
        output_.Key("element");
        WriteType(*set.element);
    }

    void WriteKind(const schema_type::Record& record)
    {
        WriteTag("Record");
        output_.Key("attributes");
        output_.BeginObject();
        for (const detail::RecordAttribute& attribute : record.attributes)
        {
            output_.Key(attribute.name);
            output_.BeginObject();
            WriteTypeMembers(*attribute.type);
            WriteAnnotations(attribute.annotations);
            if (!attribute.required)
            {
                output_.Key("required");
                output_.Boolean(false);
            }
            output_.EndObject();
        }
        output_.EndObject();
        if (record.additionalAttributes)
        {
            output_.Key("additionalAttributes");
            output_.Boolean(true);
        }
    }

    void WriteKind(const schema_type::Entity& entity)
    {
        WriteTag("Entity");
        output_.Key("name");
        output_.String(Reference(entity.name, ReferenceKind::Entity));
    }

    void WriteKind(const schema_type::EntityOrCommon& reference)
    {
        if (resolver_ == nullptr)
        {
            WriteTag("EntityOrCommon");
            output_.Key("name");
            output_.String(reference.name.name.QualifiedText());
            return;
        }
        // `TypeVariant::resolve_type_variant_entity_or_common`: a common type wins over an entity type.
        const std::string resolved = Reference(reference.name, ReferenceKind::CommonOrEntity);
        if (resolver_->IsCommonType(resolved))
        {
            WriteTag(resolved);
            return;
        }
        WriteTag("Entity");
        output_.Key("name");
        output_.String(resolved);
    }

    void WriteKind(const schema_type::Extension& extension)
    {
        WriteTag("Extension");
        output_.Key("name");
        output_.String(extension.name.Text());
    }

    void WriteKind(const schema_type::CommonTypeReference& reference)
    {
        WriteTag(Reference(reference.name, ReferenceKind::Common));
    }

    void WriteAnnotations(const Annotations& annotations)
    {
        if (annotations.IsEmpty()) { return; }
        output_.Key("annotations");
        output_.BeginObject();
        for (const Annotation& annotation : annotations)
        {
            output_.Key(annotation.key.Text());
            if (annotation.value) { output_.String(*annotation.value); }
            else { output_.Null(); }
        }
        output_.EndObject();
    }

    JsonOutput& output_;
    Resolver* resolver_;
};

} // namespace

auto ToJson(const SchemaFragment& fragment) -> std::string
{
    JsonOutput output{false};
    FragmentWriter{output, nullptr}.WriteFragment(detail::SchemaAccess::Model(fragment));
    return output.Take();
}

auto ToJsonWithResolvedTypes(const SchemaFragment& fragment) -> std::expected<std::string, SchemaError>
{
    const SchemaFragmentModel& model = detail::SchemaAccess::Model(fragment);
    Resolver resolver{model};
    JsonOutput output{true};
    FragmentWriter{output, &resolver}.WriteFragment(model);
    if (auto error = SchemaError::FromDiagnostics(resolver.TakeErrors())) { return std::unexpected{*std::move(error)}; }
    return output.Take();
}

} // namespace cedar
