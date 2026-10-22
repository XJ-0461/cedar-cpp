#include "json/ValueJson.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "validation/Types.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace cedar::detail
{

namespace
{

[[nodiscard]] auto ErrorAt(const JsonNode& node, const DiagnosticCode code, std::string message,
                           std::optional<std::string> help = std::nullopt) -> JsonReadError
{
    return JsonReadError{
        .code = code, .offset = node.offset, .length = node.length, .message = std::move(message), .help = std::move(help)};
}

/// The member `key` of `object` as a struct field reads it: under Keep a key given twice is
/// serde's "duplicate field" and fails the struct (`unique` false); under LastWins the last wins.
struct FieldLookup
{
    const JsonNode* value = nullptr;
    bool unique = false;
};

[[nodiscard]] auto LookupField(const JsonNode& object, const std::string_view key, const DuplicateKeys duplicates)
    -> FieldLookup
{
    FieldLookup found;
    std::size_t count = 0;
    for (const JsonMember& member : object.Members())
    {
        if (member.key != key) { continue; }
        ++count;
        found.value = &member.value;
    }
    found.unique = count == 1 || (count > 1 && duplicates != DuplicateKeys::Keep);
    return found;
}

[[nodiscard]] auto ReadTypeAndId(const JsonNode& node, const DuplicateKeys duplicates) -> std::optional<TypeAndId>
{
    if (node.IsArray())
    {
        const auto elements = node.Elements();
        if (elements.size() != 2 || !elements[0].IsString() || !elements[1].IsString()) { return std::nullopt; }
        return TypeAndId{elements[0].text, elements[1].text, &elements[0]};
    }
    if (!node.IsObject()) { return std::nullopt; }
    const FieldLookup type = LookupField(node, "type", duplicates);
    const FieldLookup id = LookupField(node, "id", duplicates);
    if (!type.unique || !id.unique || !type.value->IsString() || !id.value->IsString()) { return std::nullopt; }
    return TypeAndId{type.value->text, id.value->text, type.value};
}

[[nodiscard]] auto FindEffective(const std::vector<const JsonMember*>& members, const std::string_view key)
    -> const JsonNode*
{
    const auto found = std::ranges::lower_bound(members, key, {}, &JsonMember::key);
    if (found == members.end() || (*found)->key != key) { return nullptr; }
    return &(*found)->value;
}

/// The extension function name Rust reads as its legacy partial-evaluation unknown.
constexpr std::string_view UnknownFunction = "unknown";

/// The refusal of a call to `unknown`, in every form and position a reader meets one.
[[nodiscard]] auto UnknownNotSupported(const JsonNode& node) -> JsonReadError
{
    return ErrorAt(node, DiagnosticCode::UnknownValueNotSupported,
                   "an `unknown` extension value is not supported in JSON input",
                   "partial evaluation takes its unknowns from a partial request and partial entities");
}

/// Whether `node` is the escape Rust's `parse_as_unknown` reads ahead of any expected type: an
/// object holding `{"__extn": {"fn": "unknown", ..}}`, other keys ignored.
[[nodiscard]] auto IsUnknownEscape(const JsonNode& node, DuplicateKeys duplicates) -> bool;

[[nodiscard]] auto ConvertCedarValue(const JsonNode& node, const DuplicateKeys duplicates, ValueArena& arena,
                                     const EntityTypeNames names) -> std::expected<Value, JsonReadError>
{
    switch (node.kind)
    {
    case JsonKind::Null:
        return std::unexpected{ErrorAt(node, DiagnosticCode::NullValue, "found a JSON `null`, which is not a Cedar value")};
    case JsonKind::False: return Value::FromBool(false);
    case JsonKind::True: return Value::FromBool(true);
    case JsonKind::Number: return Value::FromLong(ClassifyJsonNumber(node.text).value);
    case JsonKind::String: return arena.MakeString(node.text);
    case JsonKind::Array:
    {
        const std::span<Value> elements = arena.AllocateValues(node.count);
        for (std::size_t index = 0; index < node.count; ++index)
        {
            auto element = ConvertCedarValue(node.Elements()[index], duplicates, arena, names);
            if (!element) { return element; }
            elements[index] = *element;
        }
        return arena.AdoptSet(elements);
    }
    case JsonKind::Object: break;
    }

    const CedarValueEscape escape = ClassifyCedarValueEscape(node, duplicates);
    switch (escape.kind)
    {
    case CedarValueEscapeKind::Expression:
        return std::unexpected{ErrorAt(
            node, DiagnosticCode::ExpressionTag, "the `__expr` escape is not supported",
            "write an entity reference with `__entity`, an extension value with `__extn`, and any other value as "
            "plain JSON")};
    case CedarValueEscapeKind::Entity:
    {
        auto uid = MakeEntityUid(escape.entity, names);
        if (!uid) { return std::unexpected{std::move(uid).error()}; }
        return arena.MakeEntity(*uid);
    }
    case CedarValueEscapeKind::Extension:
    {
        const std::string_view function = escape.function->text;
        if (!CheckName(function))
        {
            return std::unexpected{ErrorAt(*escape.function, DiagnosticCode::InvalidEscapeValue,
                                           std::format("`{}` is not a valid extension function name", function))};
        }
        if (function == UnknownFunction) { return std::unexpected{UnknownNotSupported(node)}; }
        std::vector<Value> arguments;
        const auto read = [&](const JsonNode& argument) -> std::expected<void, JsonReadError> {
            auto value = ConvertCedarValue(argument, duplicates, arena, names);
            if (!value) { return std::unexpected{std::move(value).error()}; }
            arguments.push_back(*value);
            return {};
        };
        if (escape.argument != nullptr)
        {
            if (auto done = read(*escape.argument); !done) { return std::unexpected{std::move(done).error()}; }
        }
        else
        {
            for (const JsonNode& argument : escape.arguments->Elements())
            {
                if (auto done = read(argument); !done) { return std::unexpected{std::move(done).error()}; }
            }
        }
        auto result = CallExtensionFunction(function, arguments, arena);
        if (!result)
        {
            const EvaluationError& error = result.error();
            std::optional<std::string> help;
            if (const auto text = error.Help()) { help = std::string{*text}; }
            JsonReadError failed = ErrorAt(node, error.Code(), std::string{error.Message()}, std::move(help));
            failed.evaluationFailed = true;
            return std::unexpected{std::move(failed)};
        }
        return *result;
    }
    case CedarValueEscapeKind::None: break;
    }

    std::vector<const JsonMember*> members;
    EffectiveMembers(node, duplicates, members);
    const std::span<RecordEntry> entries = arena.AllocateRecordEntries(members.size());
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        auto value = ConvertCedarValue(members[index]->value, duplicates, arena, names);
        if (!value) { return value; }
        entries[index] = RecordEntry{arena.CopyText(members[index]->key), *value};
    }
    auto record = arena.AdoptRecord(entries);
    if (!record)
    {
        return std::unexpected{ErrorAt(node, DiagnosticCode::DuplicateKey,
                                       std::format("the key `{}` appears more than once", record.error().key))};
    }
    return *record;
}

void AppendDescription(std::string& output, const JsonNode& node, const std::size_t limit)
{
    if (output.size() > limit) { return; }
    switch (node.kind)
    {
    case JsonKind::Null: output += "null"; return;
    case JsonKind::False: output += "false"; return;
    case JsonKind::True: output += "true"; return;
    case JsonKind::Number: output += node.text; return;
    case JsonKind::String: AppendJsonString(output, node.text); return;
    case JsonKind::Array:
        output.push_back('[');
        for (std::size_t index = 0; index < node.count; ++index)
        {
            if (index > 0) { output.push_back(','); }
            AppendDescription(output, node.Elements()[index], limit);
            if (output.size() > limit) { return; }
        }
        output.push_back(']');
        return;
    case JsonKind::Object:
        output.push_back('{');
        for (std::size_t index = 0; index < node.count; ++index)
        {
            if (index > 0) { output.push_back(','); }
            AppendJsonString(output, node.Members()[index].key);
            output.push_back(':');
            AppendDescription(output, node.Members()[index].value, limit);
            if (output.size() > limit) { return; }
        }
        output.push_back('}');
        return;
    }
}

} // namespace

