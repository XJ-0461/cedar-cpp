// Entities, contexts and entity uids written in the entity JSON format (Rust's `EntityJson::from_entity`
// and `CedarValueJson::from_value`, `cedar-policy-core/src/entities/json/{entities,value}.rs`).

#include "json/JsonReader.hpp"

#include <cedar/Json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <string>
#include <vector>

namespace cedar
{

namespace
{

/// value.rs `RESERVED_KEYS`: a record with one of these keys could read back as an escape.
constexpr std::array<std::string_view, 3> ReservedKeys{"__entity", "__extn", "__expr"};

void AppendTypeAndId(std::string& output, const EntityUid& uid)
{
    output += R"({"type":)";
    detail::AppendJsonString(output, uid.Type().GetName().QualifiedText());
    output += R"(,"id":)";
    detail::AppendJsonString(output, uid.Id().Text());
    output.push_back('}');
}

void AppendExtensionCall(std::string& output, const std::string_view function, const std::string_view argument)
{
    output += R"({"__extn":{"fn":)";
    detail::AppendJsonString(output, function);
    output += R"(,"arg":)";
    detail::AppendJsonString(output, argument);
    output += "}}";
}

/// Appends `value`; fails with the reserved key when a record (at any depth) has one.
[[nodiscard]] auto AppendValue(std::string& output, const Value& value) -> std::expected<void, std::string_view>
{
    switch (value.Kind())
    {
    case ValueKind::Bool: output += value.AsBool() ? "true" : "false"; return {};
    case ValueKind::Long:
    {
        std::array<char, 24> digits{};
        const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value.AsLong());
        output.append(digits.data(), end);
        return {};
    }
    case ValueKind::String: detail::AppendJsonString(output, value.AsString()); return {};
    case ValueKind::Entity:
        output += R"({"__entity":)";
        AppendTypeAndId(output, value.AsEntity());
        output.push_back('}');
        return {};
    case ValueKind::Set:
    {
        output.push_back('[');
        bool first = true;
        for (const Value& element : value.AsSet())
        {
            if (!first) { output.push_back(','); }
            first = false;
            if (auto appended = AppendValue(output, element); !appended) { return appended; }
        }
        output.push_back(']');
        return {};
    }
    case ValueKind::Record:
    {
        const Record record = value.AsRecord();
        for (const RecordEntry& entry : record)
        {
            if (std::ranges::contains(ReservedKeys, entry.key)) { return std::unexpected{entry.key}; }
        }
        output.push_back('{');
        bool first = true;
        for (const RecordEntry& entry : record)
        {
            if (!first) { output.push_back(','); }
            first = false;
            detail::AppendJsonString(output, entry.key);
            output.push_back(':');
            if (auto appended = AppendValue(output, entry.value); !appended) { return appended; }
        }
        output.push_back('}');
        return {};
    }
    case ValueKind::Decimal:
    {
        std::string argument;
        AppendCanonicalString(argument, value.AsDecimal());
        AppendExtensionCall(output, "decimal", argument);
        return {};
    }
    case ValueKind::IpAddress:
    {
        std::string argument;
        AppendCanonicalString(argument, value.AsIpAddress());
        AppendExtensionCall(output, "ip", argument);
        return {};
    }
    case ValueKind::Duration:
    {
        std::string argument;
        AppendCanonicalString(argument, value.AsDuration());
        AppendExtensionCall(output, "duration", argument);
        return {};
    }
    case ValueKind::Datetime:
    {
        // The canonical call `datetime("1970-01-01").offset(duration("<n>ms"))` in the
        // multiple-argument form.
        std::string offset;
        AppendCanonicalString(offset, Duration{value.AsDatetime().millisecondsSinceEpoch});
        output += R"({"__extn":{"fn":"offset","args":[)";
        AppendExtensionCall(output, "datetime", "1970-01-01");
        output.push_back(',');
        AppendExtensionCall(output, "duration", offset);
        output += "]}}";
        return {};
    }
    }
    return {};
}

[[nodiscard]] auto ReservedKeyMessage(const std::string_view key) -> std::string
{
    return std::format("a record has the key `{}`, which the entity JSON format reserves for its escapes", key);
}

/// Appends a map of attributes or tags. Its own keys may be reserved words (Rust checks only the
/// records inside the values).
[[nodiscard]] auto AppendValueMap(std::string& output, const Record record) -> std::expected<void, std::string_view>
{
    output.push_back('{');
    bool first = true;
    for (const RecordEntry& entry : record)
    {
        if (!first) { output.push_back(','); }
        first = false;
        detail::AppendJsonString(output, entry.key);
        output.push_back(':');
        if (auto appended = AppendValue(output, entry.value); !appended) { return appended; }
    }
    output.push_back('}');
    return {};
}

[[nodiscard]] auto AppendEntity(std::string& output, const Entity& entity) -> std::expected<void, EntitiesError>
{
    const auto fail = [&](const std::string_view key) {
        return std::unexpected{EntitiesError{Diagnostic{
            .code = DiagnosticCode::SerializationReservedKey,
            .message = std::format("cannot write `{}`: {}", entity.Uid().ToString(), ReservedKeyMessage(key)),
        }}};
    };
    output += R"({"uid":)";
    AppendTypeAndId(output, entity.Uid());
    output += R"(,"attrs":)";
    if (auto appended = AppendValueMap(output, entity.Attributes()); !appended) { return fail(appended.error()); }
    output += R"(,"parents":[)";
    std::vector<const EntityUid*> ancestors;
    ancestors.reserve(entity.Ancestors().Size());
    for (const EntityUid& ancestor : entity.Ancestors()) { ancestors.push_back(&ancestor); }
    std::ranges::sort(ancestors, [](const EntityUid* left, const EntityUid* right) { return *left < *right; });
    bool first = true;
    for (const EntityUid* ancestor : ancestors)
    {
        if (!first) { output.push_back(','); }
        first = false;
        AppendTypeAndId(output, *ancestor);
    }
    output.push_back(']');
    if (!entity.Tags().IsEmpty())
    {
        output += R"(,"tags":)";
        if (auto appended = AppendValueMap(output, entity.Tags()); !appended) { return fail(appended.error()); }
    }
    output.push_back('}');
    return {};
}

} // namespace

