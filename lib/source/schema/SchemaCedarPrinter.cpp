// The fragment model in Cedar schema syntax (Rust's `json_schema_to_cedar_schema_str` and the
// `Display` impls in `cedar-policy-core/src/validator/cedar_schema/fmt.rs`), character for
// character: the layout, the two-space indentation steps, the quirks (an empty record type prints
// `{` and `}` with the current indentation between them; a `tags` type prints at indentation zero)
// and the two refusals.

#include "model/EscapeDebug.hpp"
#include "schema/SchemaAccess.hpp"
#include "schema/SchemaFragmentModel.hpp"

#include <cedar/Schema.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
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

/// Rust's `is_normalized_ident`: an attribute name that prints without quotes.
[[nodiscard]] auto IsNormalizedIdentifier(const std::string_view text) -> bool
{
    return CheckIdentifier(text).has_value() && text != ReservedNamespaceName;
}

class CedarPrinter final
{
public:
    [[nodiscard]] auto Print(const SchemaFragmentModel& model) -> std::string
    {
        for (std::size_t index = 0; index < model.namespaces.size(); ++index)
        {
            const NamespaceDefinition& definition = model.namespaces[index];
            if (!definition.name) { PrintNamespaceBody(definition, 0); }
            else
            {
                PrintAnnotations(definition.annotations, 0);
                output_ += std::format("namespace {} {{\n", definition.name->QualifiedText());
                PrintNamespaceBody(definition, 2);
                output_ += "}\n";
            }
            if (index + 1 < model.namespaces.size()) { output_ += '\n'; }
        }
        return std::move(output_);
    }

private:
    void Indent(const std::size_t indentation) { output_.append(indentation, ' '); }

    void PrintQuoted(const std::string_view text)
    {
        output_ += '"';
        detail::AppendEscapeDebug(output_, text);
        output_ += '"';
    }

    void PrintAnnotations(const Annotations& annotations, const std::size_t indentation)
    {
        for (const Annotation& annotation : annotations)
        {
            Indent(indentation);
            output_ += '@';
            output_ += annotation.key.Text();
            if (annotation.value)
            {
                output_ += '(';
                PrintQuoted(*annotation.value);
                output_ += ')';
            }
            output_ += '\n';
        }
    }

    /// Declarations one per line, with a blank line between them: common types, then entity types,
    /// then actions, each group in name order.
    void PrintNamespaceBody(const NamespaceDefinition& definition, const std::size_t indentation)
    {
        const std::size_t total = definition.commonTypes.size() + definition.entityTypes.size() + definition.actions.size();
        std::size_t printed = 0;
        const auto separate = [&]
        {
            if (++printed < total) { output_ += '\n'; }
        };
        for (const auto& common : definition.commonTypes)
        {
            PrintAnnotations(common.annotations, indentation);
            Indent(indentation);
            output_ += std::format("type {} = ", common.name.Text());
            PrintType(common.type, indentation);
            output_ += ";\n";
            separate();
        }
        for (const EntityTypeDeclaration& entityType : definition.entityTypes)
        {
            PrintAnnotations(entityType.annotations, indentation);
            Indent(indentation);
            output_ += std::format("{}entity {}", entityType.external ? "external " : "", entityType.name.Text());
            PrintEntityType(entityType, indentation);
            output_ += ";\n";
            separate();
        }
        for (const ActionDeclaration& action : definition.actions)
        {
            PrintAnnotations(action.annotations, indentation);
            Indent(indentation);
            output_ += action.external ? "external action " : "action ";
            PrintQuoted(action.name);
            PrintAction(action, indentation);
            output_ += ";\n";
            separate();
        }
    }

    void PrintTypeNames(const std::vector<TypeName>& names)
    {
        output_ += '[';
        for (std::size_t index = 0; index < names.size(); ++index)
        {
            if (index > 0) { output_ += ", "; }
            output_ += names[index].name.QualifiedText();
        }
        output_ += ']';
    }

    void PrintEntityType(const EntityTypeDeclaration& entityType, const std::size_t indentation)
    {
        if (const auto* enumerated = std::get_if<detail::EnumEntityType>(&entityType.kind))
        {
            output_ += " enum [";
            for (std::size_t index = 0; index < enumerated->choices.size(); ++index)
            {
                if (index > 0) { output_ += ", "; }
                PrintQuoted(enumerated->choices[index].Text());
            }
            output_ += ']';
            return;
        }
        const auto& standard = std::get<detail::StandardEntityType>(entityType.kind);
        if (!standard.memberOfTypes.empty())
        {
            output_ += " in ";
            PrintTypeNames(standard.memberOfTypes);
        }
        if (!detail::IsEmptyRecord(standard.shape))
        {
            output_ += " = ";
            PrintType(standard.shape, indentation);
        }
        if (standard.tags)
        {
            // Rust writes the tags type with `Display`, which starts from no indentation.
            output_ += " tags ";
            PrintType(*standard.tags, 0);
        }
    }

    void PrintActionReference(const ActionReference& reference)
    {
        output_ += reference.type ? reference.type->name.QualifiedText() : std::string_view{"Action"};
        output_ += "::";
        PrintQuoted(reference.id);
    }