auto MakeEntityUid(const TypeAndId& parts, const EntityTypeNames names) -> std::expected<EntityUid, JsonReadError>
{
    if (names == EntityTypeNames::Existing && CheckName(parts.type))
    {
        auto existing = EntityType::Find(parts.type);
        if (!existing)
        {
            return std::unexpected{ErrorAt(*parts.typeNode, DiagnosticCode::UnexpectedEntityType,
                                           std::format("entity type `{}` is not declared in the schema", parts.type))};
        }
        return EntityUid{*std::move(existing), Eid{parts.id}};
    }
    auto type = EntityType::Parse(parts.type);
    if (!type)
    {
        return std::unexpected{ErrorAt(*parts.typeNode, DiagnosticCode::InvalidEscapeValue,
                                       std::format("`{}` is not a valid entity type name", parts.type))};
    }
    return EntityUid{*std::move(type), Eid{parts.id}};
}

auto ClassifyCedarValueEscape(const JsonNode& object, const DuplicateKeys duplicates) -> CedarValueEscape
{
    std::vector<const JsonMember*> members;
    EffectiveMembers(object, duplicates, members);
    if (members.size() != 1) { return {}; }
    const std::string_view key = members.front()->key;
    const JsonNode& inner = members.front()->value;
    if (key == "__expr" && inner.IsString()) { return CedarValueEscape{.kind = CedarValueEscapeKind::Expression}; }
    if (!inner.IsObject() || (key != "__extn" && key != "__entity")) { return {}; }
    std::vector<const JsonMember*> fields;
    EffectiveMembers(inner, duplicates, fields);
    if (fields.size() < 2) { return {}; }
    if (key == "__extn")
    {
        const JsonNode* function = FindEffective(fields, "fn");
        if (function == nullptr || !function->IsString()) { return {}; }
        if (const JsonNode* argument = FindEffective(fields, "arg"))
        {
            return CedarValueEscape{.kind = CedarValueEscapeKind::Extension, .function = function, .argument = argument};
        }
        if (const JsonNode* arguments = FindEffective(fields, "args"); arguments != nullptr && arguments->IsArray())
        {
            return CedarValueEscape{.kind = CedarValueEscapeKind::Extension, .function = function, .arguments = arguments};
        }
        return {};
    }
    const JsonNode* type = FindEffective(fields, "type");
    const JsonNode* id = FindEffective(fields, "id");
    if (type == nullptr || id == nullptr || !type->IsString() || !id->IsString()) { return {}; }
    return CedarValueEscape{.kind = CedarValueEscapeKind::Entity, .entity = TypeAndId{type->text, id->text, type}};
}