auto ToJson(const Entities& entities) -> std::expected<std::string, EntitiesError>
{
    std::vector<const Entity*> sorted;
    sorted.reserve(entities.Size());
    for (const Entity& entity : entities) { sorted.push_back(&entity); }
    std::ranges::sort(sorted, [](const Entity* left, const Entity* right) { return left->Uid() < right->Uid(); });
    std::string output = "[";
    bool first = true;
    for (const Entity* entity : sorted)
    {
        if (!first) { output.push_back(','); }
        first = false;
        if (auto appended = AppendEntity(output, *entity); !appended) { return std::unexpected{appended.error()}; }
    }
    output.push_back(']');
    return output;
}

auto ToJson(const Entity& entity) -> std::expected<std::string, EntitiesError>
{
    std::string output;
    if (auto appended = AppendEntity(output, entity); !appended) { return std::unexpected{appended.error()}; }
    return output;
}

auto ToJson(const Context& context) -> std::expected<std::string, ContextError>
{
    std::string output;
    if (auto appended = AppendValue(output, context.AsValue()); !appended)
    {
        return std::unexpected{ContextError{Diagnostic{
            .code = DiagnosticCode::SerializationReservedKey,
            .message = std::format("cannot write the context: {}", ReservedKeyMessage(appended.error())),
        }}};
    }
    return output;
}

auto ToJson(const EntityUid& uid) -> std::string
{
    std::string output;
    AppendTypeAndId(output, uid);
    return output;
}

} // namespace cedar