    void PrintAction(const ActionDeclaration& action, const std::size_t indentation)
    {
        if (action.memberOf && !action.memberOf->empty())
        {
            output_ += " in [";
            for (std::size_t index = 0; index < action.memberOf->size(); ++index)
            {
                if (index > 0) { output_ += ", "; }
                PrintActionReference((*action.memberOf)[index]);
            }
            output_ += ']';
        }
        // An action that applies to no principal or no resource prints no `appliesTo`.
        if (!action.appliesTo || action.appliesTo->principalTypes.empty() || action.appliesTo->resourceTypes.empty())
        {
            return;
        }
        const std::size_t member = indentation + 2;
        output_ += " appliesTo {\n";
        Indent(member);
        output_ += "principal: ";
        PrintTypeNames(action.appliesTo->principalTypes);
        output_ += ",\n";
        Indent(member);
        output_ += "resource: ";
        PrintTypeNames(action.appliesTo->resourceTypes);
        output_ += ",\n";
        Indent(member);
        output_ += "context: ";
        if (detail::IsEmptyRecord(action.appliesTo->context)) { output_ += "{}"; }
        else { PrintType(action.appliesTo->context, member); }
        output_ += '\n';
        Indent(indentation);
        output_ += '}';
    }

    void PrintType(const SchemaType& type, const std::size_t indentation)
    {
        std::visit([&](const auto& kind) { PrintKind(kind, indentation); }, type.kind);
    }

    void PrintKind(const schema_type::String&, std::size_t) { output_ += "__cedar::String"; }
    void PrintKind(const schema_type::Long&, std::size_t) { output_ += "__cedar::Long"; }
    void PrintKind(const schema_type::Boolean&, std::size_t) { output_ += "__cedar::Bool"; }
    void PrintKind(const schema_type::Entity& entity, std::size_t) { output_ += entity.name.name.QualifiedText(); }

    void PrintKind(const schema_type::EntityOrCommon& reference, std::size_t)
    {
        output_ += reference.name.name.QualifiedText();
    }

    void PrintKind(const schema_type::Extension& extension, std::size_t)
    {
        output_ += std::format("{}::{}", ReservedNamespaceName, extension.name.Text());
    }

    void PrintKind(const schema_type::CommonTypeReference& reference, std::size_t)
    {
        output_ += reference.name.name.QualifiedText();
    }

    void PrintKind(const schema_type::Set& set, const std::size_t indentation)
    {
        output_ += "Set<";
        PrintType(*set.element, indentation);
        output_ += '>';
    }

    void PrintKind(const schema_type::Record& record, const std::size_t indentation)
    {
        const std::size_t member = indentation + 2;
        output_ += '{';
        for (std::size_t index = 0; index < record.attributes.size(); ++index)
        {
            const detail::RecordAttribute& attribute = record.attributes[index];
            if (index == 0) { output_ += '\n'; }
            PrintAnnotations(attribute.annotations, member);
            Indent(member);
            if (IsNormalizedIdentifier(attribute.name)) { output_ += attribute.name; }
            else { PrintQuoted(attribute.name); }
            output_ += attribute.required ? ": " : "?: ";
            PrintType(*attribute.type, member);
            output_ += index + 1 < record.attributes.size() ? ",\n" : "\n";
        }
        Indent(indentation);
        output_ += '}';
    }

    std::string output_;
};

[[nodiscard]] auto QualifiedText(const std::optional<Name>& namespaceName, const std::string_view name) -> std::string
{
    return namespaceName ? std::format("{}::{}", namespaceName->QualifiedText(), name) : std::string{name};
}

[[nodiscard]] auto JoinNames(const std::vector<std::string>& names) -> std::string
{
    std::string joined;
    for (const std::string& name : names) { joined += std::format("{}`{}`", joined.empty() ? "" : ", ", name); }
    return joined;
}

} // namespace

auto ToCedar(const SchemaFragment& fragment) -> std::expected<std::string, SchemaError>
{
    const SchemaFragmentModel& model = detail::SchemaAccess::Model(fragment);

    // A named namespace that declares an entity type and a common type of one name cannot be
    // written: in Cedar syntax the common type would shadow the entity type.
    std::vector<std::string> collisions;
    for (const NamespaceDefinition& definition : model.namespaces)
    {
        if (!definition.name) { continue; }
        for (const auto& common : definition.commonTypes)
        {
            if (detail::FindSorted(std::span{definition.entityTypes}, common.name) != nullptr)
            {
                collisions.push_back(QualifiedText(definition.name, common.name.Text()));
            }
        }
    }
    if (!collisions.empty())
    {
        return std::unexpected{SchemaError{Diagnostic{
            .code = DiagnosticCode::SchemaNameCollisions,
            .message = std::format("these names are declared as both an entity type and a common type: {}",
                                   JoinNames(collisions)),
            .source = model.source}}};
    }

    // Cedar syntax writes an entity shape only as a record type.
    std::vector<std::string> unconvertible;
    for (const NamespaceDefinition& definition : model.namespaces)
    {
        for (const EntityTypeDeclaration& entityType : definition.entityTypes)
        {
            const auto* standard = std::get_if<detail::StandardEntityType>(&entityType.kind);
            if (entityType.external || standard == nullptr ||
                std::holds_alternative<schema_type::Record>(standard->shape.kind))
            {
                continue;
            }
            unconvertible.push_back(QualifiedText(definition.name, entityType.name.Text()));
        }
    }
    if (!unconvertible.empty())
    {
        return std::unexpected{SchemaError{Diagnostic{
            .code = DiagnosticCode::UnconvertibleEntityTypeShape,
            .message = std::format("these entity types have shapes that Cedar schema syntax cannot write: {}",
                                   JoinNames(unconvertible)),
            .help = "an entity shape must be a record type written out in place, not a reference to a common type",
            .source = model.source}}};
    }

    return CedarPrinter{}.Print(model);
}

} // namespace cedar