auto CheckCedarNumbers(const JsonNode& node, const DuplicateKeys duplicates)
    -> std::expected<void, JsonReadError>
{
    switch (node.kind)
    {
    case JsonKind::Number:
        if (ClassifyJsonNumber(node.text).kind != JsonNumberKind::Long)
        {
            return std::unexpected{ErrorAt(node, DiagnosticCode::JsonUnexpectedShape,
                                           std::format("`{}` is not a Cedar value: numbers must be integers that "
                                                       "fit in 64 bits",
                                                       node.text))};
        }
        return {};
    case JsonKind::Array:
        for (const JsonNode& element : node.Elements())
        {
            if (auto checked = CheckCedarNumbers(element, duplicates); !checked) { return checked; }
        }
        return {};
    case JsonKind::Object:
    {
        std::vector<const JsonMember*> members;
        EffectiveMembers(node, duplicates, members);
        for (const JsonMember* member : members)
        {
            if (auto checked = CheckCedarNumbers(member->value, duplicates); !checked) { return checked; }
        }
        return {};
    }
    default: return {};
    }
}

auto CheckStrictJson(const JsonNode& node, const DuplicateKeys duplicates) -> std::expected<void, JsonReadError>
{
    switch (node.kind)
    {
    case JsonKind::Number:
        if (ClassifyJsonNumber(node.text).kind == JsonNumberKind::OutOfRange)
        {
            return std::unexpected{
                ErrorAt(node, DiagnosticCode::JsonSyntax, std::format("the number `{}` is out of range", node.text))};
        }
        return {};
    case JsonKind::String:
        if (!node.strictString)
        {
            return std::unexpected{
                ErrorAt(node, DiagnosticCode::JsonSyntax, "a `\\u` escape in the string is an unpaired surrogate")};
        }
        return {};
    case JsonKind::Array:
    case JsonKind::Object:
        if (node.depth > MaximumJsonDepth)
        {
            return std::unexpected{ErrorAt(node, DiagnosticCode::JsonSyntax,
                                           std::format("the JSON nests more than {} levels deep", MaximumJsonDepth))};
        }
        break;
    default: return {};
    }
    if (node.IsArray())
    {
        for (const JsonNode& element : node.Elements())
        {
            if (auto checked = CheckStrictJson(element, duplicates); !checked) { return checked; }
        }
        return {};
    }
    for (const JsonMember& member : node.Members())
    {
        if (!member.strictKey)
        {
            return std::unexpected{JsonReadError{.code = DiagnosticCode::JsonSyntax,
                                                 .offset = member.keyOffset,
                                                 .length = 1,
                                                 .message = "a `\\u` escape in the key is an unpaired surrogate"}};
        }
        if (auto checked = CheckStrictJson(member.value, duplicates); !checked) { return checked; }
    }
    if (duplicates == DuplicateKeys::Reject && node.count > 1)
    {
        std::vector<const JsonMember*> members;
        members.reserve(node.count);
        for (const JsonMember& member : node.Members()) { members.push_back(&member); }
        std::ranges::stable_sort(members, {}, &JsonMember::key);
        const auto repeated = std::ranges::adjacent_find(members, {}, &JsonMember::key);
        if (repeated != members.end())
        {
            const JsonMember& second = **std::next(repeated);
            return std::unexpected{JsonReadError{
                .code = DiagnosticCode::DuplicateKey,
                .offset = second.keyOffset,
                .length = static_cast<std::uint32_t>(second.key.size()),
                .message = std::format("the key `{}` appears more than once in the same object", second.key)}};
        }
    }
    return {};
}

