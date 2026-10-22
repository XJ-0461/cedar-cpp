#include <cedar-cpp-cli/Commands.hpp>
#include <cedar-cpp-cli/Inputs.hpp>
#include <cedar-cpp-cli/JsonInputs.hpp>
#include <cedar-cpp-cli/SchemaInputs.hpp>

#include <json/ValueJson.hpp>

#include <cedar/Json.hpp>
#include <cedar/PartialEvaluation.hpp>
#include <cedar/Syntax.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <format>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar::cli
{

namespace
{

constexpr OptionSpecification PrincipalTypeOption{
    .longName = "principal-type", .valueName = "PRINCIPAL_TYPE", .help = "Principal type of the request, e.g., User"};
constexpr OptionSpecification PrincipalEidOption{
    .longName = "principal-eid", .valueName = "PRINCIPAL_EID", .help = "Optional principal eid"};
constexpr OptionSpecification TpeActionOption{
    .longName = "action", .shortName = 'a', .valueName = "ACTION",
    .help = "Action for the request, e.g., Action::\"view\""};
constexpr OptionSpecification ResourceTypeOption{
    .longName = "resource-type", .valueName = "RESOURCE_TYPE", .help = "Resource type of the request, e.g., File"};
constexpr OptionSpecification ResourceEidOption{
    .longName = "resource-eid", .valueName = "RESOURCE_EID", .help = "Optional resource eid"};
constexpr OptionSpecification TpeContextOption{
    .longName = "context", .shortName = 'c', .valueName = "FILE",
    .help = "File containing a JSON object representing the context for the request"};
constexpr OptionSpecification TpeRequestJsonOption{
    .longName = "request-json", .valueName = "FILE",
    .help = "File containing a JSON object representing the entire request, with fields \"principal_type\", "
            "\"principal_eid\", \"action\", \"resource_type\", \"resource_eid\" and \"context\". Replaces "
            "--principal-type, --principal-eid, --action, --resource-type, --resource-eid and --context"};
constexpr OptionSpecification EntitiesOption{
    .longName = "entities", .valueName = "FILE",
    .help = "File containing JSON representation of the Cedar entity hierarchy (required)"};
constexpr OptionSpecification TimingOption{
    .longName = "timing", .shortName = 't', .takesValue = false,
    .help = "Time authorization and report timing information"};

constexpr std::array Options{PrincipalTypeOption, PrincipalEidOption,   TpeActionOption,    ResourceTypeOption,
                             ResourceEidOption,   TpeContextOption,     TpeRequestJsonOption, PoliciesOption,
                             PolicyFormatOption,  TemplateLinkedOption, SchemaOption,       SchemaFormatOption,
                             EntitiesOption,      TimingOption};

/// The request options `--request-json` cannot be given with, in clap's order.
constexpr std::array RequestFieldOptions{PrincipalTypeOption, PrincipalEidOption, TpeActionOption,
                                         ResourceTypeOption,  ResourceEidOption,  TpeContextOption};

/// The fields of the partial request before any is parsed (`command/tpe.rs` `TpeRequestJSON`). The
/// context is JSON text holding only the last member of each repeated key, as the
/// `serde_json::Value` it is read through does.
struct RequestFields
{
    std::string principalType;
    std::optional<std::string> principalEid;
    std::string action;
    std::string resourceType;
    std::optional<std::string> resourceEid;
    std::optional<std::string> context;
};

/// Reads `text` (the file `path`) as one JSON value and writes it back as a `serde_json::Value`
/// holds it.
auto ReadJsonValue(const std::string_view text, const std::string_view path) -> std::expected<std::string, Failure>
{
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(text, scratch);
    if (!document)
    {
        return std::unexpected{Failure{std::format("failed to parse context-json file {}: {}", path, document.error().message)}};
    }
    if (auto checked = detail::CheckStrictJson(document->Root(), detail::DuplicateKeys::LastWins); !checked)
    {
        return std::unexpected{Failure{std::format("failed to parse context-json file {}: {}", path, checked.error().message)}};
    }
    std::string value;
    AppendJson(value, document->Root(), JsonOrigin::Value);
    return value;
}

/// A `String` field of the derived struct, or a missing-field error.
auto ReadRequiredString(const detail::JsonNode* node, const std::string_view field)
    -> std::expected<std::string, std::string>
{
    if (node == nullptr) { return std::unexpected{std::format("missing field `{}`", field)}; }
    if (!node->IsString())
    {
        return std::unexpected{std::format("invalid type: {}, expected a string", detail::DescribeJson(*node))};
    }
    if (!node->strictString) { return std::unexpected{std::format("`{}` holds an unpaired surrogate", field)}; }
    return std::string{node->text};
}

/// An `Option<String>` field: absent or null is none.
auto ReadOptionalString(const detail::JsonNode* node, const std::string_view field)
    -> std::expected<std::optional<std::string>, std::string>
{
    if (node == nullptr || node->kind == detail::JsonKind::Null) { return std::optional<std::string>{}; }
    auto text = ReadRequiredString(node, field);
    if (!text) { return std::unexpected{std::move(text).error()}; }
    return std::optional<std::string>{*std::move(text)};
}

/// The `--request-json` file, read as serde reads `TpeRequestJSON` from a stream.
auto ReadRequestJsonFields(const std::string_view path) -> std::expected<RequestFields, Failure>
{
    auto text = ReadInput(path, "request json");
    if (!text)
    {
        Failure failure = std::move(text).error();
        failure.message = std::format("failed to open request json file {}: {}", path, failure.message);
        return std::unexpected{std::move(failure)};
    }
    const auto fail = [&](const std::string_view message) {
        return std::unexpected{Failure{std::format("failed to parse context-json file {}: {}", path, message)}};
    };
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(*text, scratch);
    if (!document) { return fail(document.error().message); }
    // The fields in declaration order, which serde's sequence form follows.
    static constexpr std::array<std::string_view, 6> Names{"principal_type", "principal_eid", "action",
                                                           "resource_type",  "resource_eid",  "context"};
    auto declared = ReadStructFields(document->Root(), Names, JsonOrigin::Stream, "TpeRequestJSON");
    if (!declared) { return fail(declared.error()); }

    RequestFields fields;
    for (const auto& [index, target] : {std::pair{std::size_t{0}, &fields.principalType},
                                        std::pair{std::size_t{2}, &fields.action},
                                        std::pair{std::size_t{3}, &fields.resourceType}})
    {
        auto value = ReadRequiredString((*declared)[index], Names[index]);
        if (!value) { return fail(value.error()); }
        *target = *std::move(value);
    }
    for (const auto& [index, target] :
         {std::pair{std::size_t{1}, &fields.principalEid}, std::pair{std::size_t{4}, &fields.resourceEid}})
    {
        auto value = ReadOptionalString((*declared)[index], Names[index]);
        if (!value) { return fail(value.error()); }
        *target = *std::move(value);
    }
    // `Option<serde_json::Value>`: absent or null is an unknown context.
    if (const detail::JsonNode* context = (*declared)[5]; context != nullptr && context->kind != detail::JsonKind::Null)
    {
        if (auto checked = detail::CheckStrictJson(*context, detail::DuplicateKeys::LastWins); !checked)
        {
            return fail(checked.error().message);
        }
        std::string value;
        AppendJson(value, *context, JsonOrigin::Value);
        fields.context = std::move(value);
    }
    return fields;
}

/// The request fields of the separate options: the principal type, the action and the resource
/// type are required, in that order, and the `--context` file is read last.
auto ReadRequestOptionFields(const ParsedArguments& arguments) -> std::expected<RequestFields, Failure>
{
    RequestFields fields;
    const std::array<std::pair<const OptionSpecification*, std::string*>, 3> required{
        std::pair{&PrincipalTypeOption, &fields.principalType}, std::pair{&TpeActionOption, &fields.action},
        std::pair{&ResourceTypeOption, &fields.resourceType}};
    for (const auto& [option, target] : required)
    {
        const auto value = arguments.Value(option->longName);
        if (!value)
        {
            // `principal type must be specified`, from the option's name.
            std::string what{option->longName};
            for (char& character : what)
            {
                if (character == '-') { character = ' '; }
            }
            return std::unexpected{Failure{std::format("{} must be specified", what)}};
        }
        *target = std::string{*value};
    }
    if (const auto eid = arguments.Value(PrincipalEidOption.longName)) { fields.principalEid = std::string{*eid}; }
    if (const auto eid = arguments.Value(ResourceEidOption.longName)) { fields.resourceEid = std::string{*eid}; }
    if (const auto contextFile = arguments.Value(TpeContextOption.longName))
    {
        auto text = ReadInput(*contextFile, "context-json");
        if (!text)
        {
            Failure failure = std::move(text).error();
            failure.message = std::format("failed to open context-json file {}: {}", *contextFile, failure.message);
            return std::unexpected{std::move(failure)};
        }
        auto value = ReadJsonValue(*text, *contextFile);
        if (!value) { return std::unexpected{std::move(value).error()}; }
        fields.context = *std::move(value);
    }
    return fields;
}

auto NameFailure(const std::string_view field, const std::string_view text, const NameError error) -> Failure
{
    return Failure{std::format("failed to parse `{}` as an entity type name", field),
                   {Diagnostic{.code = CodeFor(error), .message = std::format("invalid entity type name `{}`", text)}}};
}

/// The partial request the options describe (`TpeRequestArgs::get_request`): the action parsed as
/// a normalized uid, the two types as normalized names, the known context read by the type the
/// schema declares for the action, and the whole checked against the schema.
auto LoadPartialRequest(const ParsedArguments& arguments, const Schema& schema) -> std::expected<PartialRequest, Failure>
{
    const auto requestJson = arguments.Value(TpeRequestJsonOption.longName);
    auto fields = requestJson ? ReadRequestJsonFields(*requestJson) : ReadRequestOptionFields(arguments);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }

    auto action = ParseNormalizedEntityUid(fields->action, "action");
    if (!action)
    {
        return std::unexpected{
            Failure{"failed to parse `action` as an entity UID", std::move(action).error().TakeDiagnostics()}};
    }
    auto principalType = EntityType::Parse(fields->principalType);
    if (!principalType) { return std::unexpected{NameFailure("principal_type", fields->principalType, principalType.error())}; }
    auto resourceType = EntityType::Parse(fields->resourceType);
    if (!resourceType) { return std::unexpected{NameFailure("resource_type", fields->resourceType, resourceType.error())}; }

    std::optional<Context> context;
    if (fields->context)
    {
        auto loaded = ContextFromJson(*fields->context, schema, *action, requestJson.value_or("<context>"));
        if (!loaded)
        {
            return std::unexpected{
                Failure{"failed to parse request context", std::move(loaded).error().TakeDiagnostics()}};
        }
        context = *std::move(loaded);
    }

    const auto partialUid = [](const EntityType type, const std::optional<std::string>& eid) {
        PartialEntityUid uid{.type = type};
        if (eid) { uid.id = Eid{*eid}; }
        return uid;
    };
    auto request = PartialRequest::Create(partialUid(*principalType, fields->principalEid), *std::move(action),
                                          partialUid(*resourceType, fields->resourceEid), std::move(context), schema);
    if (!request)
    {
        return std::unexpected{
            Failure{"the request is not valid for the schema", std::move(request).error().TakeDiagnostics()}};
    }
    return *std::move(request);
}

/// The partial entities file, read by the schema (`command/tpe.rs` `load_partial_entities`).
auto LoadPartialEntities(const std::string_view path, const Schema& schema) -> std::expected<PartialEntities, Failure>
{
    auto text = ReadInput(path, "entities");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto entities = PartialEntitiesFromJson(*text, schema, path);
    if (!entities)
    {
        return std::unexpected{Failure{std::format("failed to parse entities from file {}", path),
                                       std::move(entities).error().TakeDiagnostics()}};
    }
    return *std::move(entities);
}

/// `command/tpe.rs`: reads the policies, the schema, the partial entities and the partial request,
/// reporting every input that fails (a schema that fails ends the reading); then partially
/// authorizes. Exits 0 on Allow, 2 on Deny, and 4 when the decision is unknown, after printing
/// every policy's residual.
auto Run(const Invocation& invocation) -> std::expected<ExitCode, UsageError>
{
    const ParsedArguments& arguments = invocation.arguments;
    auto policyInput = ReadPolicyInput(arguments);
    if (!policyInput) { return std::unexpected{std::move(policyInput).error()}; }
    if (auto checked = CheckSchemaFormat(arguments); !checked) { return std::unexpected{std::move(checked).error()}; }
    if (arguments.Has(TpeRequestJsonOption.longName))
    {
        for (const OptionSpecification& other : RequestFieldOptions)
        {
            if (arguments.Has(other.longName))
            {
                return std::unexpected{UsageError{std::format(
                    "the argument '--request-json <FILE>' cannot be used with '--{} <{}>'", other.longName,
                    other.valueName)}};
            }
        }
    }
    std::string missing;
    for (const OptionSpecification& required : {SchemaOption, EntitiesOption})
    {
        if (!arguments.Has(required.longName)) { missing += std::format("\n  --{} <{}>", required.longName, required.valueName); }
    }
    if (!missing.empty())
    {
        return std::unexpected{UsageError{"the following required arguments were not provided:" + missing}};
    }
    const std::string_view entitiesFile = *arguments.Value(EntitiesOption.longName);

    std::fputs("\n", stdout);
    std::vector<Failure> failures;
    const auto reportFailures = [&] {
        for (const Failure& failure : failures) { Report(invocation.errorFormat, failure); }
        return ExitCode::Failure;
    };
    auto policies = LoadPolicySet(*policyInput);
    if (!policies) { failures.push_back(std::move(policies).error()); }
    auto loadedSchema = LoadSchema(arguments);
    if (!loadedSchema)
    {
        failures.push_back(std::move(loadedSchema).error());
        return reportFailures();
    }
    const Schema& schema = **loadedSchema;
    auto entities = LoadPartialEntities(entitiesFile, schema);
    if (!entities) { failures.push_back(std::move(entities).error()); }
    auto request = LoadPartialRequest(arguments, schema);
    if (!request)
    {
        Failure failure = std::move(request).error();
        failure.message = "failed to parse request: " + failure.message;
        failures.push_back(std::move(failure));
    }
    if (!failures.empty()) { return reportFailures(); }

    const auto start = std::chrono::steady_clock::now();
    auto response = IsAuthorizedPartial(*request, *policies, *entities, schema);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!response)
    {
        failures.push_back(Failure{"partial evaluation failed", std::move(response).error().TakeDiagnostics()});
        return reportFailures();
    }
    std::string text;
    if (arguments.Has(TimingOption.longName))
    {
        text += std::format("Authorization Time (micro seconds) : {}\n",
                            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }
    ExitCode status = ExitCode::Unknown;
    if (const auto decision = response->GetDecision(); !decision)
    {
        text += "UNKNOWN\nAll policy residuals:\n";
        for (const Policy& residual : response->Policies())
        {
            text += ToCedar(residual);
            text += '\n';
        }
    }
    else if (*decision == Decision::Allow)
    {
        text += "ALLOW\n";
        status = ExitCode::Success;
    }
    else
    {
        text += "DENY\n";
        status = ExitCode::AuthorizeDeny;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return status;
}

} // namespace

const Command PartialEvaluationCommand{
    .name = "tpe",
    .summary = "Partially evaluate an authorization request in a type-aware manner",
    .options = Options,
    .run = Run,
};

} // namespace cedar::cli
