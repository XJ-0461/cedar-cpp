#include <cedar-cpp-cli/Requests.hpp>

#include <cedar-cpp-cli/Inputs.hpp>
#include <cedar-cpp-cli/JsonInputs.hpp>

#include <cedar/Json.hpp>
#include <cedar/Syntax.hpp>

#include <format>
#include <memory_resource>
#include <optional>

namespace cedar::cli
{

auto CheckRequestArguments(const ParsedArguments& arguments) -> std::expected<void, UsageError>
{
    if (arguments.Has(RequestJsonOption.longName))
    {
        for (const OptionSpecification& other : {PrincipalOption, ActionOption, ResourceOption, ContextOption})
        {
            if (arguments.Has(other.longName))
            {
                return std::unexpected{UsageError{std::format(
                    "the argument '--request-json <FILE>' cannot be used with '--{} <{}>'", other.longName,
                    other.valueName)}};
            }
        }
    }
    if (const auto validation = arguments.Value(RequestValidationOption.longName);
        validation && *validation != "true" && *validation != "false")
    {
        return std::unexpected{UsageError{std::format(
            "invalid value '{}' for '--request-validation <REQUEST_VALIDATION>' [possible values: true, false]",
            *validation)}};
    }
    return {};
}

namespace
{

auto LoadRequestJson(const std::string_view path, const RequestSchema requestSchema) -> std::expected<Request, Failure>
{
    auto text = ReadInput(path, "request-json");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    std::pmr::monotonic_buffer_resource scratch;
    auto document = detail::ReadJson(*text, scratch);
    if (!document)
    {
        return std::unexpected{
            Failure{std::format("failed to parse request-json file {}: {}", path, document.error().message)}};
    }
    return ReadRequestJson(document->Root(), JsonOrigin::Stream, path, requestSchema);
}

} // namespace

auto LoadRequest(const ParsedArguments& arguments, const std::optional<Schema>& schema)
    -> std::expected<Request, Failure>
{
    const RequestSchema requestSchema{.schema = schema ? &*schema : nullptr,
                                      .validate = arguments.Value(RequestValidationOption.longName) != "false"};
    if (const auto requestJson = arguments.Value(RequestJsonOption.longName))
    {
        return LoadRequestJson(*requestJson, requestSchema);
    }

    // Each uid given is parsed before the context is read, and the missing-uid check comes last.
    std::array<std::optional<EntityUid>, 3> uids{};
    const std::array<OptionSpecification, 3> uidOptions{PrincipalOption, ActionOption, ResourceOption};
    for (std::size_t index = 0; index < uids.size(); ++index)
    {
        const auto text = arguments.Value(uidOptions[index].longName);
        if (!text) { continue; }
        auto uid = ParseNormalizedEntityUid(*text, uidOptions[index].longName);
        if (!uid)
        {
            return std::unexpected{
                Failure{std::format("failed to parse {} {} as entity Uid", uidOptions[index].longName, *text),
                        std::move(uid).error().TakeDiagnostics()}};
        }
        uids[index] = *std::move(uid);
    }

    Context context;
    if (const auto contextFile = arguments.Value(ContextOption.longName))
    {
        auto text = ReadInput(*contextFile, "context");
        if (!text) { return std::unexpected{std::move(text).error()}; }
        // Without an action the context is read without the schema.
        auto loaded = schema && uids[1] ? ContextFromJson(*text, *schema, *uids[1], *contextFile)
                                        : ContextFromJson(*text, *contextFile);
        if (!loaded)
        {
            return std::unexpected{Failure{std::format("failed to create a context from {}", *contextFile),
                                           std::move(loaded).error().TakeDiagnostics()}};
        }
        context = *std::move(loaded);
    }

    if (!uids[0] || !uids[1] || !uids[2])
    {
        return std::unexpected{Failure{"All three (`principal`, `action`, `resource`) variables must be specified"}};
    }
    return MakeRequest(*std::move(uids[0]), *std::move(uids[1]), *std::move(uids[2]), std::move(context),
                       requestSchema);
}

} // namespace cedar::cli