void EffectiveMembers(const JsonNode& object, const DuplicateKeys duplicates, std::vector<const JsonMember*>& members)
{
    members.clear();
    members.reserve(object.count);
    for (const JsonMember& member : object.Members()) { members.push_back(&member); }
    std::ranges::stable_sort(members, {}, &JsonMember::key);
    if (duplicates != DuplicateKeys::LastWins) { return; }
    // Within a run of equal keys the stable sort kept document order; keep the run's last member.
    std::size_t kept = 0;
    for (std::size_t index = 0; index < members.size(); ++index)
    {
        if (index + 1 < members.size() && members[index + 1]->key == members[index]->key) { continue; }
        members[kept++] = members[index];
    }
    members.resize(kept);
}

auto ReadCedarValue(const JsonNode& node, const DuplicateKeys duplicates, ValueArena& arena,
                    const EntityTypeNames names) -> std::expected<Value, JsonReadError>
{
    if (IsUnknownEscape(node, duplicates)) { return std::unexpected{UnknownNotSupported(node)}; }
    if (auto checked = CheckCedarNumbers(node, duplicates); !checked) { return std::unexpected{checked.error()}; }
    return ConvertCedarValue(node, duplicates, arena, names);
}

auto IsCedarRecord(const JsonNode& node, const DuplicateKeys duplicates) -> bool
{
    return node.IsObject() && ClassifyCedarValueEscape(node, duplicates).kind == CedarValueEscapeKind::None;
}

