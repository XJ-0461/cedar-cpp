#include "schema/SchemaFragmentModel.hpp"

#include "schema/SchemaAccess.hpp"

#include <cedar/Schema.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <vector>

namespace cedar
{

namespace detail
{

auto operator==(const RecordAttribute& left, const RecordAttribute& right) -> bool
{
    return left.name == right.name && *left.type == *right.type && left.required == right.required &&
           left.annotations == right.annotations;
}

namespace schema_type
{

auto operator==(const Set& left, const Set& right) -> bool { return *left.element == *right.element; }

} // namespace schema_type

auto EmptyRecordType() -> SchemaType { return SchemaType{.kind = schema_type::Record{}}; }

auto IsEmptyRecord(const SchemaType& type) noexcept -> bool
{
    const auto* record = std::get_if<schema_type::Record>(&type.kind);
    return record != nullptr && record->attributes.empty() && !record->additionalAttributes;
}

auto CheckFragmentRestrictions(const SchemaFragmentModel& model) -> std::expected<void, SchemaError>
{
    std::vector<Diagnostic> diagnostics;
    for (const NamespaceDefinition& definition : model.namespaces)
    {
        // namespace_def.rs `check_action_restrictions`: a declared name is never qualified, so the
        // basename alone decides, in any namespace.
        const auto action = std::ranges::find_if(definition.entityTypes, [](const EntityTypeDeclaration& entityType)
                                                 { return entityType.name.Text() == "Action"; });
        if (action == definition.entityTypes.end()) { continue; }
        const std::string where =
            definition.name ? std::format("namespace `{}`", definition.name->QualifiedText()) : "the empty namespace";
        diagnostics.push_back(Diagnostic{
            .code = DiagnosticCode::ActionEntityTypeDeclared,
            .message = std::format("entity type `Action` declared in `entityTypes` of {}", where),
            .help = "actions are declared in `actions`, and their entity type `Action` is implicit",
            .primary = action->span.value,
            .source = model.source,
        });
    }
    if (diagnostics.empty()) { return {}; }
    return std::unexpected{*SchemaError::FromDiagnostics(std::move(diagnostics))};
}

} // namespace detail

auto operator==(const SchemaFragment& left, const SchemaFragment& right) -> bool
{
    const auto& leftModel = detail::SchemaAccess::SharedModel(left);
    const auto& rightModel = detail::SchemaAccess::SharedModel(right);
    return leftModel == rightModel || *leftModel == *rightModel;
}

} // namespace cedar