auto ReadEntityUidJson(const JsonNode& node, const DuplicateKeys duplicates, const EntityTypeNames names)
    -> std::expected<EntityUid, JsonReadError>
{
    // Rust's untagged `EntityUidJson` tries its variants in order: an `__expr` escape (refused),
    // an explicit `__entity` escape, the implicit `{type, id}`, and finally any other value
    // (refused). Its two struct variants read only objects; `TypeAndId` also reads a sequence.
    if (node.IsObject())
    {
        if (const FieldLookup expression = LookupField(node, "__expr", duplicates);
            expression.unique && expression.value->IsString())
        {
            return std::unexpected{ErrorAt(
                node, DiagnosticCode::ExpressionTag, "the `__expr` escape is not supported",
                "write an entity reference as `{\"type\": \"SomeType\", \"id\": \"SomeId\"}`")};
        }
        if (const FieldLookup entity = LookupField(node, "__entity", duplicates); entity.unique)
        {
            if (const auto parts = ReadTypeAndId(*entity.value, duplicates)) { return MakeEntityUid(*parts, names); }
        }
    }
    if (const auto parts = ReadTypeAndId(node, duplicates)) { return MakeEntityUid(*parts, names); }
    return std::unexpected{
        ErrorAt(node, DiagnosticCode::ExpectedLiteralEntityReference,
                std::format("expected a literal entity reference, but got `{}`", DescribeJson(node)),
                "write an entity reference as `{\"type\": \"SomeType\", \"id\": \"SomeId\"}`")};
}

namespace
{

// Schema-typed reading (Rust's `ValueParser::val_into_restricted_expr` with an expected type,
// `cedar-policy-core/src/entities/json/value.rs:715`). Rust reads each part from a
// `serde_json::Value` into a type chosen by the expected type, so unlike a plain Cedar value only
// the parts read as `CedarValueJson` must hold `i64` numbers, and keys outside the derived
// structs are ignored.

/// The member `key` of `object` as a struct field reads it from a `serde_json::Value`, or null.
[[nodiscard]] auto ValueField(const JsonNode& object, const std::string_view key, const DuplicateKeys duplicates)
    -> const JsonNode*
{
    const FieldLookup found = LookupField(object, key, duplicates);
    return found.unique ? found.value : nullptr;
}

/// Whether serde reads `node` as a `CedarValueJson` at all: every number in it an `i64`.
[[nodiscard]] auto IsCedarValueJson(const JsonNode& node, const DuplicateKeys duplicates) -> bool
{
    return CheckCedarNumbers(node, duplicates).has_value();
}

/// An extension function call as Rust's untagged `FnAndArgs` reads it.
struct FunctionAndArguments
{
    const JsonNode* function;
    std::vector<const JsonNode*> arguments;
};

/// `FnAndArgs` from `node`: `Single {fn, arg}` first, then `Multi {fn, args}`, each from an object
/// with other keys ignored (an untagged enum's struct variants read no sequence form). The
/// arguments must each read as a `CedarValueJson`.
[[nodiscard]] auto ReadFunctionAndArguments(const JsonNode& node, const DuplicateKeys duplicates)
    -> std::optional<FunctionAndArguments>
{
    if (!node.IsObject()) { return std::nullopt; }
    const JsonNode* function = ValueField(node, "fn", duplicates);
    const JsonNode* single = ValueField(node, "arg", duplicates);
    const JsonNode* multiple = ValueField(node, "args", duplicates);
    if (function == nullptr || !function->IsString()) { return std::nullopt; }
    if (single != nullptr && IsCedarValueJson(*single, duplicates))
    {
        return FunctionAndArguments{.function = function, .arguments = {single}};
    }
    if (multiple != nullptr && multiple->IsArray() &&
        std::ranges::all_of(multiple->Elements(),
                            [&](const JsonNode& argument) { return IsCedarValueJson(argument, duplicates); }))
    {
        FunctionAndArguments call{.function = function, .arguments = {}};
        for (const JsonNode& argument : multiple->Elements()) { call.arguments.push_back(&argument); }
        return call;
    }
    return std::nullopt;
}

auto IsUnknownEscape(const JsonNode& node, const DuplicateKeys duplicates) -> bool
{
    if (!node.IsObject()) { return false; }
    // Rust's untagged `ExtnValueJson` tries the `__expr` escape first.
    if (const JsonNode* expression = ValueField(node, "__expr", duplicates);
        expression != nullptr && expression->IsString())
    {
        return false;
    }
    const JsonNode* escaped = ValueField(node, "__extn", duplicates);
    if (escaped == nullptr) { return false; }
    const auto call = ReadFunctionAndArguments(*escaped, duplicates);
    return call && call->function->text == UnknownFunction;
}

[[nodiscard]] auto ReadTypedExtension(const JsonNode& node, ValueKind kind, DuplicateKeys duplicates, ValueArena& arena,
                                      EntityTypeNames names) -> std::expected<Value, JsonReadError>;

/// Calls `function` on the arguments, each read by the type the function declares for it.
[[nodiscard]] auto CallTyped(const JsonNode& node, const JsonNode& function,
                             const std::span<const JsonNode* const> arguments, const DuplicateKeys duplicates,
                             ValueArena& arena, const EntityTypeNames names) -> std::expected<Value, JsonReadError>
{
    const std::string_view name = function.text;
    if (!CheckName(name))
    {
        return std::unexpected{ErrorAt(function, DiagnosticCode::InvalidEscapeValue,
                                       std::format("`{}` is not a valid extension function name", name))};
    }
    if (name == UnknownFunction) { return std::unexpected{UnknownNotSupported(node)}; }
    const ExtensionCall* call = FindExtensionCall(name);
    if (call == nullptr)
    {
        return std::unexpected{ErrorAt(function, DiagnosticCode::ExtensionFunctionLookup,
                                       std::format("`{}` is not an extension function", name))};
    }
    if (arguments.size() != call->arity)
    {
        return std::unexpected{ErrorAt(node, DiagnosticCode::IncorrectArgumentCount,
                                       std::format("`{}` takes {} argument{}, not {}", name, call->arity,
                                                   call->arity == 1 ? "" : "s", arguments.size()))};
    }
    std::vector<Value> values;
    values.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const ValueKind argumentKind = call->argumentKinds[std::min<std::size_t>(index, 1)];
        auto value = std::ranges::contains(ExtensionTypes, argumentKind)
                         ? ReadTypedExtension(*arguments[index], argumentKind, duplicates, arena, names)
                         : ReadCedarValue(*arguments[index], duplicates, arena, names);
        if (!value) { return value; }
        values.push_back(*value);
    }
    auto result = CallExtensionFunction(name, values, arena);
    if (!result)
    {
        const EvaluationError& error = result.error();
        std::optional<std::string> help;
        if (const auto text = error.Help()) { help = std::string{*text}; }
        JsonReadError failed = ErrorAt(node, error.Code(), std::string{error.Message()}, std::move(help));
        failed.evaluationFailed = true;
        return std::unexpected{std::move(failed)};
    }
    return *result;
}

/// A value of the extension type `kind` (Rust's untagged `ExtnValueJson`, tried in order): an
/// `__expr` escape (refused), an explicit `{"__extn": {...}}`, the call without the escape
/// (`{"fn": .., "arg": ..}`), and last any Cedar value, passed to the type's one-argument
/// constructor. The escape forms are objects whose other keys are ignored.
auto ReadTypedExtension(const JsonNode& node, const ValueKind kind, const DuplicateKeys duplicates, ValueArena& arena,
                        const EntityTypeNames names) -> std::expected<Value, JsonReadError>
{
    const JsonNode* escaped = nullptr;
    if (node.IsObject())
    {
        if (const JsonNode* expression = ValueField(node, "__expr", duplicates);
            expression != nullptr && expression->IsString())
        {
            return std::unexpected{ErrorAt(node, DiagnosticCode::ExpressionTag, "the `__expr` escape is not supported",
                                           "write an extension value with `__extn`, or as the constructor's argument")};
        }
        escaped = ValueField(node, "__extn", duplicates);
    }
    if (escaped != nullptr)
    {
        if (const auto call = ReadFunctionAndArguments(*escaped, duplicates))
        {
            return CallTyped(node, *call->function, call->arguments, duplicates, arena, names);
        }
    }
    if (const auto call = ReadFunctionAndArguments(node, duplicates))
    {
        return CallTyped(node, *call->function, call->arguments, duplicates, arena, names);
    }

    // `ImplicitConstructor`: the one function taking a string and returning `kind`.
    const auto constructor = std::ranges::find_if(ExtensionCalls, [&](const ExtensionCall& call) {
        return call.arity == 1 && call.argumentKinds[0] == ValueKind::String && call.returnKind == kind;
    });
    if (constructor == ExtensionCalls.end())
    {
        return std::unexpected{ErrorAt(node, DiagnosticCode::MissingImpliedConstructor,
                                       std::format("no constructor makes a `{}` from one string", ExtensionTypeName(kind)))};
    }
    auto argument = ReadCedarValue(node, duplicates, arena, names);
    if (!argument) { return argument; }
    auto result = CallExtensionFunction(constructor->name, std::span{&*argument, 1}, arena);
    if (!result)
    {
        const EvaluationError& error = result.error();
        std::optional<std::string> help;
        if (const auto text = error.Help()) { help = std::string{*text}; }
        JsonReadError failed = ErrorAt(node, error.Code(), std::string{error.Message()}, std::move(help));
        failed.evaluationFailed = true;
        return std::unexpected{std::move(failed)};
    }
    return *result;
}

[[nodiscard]] auto TypeMismatch(const JsonNode& node, const Type& expected) -> JsonReadError
{
    return ErrorAt(node, DiagnosticCode::JsonTypeMismatch,
                   std::format("expected a value of type `{}`, but got `{}`", ToString(expected), DescribeJson(node)));
}

} // namespace

auto ReadTypedCedarValue(const JsonNode& node, const Type& expected, const DuplicateKeys duplicates, ValueArena& arena,
                         const EntityTypeNames names) -> std::expected<Value, JsonReadError>
{
    if (IsUnknownEscape(node, duplicates)) { return std::unexpected{UnknownNotSupported(node)}; }
    if (std::holds_alternative<types::Entity>(expected.kind))
    {
        auto uid = ReadEntityUidJson(node, duplicates, names);
        if (!uid) { return std::unexpected{std::move(uid).error()}; }
        return arena.MakeEntity(*uid);
    }
    if (const auto* extension = std::get_if<types::Extension>(&expected.kind))
    {
        return ReadTypedExtension(node, extension->kind, duplicates, arena, names);
    }
    if (const auto* set = std::get_if<types::Set>(&expected.kind); set != nullptr && set->element != nullptr)
    {
        if (!node.IsArray()) { return std::unexpected{TypeMismatch(node, expected)}; }
        const std::span<Value> elements = arena.AllocateValues(node.count);
        for (std::size_t index = 0; index < node.count; ++index)
        {
            auto element = ReadTypedCedarValue(node.Elements()[index], *set->element, duplicates, arena, names);
            if (!element) { return element; }
            elements[index] = *element;
        }
        return arena.AdoptSet(elements);
    }
    if (const auto* record = AsRecord(expected))
    {
        if (!node.IsObject()) { return std::unexpected{TypeMismatch(node, expected)}; }
        std::vector<const JsonMember*> members;
        EffectiveMembers(node, duplicates, members);
        // Both lists are sorted by key: walk them together.
        const std::span<RecordEntry> entries = arena.AllocateRecordEntries(members.size());
        std::size_t filled = 0;
        std::size_t next = 0;
        for (const Attribute& attribute : record->attributes)
        {
            while (next < members.size() && members[next]->key < attribute.name)
            {
                if (record->open == OpenTag::ClosedAttributes)
                {
                    return std::unexpected{ErrorAt(members[next]->value, DiagnosticCode::UnexpectedRecordAttribute,
                                                   std::format("the record attribute `{}` is not declared in `{}`",
                                                               members[next]->key, ToString(expected)))};
                }
                ++next;
            }
            if (next == members.size() || members[next]->key != attribute.name)
            {
                if (attribute.type.required)
                {
                    return std::unexpected{
                        ErrorAt(node, DiagnosticCode::MissingRequiredRecordAttribute,
                                std::format("the required record attribute `{}` is missing", attribute.name))};
                }
                continue;
            }
            auto value = ReadTypedCedarValue(members[next]->value, *attribute.type.type, duplicates, arena, names);
            if (!value)
            {
                JsonReadError error = std::move(value).error();
                error.message = std::format("in attribute `{}`: {}", attribute.name, error.message);
                return std::unexpected{std::move(error)};
            }
            entries[filled++] = RecordEntry{arena.CopyText(members[next]->key), *value};
            ++next;
        }
        if (next < members.size() && record->open == OpenTag::ClosedAttributes)
        {
            return std::unexpected{ErrorAt(members[next]->value, DiagnosticCode::UnexpectedRecordAttribute,
                                           std::format("the record attribute `{}` is not declared in `{}`",
                                                       members[next]->key, ToString(expected)))};
        }
        auto built = arena.AdoptRecord(entries.first(filled));
        if (!built)
        {
            return std::unexpected{ErrorAt(node, DiagnosticCode::DuplicateKey,
                                           std::format("the key `{}` appears more than once", built.error().key))};
        }
        return *built;
    }
    return ReadCedarValue(node, duplicates, arena, names);
}

auto FromSyntaxError(JsonSyntaxError error) -> JsonReadError
{
    return JsonReadError{
        .code = DiagnosticCode::JsonSyntax, .offset = error.offset, .length = 1, .message = std::move(error.message)};
}

auto ToDiagnostic(JsonReadError error, const SharedSourceText& source, const std::string_view context) -> Diagnostic
{
    std::string message = context.empty() ? std::move(error.message) : std::format("{}: {}", context, error.message);
    const std::uint32_t length = error.length == 0 ? 1 : error.length;
    return Diagnostic{
        .code = error.code,
        .message = std::move(message),
        .help = std::move(error.help),
        .primary = SourceSpan{error.offset, error.offset + length},
        .source = source,
    };
}

auto DescribeJson(const JsonNode& node) -> std::string
{
    constexpr std::size_t Limit = 80;
    std::string output;
    AppendDescription(output, node, Limit);
    if (output.size() > Limit)
    {
        output.resize(Limit);
        // Cut at a character boundary: drop the continuation bytes of a split UTF-8 sequence and its lead byte.
        while (!output.empty() && (static_cast<unsigned char>(output.back()) & 0xC0) == 0x80) { output.pop_back(); }
        if (!output.empty() && static_cast<unsigned char>(output.back()) >= 0xC0) { output.pop_back(); }
        output += "...";
    }
    return output;
}

} // namespace cedar::detail
