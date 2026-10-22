// cedar-cpp-partial-evaluation-runner: cedar-cpp's type-aware partial evaluation, driven by a
// manifest of cases and answered as canonical JSON lines. It speaks the contract written at the top
// of `tests/parity/partial-evaluation-oracle/src/main.rs` and must write the oracle's lines byte for
// byte: the same manifest keys and steps, the same canonical JSON, the same residual tree encoding,
// and errors as the oracle's Rust variant paths.
//
// `cedar-cpp-partial-evaluation-runner <manifest.jsonl>`. Exit status 0 when the manifest was read,
// 2 for a usage error or an unreadable manifest, with nothing written.
//
// Error paths come from one table keyed by diagnostic code (ErrorPaths). A code gives the Rust enum
// that holds the variant and the variant's segment; the API that raised the error decides how that
// variant nests under its own enum, as the oracle's `*_error` functions do. Message text is never
// read.
//
// cedar-cpp's JSON readers refuse a legacy `unknown` extension value, which Rust accepts and then
// refuses later with an error of its own (an accepted divergence). Where a reader refuses with
// UnknownValueNotSupported, the runner reports Rust's error for that input: a partial or
// query request's context reports `partial-request-creation.context-contains-unknowns`, and the
// concrete entities of a principal or resource query report
// `permission-query.entities.partial-value-to-value.contains-unknown`. Partial entities report
// Rust's `entities.deserialization.restricted-expression-evaluation` through ErrorPaths, as Rust
// fails there evaluating the parsed value. Every other input keeps its `input.*` path.

#include <json/JsonReader.hpp>

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/ExtensionValues.hpp>
#include <cedar/Json.hpp>
#include <cedar/PartialEvaluation.hpp>
#include <cedar/Policy.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>
#include <cedar/Syntax.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using cedar::DiagnosticCode;
using cedar::detail::JsonKind;
using cedar::detail::JsonMember;
using cedar::detail::JsonNode;

template <typename T>
using Outcome = std::expected<T, std::string>;

[[nodiscard]] auto Fail(std::string_view path) -> std::unexpected<std::string>
{
    return std::unexpected{std::string{path}};
}

// ---- Output JSON ----

/// A JSON value to be written canonically: object keys sorted by their UTF-8 bytes, no whitespace,
/// strings escaped as serde_json escapes them.
struct OutputJson
{
    enum class Kind : std::uint8_t
    {
        Null,
        Bool,
        Integer,
        String,
        Array,
        Object,
    };

    Kind kind{Kind::Null};
    bool boolean{};
    std::int64_t integer{};
    std::string text{};
    std::vector<OutputJson> elements{};
    std::vector<std::pair<std::string, OutputJson>> members{};

    [[nodiscard]] static auto Null() -> OutputJson { return {}; }
    [[nodiscard]] static auto Bool(const bool value) -> OutputJson
    {
        return OutputJson{.kind = Kind::Bool, .boolean = value};
    }
    [[nodiscard]] static auto Integer(const std::int64_t value) -> OutputJson
    {
        return OutputJson{.kind = Kind::Integer, .integer = value};
    }
    [[nodiscard]] static auto String(const std::string_view value) -> OutputJson
    {
        return OutputJson{.kind = Kind::String, .text = std::string{value}};
    }
    [[nodiscard]] static auto Array(std::vector<OutputJson> values = {}) -> OutputJson
    {
        return OutputJson{.kind = Kind::Array, .elements = std::move(values)};
    }
    [[nodiscard]] static auto Object() -> OutputJson { return OutputJson{.kind = Kind::Object}; }

    auto Add(std::string key, OutputJson value) -> OutputJson&
    {
        members.emplace_back(std::move(key), std::move(value));
        return *this;
    }
};

void WriteCanonical(const OutputJson& value, std::string& output)
{
    switch (value.kind)
    {
    case OutputJson::Kind::Null: output += "null"; return;
    case OutputJson::Kind::Bool: output += value.boolean ? "true" : "false"; return;
    case OutputJson::Kind::Integer:
    {
        std::array<char, 24> digits{};
        const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value.integer);
        output.append(digits.data(), end);
        return;
    }
    case OutputJson::Kind::String: cedar::detail::AppendJsonString(output, value.text); return;
    case OutputJson::Kind::Array:
    {
        output.push_back('[');
        for (std::size_t index = 0; index < value.elements.size(); ++index)
        {
            if (index > 0) { output.push_back(','); }
            WriteCanonical(value.elements[index], output);
        }
        output.push_back(']');
        return;
    }
    case OutputJson::Kind::Object:
    {
        std::vector<const std::pair<std::string, OutputJson>*> sorted;
        sorted.reserve(value.members.size());
        for (const auto& member : value.members) { sorted.push_back(&member); }
        // std::string compares as unsigned bytes through char_traits<char>::compare.
        std::ranges::stable_sort(sorted, {}, [](const auto* member) -> std::string_view { return member->first; });
        output.push_back('{');
        for (std::size_t index = 0; index < sorted.size(); ++index)
        {
            if (index > 0) { output.push_back(','); }
            cedar::detail::AppendJsonString(output, sorted[index]->first);
            output.push_back(':');
            WriteCanonical(sorted[index]->second, output);
        }
        output.push_back('}');
        return;
    }
    }
}

// ---- Error paths ----

/// The Rust enum that holds a variant, which decides how the variant nests under the error enum of
/// the API that raised it.
enum class ErrorFamily : std::uint8_t
{
    RequestValidation,       ///< `RequestValidationError`
    EntitySchemaConformance, ///< `EntitySchemaConformanceError`
    EntitiesDeserialization, ///< tpe's `JsonDeserializationError`
    EntityValidation,        ///< tpe's `EntityValidationError`, other than `Concrete`
    Entities,                ///< tpe's `EntitiesError`, a variant with no enum payload
    TransitiveClosure,       ///< `TcError`
    Tpe,                     ///< `TpeError`, a variant with no enum payload
    ExpressionToResidual,    ///< `ExprToResidualError`
    EntitiesConsistency,     ///< `EntitiesConsistencyError`, other than `InconsistentEntity`
    EntityConsistency,       ///< `EntityConsistencyError`
    RequestConsistency,      ///< `RequestConsistencyError`
    BatchedEvaluation,       ///< `BatchedEvalError`, a variant with no enum payload
};

struct ErrorPath
{
    DiagnosticCode code;
    ErrorFamily family;
    std::string_view variant; ///< The variant's path segment, as the oracle's `segment` writes it.
};

using enum ErrorFamily;

/// Every cedar-cpp diagnostic code that a partial-evaluation API reports, with the Rust variant it
/// stands for. A code missing here is written as `unmapped(<code name>)`, which no oracle line holds.
constexpr std::array ErrorPaths{
    ErrorPath{DiagnosticCode::RequestUndeclaredAction, RequestValidation, "undeclared-action"},
    ErrorPath{DiagnosticCode::RequestExternalAction, RequestValidation, "external-action"},
    ErrorPath{DiagnosticCode::UndeclaredPrincipalType, RequestValidation, "undeclared-principal-type"},
    ErrorPath{DiagnosticCode::UndeclaredResourceType, RequestValidation, "undeclared-resource-type"},
    ErrorPath{DiagnosticCode::InvalidPrincipalType, RequestValidation, "invalid-principal-type"},
    ErrorPath{DiagnosticCode::InvalidResourceType, RequestValidation, "invalid-resource-type"},
    ErrorPath{DiagnosticCode::InvalidContext, RequestValidation, "invalid-context"},
    ErrorPath{DiagnosticCode::TypeOfContext, RequestValidation, "type-of-context"},
    ErrorPath{DiagnosticCode::RequestInvalidEnumEntity, RequestValidation, "invalid-enum-entity"},

    ErrorPath{DiagnosticCode::UnexpectedEntityAttribute, EntitySchemaConformance, "unexpected-entity-attribute"},
    ErrorPath{DiagnosticCode::UnexpectedEntityTag, EntitySchemaConformance, "unexpected-entity-tag"},
    ErrorPath{DiagnosticCode::MissingRequiredEntityAttribute, EntitySchemaConformance,
              "missing-required-entity-attribute"},
    ErrorPath{DiagnosticCode::TypeMismatch, EntitySchemaConformance, "type-mismatch"},
    ErrorPath{DiagnosticCode::InvalidAncestorType, EntitySchemaConformance, "invalid-ancestor-type"},
    ErrorPath{DiagnosticCode::UnexpectedEntityType, EntitySchemaConformance, "unexpected-entity-type"},
    ErrorPath{DiagnosticCode::ExternalEntityType, EntitySchemaConformance, "external-entity-type"},
    ErrorPath{DiagnosticCode::UndeclaredAction, EntitySchemaConformance, "undeclared-action"},
    ErrorPath{DiagnosticCode::ActionDeclarationMismatch, EntitySchemaConformance, "action-declaration-mismatch"},
    ErrorPath{DiagnosticCode::InvalidEnumEntity, EntitySchemaConformance, "invalid-enum-entity"},

    ErrorPath{DiagnosticCode::JsonSyntax, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::JsonUnexpectedShape, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ReservedIdentifier, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::InvalidIdentifier, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ReservedNamespace, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::InvalidEscapeValue, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::InvalidRestrictedExpression, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ExpectedLiteralEntityReference, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ExpectedExtensionValue, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::MissingImpliedConstructor, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::IncorrectArgumentCount, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ExtensionFunctionLookup, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::DuplicateKey, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::UnexpectedRecordAttribute, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::MissingRequiredRecordAttribute, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ExpressionTag, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::NullValue, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::ReservedName, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::UnsupportedEntityTags, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::JsonTypeMismatch, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::UndeclaredTypeWithData, EntitiesDeserialization, "concrete"},
    ErrorPath{DiagnosticCode::UnexpectedAction, EntitiesDeserialization, "unexpected-action"},
    ErrorPath{DiagnosticCode::EntityAttributeEvaluation, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::EntityDoesNotExist, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::EntityAttributeDoesNotExist, EntitiesDeserialization,
              "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::RecordAttributeDoesNotExist, EntitiesDeserialization,
              "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::EntityTagDoesNotExist, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::ExtensionFunctionLookupFailed, EntitiesDeserialization,
              "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::EvaluationTypeError, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::WrongArgumentCount, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::IntegerOverflow, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::UnlinkedSlot, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::ExtensionFunctionExecutionFailed, EntitiesDeserialization,
              "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::RecursionLimit, EntitiesDeserialization, "restricted-expression-evaluation"},
    ErrorPath{DiagnosticCode::UnknownValueNotSupported, EntitiesDeserialization, "restricted-expression-evaluation"},

    ErrorPath{DiagnosticCode::MismatchedActionAncestors, EntityValidation, "mismatched-action-ancestors"},
    ErrorPath{DiagnosticCode::AncestorValidation, Entities, "ancestor-validation"},
    ErrorPath{DiagnosticCode::DuplicateEntity, Entities, "duplicate"},
    ErrorPath{DiagnosticCode::TransitiveClosureMissingEdge, TransitiveClosure, "missing-transitive-closure-edge"},
    ErrorPath{DiagnosticCode::TransitiveClosureCycle, TransitiveClosure, "has-cycle"},

    ErrorPath{DiagnosticCode::IncompleteSchema, Tpe, "incomplete-schema"},
    ErrorPath{DiagnosticCode::NoMatchingRequestEnvironment, Tpe, "no-matching-request-environment"},
    ErrorPath{DiagnosticCode::PolicyValidation, Tpe, "validation"},
    ErrorPath{DiagnosticCode::MissingTypeAnnotation, ExpressionToResidual, "missing-type-annotation"},
    ErrorPath{DiagnosticCode::ResidualUnlinkedSlot, ExpressionToResidual, "unlinked-slot-error"},

    ErrorPath{DiagnosticCode::MissingEntity, EntitiesConsistency, "missing-entity"},
    ErrorPath{DiagnosticCode::UnknownEntity, EntitiesConsistency, "unknown-entity"},
    ErrorPath{DiagnosticCode::UnknownAttribute, EntityConsistency, "unknown-attribute"},
    ErrorPath{DiagnosticCode::MismatchedAttribute, EntityConsistency, "mismatched-attribute"},
    ErrorPath{DiagnosticCode::MismatchedAncestor, EntityConsistency, "mismatched-ancestor"},
    ErrorPath{DiagnosticCode::UnknownTag, EntityConsistency, "unknown-tag"},
    ErrorPath{DiagnosticCode::MismatchedTag, EntityConsistency, "mismatched-tag"},

    ErrorPath{DiagnosticCode::InconsistentPrincipalType, RequestConsistency, "inconsistent-principal-type"},
    ErrorPath{DiagnosticCode::InconsistentPrincipalEid, RequestConsistency, "inconsistent-principal-eid"},
    ErrorPath{DiagnosticCode::InconsistentResourceType, RequestConsistency, "inconsistent-resource-type"},
    ErrorPath{DiagnosticCode::InconsistentResourceEid, RequestConsistency, "inconsistent-resource-eid"},
    ErrorPath{DiagnosticCode::InconsistentAction, RequestConsistency, "inconsistent-action"},
    ErrorPath{DiagnosticCode::InconsistentContext, RequestConsistency, "inconsistent-context"},

    ErrorPath{DiagnosticCode::InsufficientIterations, BatchedEvaluation, "insufficient-iterations"},
};

[[nodiscard]] consteval auto CodesAreDistinct() -> bool
{
    for (std::size_t left = 0; left < ErrorPaths.size(); ++left)
    {
        for (std::size_t right = left + 1; right < ErrorPaths.size(); ++right)
        {
            if (ErrorPaths[left].code == ErrorPaths[right].code) { return false; }
        }
    }
    return true;
}
static_assert(CodesAreDistinct(), "ErrorPaths maps each code once");

/// Whether `code` is the refusal of a legacy `{"__extn": {"fn": "unknown", ..}}` by cedar-cpp's JSON
/// readers, which give it the one code in every position.
[[nodiscard]] auto IsLegacyUnknownRefusal(const DiagnosticCode code) -> bool
{
    return code == DiagnosticCode::UnknownValueNotSupported;
}

[[nodiscard]] auto FindErrorPath(const DiagnosticCode code) -> const ErrorPath*
{
    const auto* found = std::ranges::find(ErrorPaths, code, &ErrorPath::code);
    return found == ErrorPaths.end() ? nullptr : found;
}

[[nodiscard]] auto Unmapped(const DiagnosticCode code) -> std::string
{
    return std::format("unmapped({})", cedar::CodeName(code));
}

/// The path under tpe's `EntitiesError` (the oracle's `entities_error`).
[[nodiscard]] auto EntitiesSuffix(const ErrorPath& path) -> std::optional<std::string>
{
    switch (path.family)
    {
    case EntitiesDeserialization: return std::format("deserialization.{}", path.variant);
    case EntitySchemaConformance: return std::format("validation.concrete.{}", path.variant);
    case EntityValidation: return std::format("validation.{}", path.variant);
    case Entities: return std::string{path.variant};
    case TransitiveClosure: return std::format("transitive-closure-computation.{}", path.variant);
    default: return std::nullopt;
    }
}

/// The path under `TpeError` (the oracle's `tpe_error`).
[[nodiscard]] auto TpeSuffix(const ErrorPath& path) -> std::optional<std::string>
{
    switch (path.family)
    {
    case Tpe: return std::string{path.variant};
    case ExpressionToResidual: return std::format("expression-to-residual-error.{}", path.variant);
    default: return std::nullopt;
    }
}

/// The APIs that raise errors, each with its own error enum.
enum class RaisedBy : std::uint8_t
{
    PartialRequestCreation,
    PartialEntities,
    PartialAuthorization,
    Reauthorization,
    BatchedAuthorization,
    PermissionQuery,
};

[[nodiscard]] auto SuffixFor(const RaisedBy raisedBy, const ErrorPath& path) -> std::optional<std::string>
{
    switch (raisedBy)
    {
    case RaisedBy::PartialRequestCreation:
        if (path.family == RequestValidation) { return std::format("validation.{}", path.variant); }
        return std::nullopt;
    case RaisedBy::PartialEntities: return EntitiesSuffix(path);
    case RaisedBy::PartialAuthorization: return TpeSuffix(path);
    case RaisedBy::Reauthorization:
        switch (path.family)
        {
        case RequestValidation: return std::format("request-validation.{}", path.variant);
        case EntitySchemaConformance: return std::format("entity-validation.{}", path.variant);
        case EntitiesConsistency: return std::format("inconsistent-entities.{}", path.variant);
        case EntityConsistency: return std::format("inconsistent-entities.inconsistent-entity.{}", path.variant);
        case RequestConsistency: return std::format("inconsistent-requests.{}", path.variant);
        default: return std::nullopt;
        }
    case RaisedBy::BatchedAuthorization:
        if (path.family == RequestValidation) { return std::format("request-validation.{}", path.variant); }
        if (path.family == BatchedEvaluation) { return std::string{path.variant}; }
        if (auto tpe = TpeSuffix(path)) { return std::format("tpe.{}", *tpe); }
        if (auto entities = EntitiesSuffix(path)) { return std::format("entities.{}", *entities); }
        return std::nullopt;
    case RaisedBy::PermissionQuery:
        if (auto tpe = TpeSuffix(path)) { return std::format("tpe.{}", *tpe); }
        if (auto entities = EntitiesSuffix(path)) { return std::format("entities.{}", *entities); }
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] auto PrefixOf(const RaisedBy raisedBy) -> std::string_view
{
    switch (raisedBy)
    {
    case RaisedBy::PartialRequestCreation: return "partial-request-creation";
    case RaisedBy::PartialEntities: return "entities";
    case RaisedBy::PartialAuthorization: return "tpe";
    case RaisedBy::Reauthorization: return "tpe-reauthorization";
    case RaisedBy::BatchedAuthorization: return "batched-evaluation";
    case RaisedBy::PermissionQuery: return "permission-query";
    }
    return {};
}

/// The oracle's path for `error`, raised by `raisedBy`, read from its primary diagnostic's code.
template <typename Tag>
[[nodiscard]] auto PathOf(const RaisedBy raisedBy, const cedar::DomainError<Tag>& error) -> std::string
{
    const DiagnosticCode code = error.Primary().code;
    const ErrorPath* path = FindErrorPath(code);
    std::optional<std::string> suffix = path == nullptr ? std::nullopt : SuffixFor(raisedBy, *path);
    return std::format("{}.{}", PrefixOf(raisedBy), suffix ? *suffix : Unmapped(code));
}

// ---- Manifest JSON ----

/// Whether every string and key under `node` decodes strictly, as serde_json requires.
[[nodiscard]] auto IsStrict(const JsonNode& node) -> bool
{
    switch (node.kind)
    {
    case JsonKind::String: return node.strictString;
    case JsonKind::Array: return std::ranges::all_of(node.Elements(), IsStrict);
    case JsonKind::Object:
        return std::ranges::all_of(node.Members(),
                                   [](const JsonMember& member) { return member.strictKey && IsStrict(member.value); });
    default: return true;
    }
}

/// The value of `key`; with a repeated key, the last one, as serde_json's map keeps it.
[[nodiscard]] auto Member(const JsonNode& object, const std::string_view key) -> const JsonNode*
{
    const JsonNode* found = nullptr;
    for (const JsonMember& member : object.Members())
    {
        if (member.key == key) { found = &member.value; }
    }
    return found;
}

/// Writes `node` back as JSON text, each repeated key once in its first position with its last
/// value: the `serde_json::Value` Rust passes on, as text for cedar-cpp's text readers.
void AppendAsSerdeValue(std::string& output, const JsonNode& node)
{
    switch (node.kind)
    {
    case JsonKind::Null: output += "null"; return;
    case JsonKind::False: output += "false"; return;
    case JsonKind::True: output += "true"; return;
    case JsonKind::Number: output += node.text; return;
    case JsonKind::String: cedar::detail::AppendJsonString(output, node.text); return;
    case JsonKind::Array:
    {
        output.push_back('[');
        bool first = true;
        for (const JsonNode& element : node.Elements())
        {
            if (!first) { output.push_back(','); }
            first = false;
            AppendAsSerdeValue(output, element);
        }
        output.push_back(']');
        return;
    }
    case JsonKind::Object:
    {
        output.push_back('{');
        std::vector<std::string_view> written;
        for (const JsonMember& member : node.Members())
        {
            if (std::ranges::contains(written, member.key)) { continue; }
            if (!written.empty()) { output.push_back(','); }
            written.push_back(member.key);
            cedar::detail::AppendJsonString(output, member.key);
            output.push_back(':');
            AppendAsSerdeValue(output, *Member(node, member.key));
        }
        output.push_back('}');
        return;
    }
    }
}

[[nodiscard]] auto AsSerdeText(const JsonNode& node) -> std::string
{
    std::string text;
    AppendAsSerdeValue(text, node);
    return text;
}

[[nodiscard]] auto IsValidUtf8(const std::string_view text) -> bool
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        std::uint32_t codePoint = 0;
        if (lead < 0x80)
        {
            ++index;
            continue;
        }
        if ((lead & 0xE0) == 0xC0) { length = 2, codePoint = lead & 0x1F; }
        else if ((lead & 0xF0) == 0xE0) { length = 3, codePoint = lead & 0x0F; }
        else if ((lead & 0xF8) == 0xF0) { length = 4, codePoint = lead & 0x07; }
        else { return false; }
        if (index + length > text.size()) { return false; }
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) { return false; }
            codePoint = (codePoint << 6) | (continuation & 0x3F);
        }
        constexpr std::array<std::uint32_t, 5> Minimum{0, 0, 0x80, 0x800, 0x10000};
        if (codePoint < Minimum[length] || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            return false;
        }
        index += length;
    }
    return true;
}

/// The whole file as UTF-8 text, or nothing when it cannot be read as such (Rust's `read_to_string`).
[[nodiscard]] auto ReadUtf8File(const fs::path& path) -> std::optional<std::string>
{
    std::error_code error;
    if (fs::is_directory(path, error)) { return std::nullopt; }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) { return std::nullopt; }
    std::string text{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    if (stream.bad() || !IsValidUtf8(text)) { return std::nullopt; }
    return text;
}

// ---- Output pieces ----

[[nodiscard]] auto DecisionJson(const std::optional<cedar::Decision> decision) -> OutputJson
{
    if (!decision) { return OutputJson::Null(); }
    return OutputJson::String(*decision == cedar::Decision::Allow ? "allow" : "deny");
}

[[nodiscard]] auto SortedIds(std::vector<std::string> ids) -> OutputJson
{
    std::ranges::sort(ids);
    std::vector<OutputJson> values;
    values.reserve(ids.size());
    for (std::string& id : ids) { values.push_back(OutputJson::String(id)); }
    return OutputJson::Array(std::move(values));
}

[[nodiscard]] auto SortedIds(const std::span<const cedar::PolicyId> ids) -> OutputJson
{
    std::vector<std::string> texts;
    texts.reserve(ids.size());
    for (const cedar::PolicyId& id : ids) { texts.emplace_back(id.Text()); }
    return SortedIds(std::move(texts));
}

using UidKey = std::pair<std::string, std::string>;

[[nodiscard]] auto KeyOf(const cedar::EntityUid& uid) -> UidKey
{
    return {std::string{uid.Type().GetName().QualifiedText()}, std::string{uid.Id().Text()}};
}

[[nodiscard]] auto UidJson(const cedar::EntityUid& uid) -> OutputJson
{
    auto [type, id] = KeyOf(uid);
    return OutputJson::Object().Add("id", OutputJson::String(id)).Add("type", OutputJson::String(type));
}

/// The uids sorted by type, then id, both by UTF-8 bytes; each once, as a Rust `HashSet` holds it.
[[nodiscard]] auto SortedUids(const std::span<const cedar::EntityUid> uids) -> OutputJson
{
    std::vector<std::pair<UidKey, const cedar::EntityUid*>> keyed;
    keyed.reserve(uids.size());
    for (const cedar::EntityUid& uid : uids) { keyed.emplace_back(KeyOf(uid), &uid); }
    std::ranges::sort(keyed, {}, &std::pair<UidKey, const cedar::EntityUid*>::first);
    const auto repeated = std::ranges::unique(keyed, {}, &std::pair<UidKey, const cedar::EntityUid*>::first);
    keyed.erase(repeated.begin(), repeated.end());
    std::vector<OutputJson> values;
    values.reserve(keyed.size());
    for (const auto& [key, uid] : keyed) { values.push_back(UidJson(*uid)); }
    return OutputJson::Array(std::move(values));
}

[[nodiscard]] auto ExtensionJson(const std::string_view function, const std::string_view argument) -> OutputJson
{
    return OutputJson::Object().Add(
        "__extn", OutputJson::Object().Add("fn", OutputJson::String(function)).Add("arg", OutputJson::String(argument)));
}

/// `value` in the Cedar JSON value form (Rust's `CedarValueJson::from_value`): extension values in
/// their canonical constructor form.
[[nodiscard]] auto ValueJson(const cedar::Value& value) -> OutputJson
{
    switch (value.Kind())
    {
    case cedar::ValueKind::Bool: return OutputJson::Bool(value.AsBool());
    case cedar::ValueKind::Long: return OutputJson::Integer(value.AsLong());
    case cedar::ValueKind::String: return OutputJson::String(value.AsString());
    case cedar::ValueKind::Entity: return OutputJson::Object().Add("__entity", UidJson(value.AsEntity()));
    case cedar::ValueKind::Set:
    {
        std::vector<OutputJson> elements;
        for (const cedar::Value& element : value.AsSet().Elements()) { elements.push_back(ValueJson(element)); }
        return OutputJson::Array(std::move(elements));
    }
    case cedar::ValueKind::Record:
    {
        OutputJson record = OutputJson::Object();
        for (const cedar::RecordEntry& entry : value.AsRecord()) { record.Add(std::string{entry.key}, ValueJson(entry.value)); }
        return record;
    }
    case cedar::ValueKind::Decimal:
    {
        std::string argument;
        cedar::AppendCanonicalString(argument, value.AsDecimal());
        return ExtensionJson("decimal", argument);
    }
    case cedar::ValueKind::IpAddress:
    {
        std::string argument;
        cedar::AppendCanonicalString(argument, value.AsIpAddress());
        return ExtensionJson("ip", argument);
    }
    case cedar::ValueKind::Duration:
    {
        std::string argument;
        cedar::AppendCanonicalString(argument, value.AsDuration());
        return ExtensionJson("duration", argument);
    }
    case cedar::ValueKind::Datetime:
    {
        // The canonical call `datetime("1970-01-01").offset(duration("<n>ms"))`.
        std::string offset;
        cedar::AppendCanonicalString(offset, cedar::Duration{value.AsDatetime().millisecondsSinceEpoch});
        std::vector<OutputJson> arguments;
        arguments.push_back(ExtensionJson("datetime", "1970-01-01"));
        arguments.push_back(ExtensionJson("duration", offset));
        return OutputJson::Object().Add("__extn", OutputJson::Object()
                                                      .Add("fn", OutputJson::String("offset"))
                                                      .Add("args", OutputJson::Array(std::move(arguments))));
    }
    }
    return OutputJson::Null();
}

// ---- Residual trees ----

[[nodiscard]] auto UnaryName(const cedar::UnaryOperator operation) -> std::string_view
{
    switch (operation)
    {
    case cedar::UnaryOperator::Not: return "not";
    case cedar::UnaryOperator::Negate: return "neg";
    case cedar::UnaryOperator::IsEmpty: return "is-empty";
    }
    return {};
}

[[nodiscard]] auto BinaryName(const cedar::BinaryOperator operation) -> std::string_view
{
    switch (operation)
    {
    case cedar::BinaryOperator::Equal: return "eq";
    case cedar::BinaryOperator::Less: return "less";
    case cedar::BinaryOperator::LessEqual: return "less-eq";
    case cedar::BinaryOperator::Add: return "add";
    case cedar::BinaryOperator::Subtract: return "sub";
    case cedar::BinaryOperator::Multiply: return "mul";
    case cedar::BinaryOperator::In: return "in";
    case cedar::BinaryOperator::Contains: return "contains";
    case cedar::BinaryOperator::ContainsAll: return "contains-all";
    case cedar::BinaryOperator::ContainsAny: return "contains-any";
    case cedar::BinaryOperator::GetTag: return "get-tag";
    case cedar::BinaryOperator::HasTag: return "has-tag";
    }
    return {};
}

/// Splits UTF-8 text into one string per character (Rust's `PatternElem::Char`).
void AppendCharacters(std::vector<OutputJson>& output, const std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::size_t length = 1;
        if (lead >= 0xF0) { length = 4; }
        else if (lead >= 0xE0) { length = 3; }
        else if (lead >= 0xC0) { length = 2; }
        length = std::min(length, text.size() - index);
        output.push_back(OutputJson::String(text.substr(index, length)));
        index += length;
    }
}

/// Writes a residual one-to-one with Rust's `tpe::residual::Residual`. A residual policy is an
/// ordinary policy whose `when` clause is the residual, held in the policy's expression arena: a
/// concrete value is a Literal (or a folded constant), and `Residual::Error` is the call `error()`.
class ResidualWriter final
{
public:
    explicit ResidualWriter(const cedar::Policy& policy) noexcept : policy_{policy}, arena_{policy.Arena()} {}

    [[nodiscard]] auto Write(const cedar::ExpressionIndex index) const -> OutputJson
    {
        return arena_.Visit(index, cedar::Overloaded{
            [](const cedar::node::Literal& literal) { return Concrete(literal.value); },
            [](const cedar::node::Folded& folded) { return Concrete(folded.value); },
            [](const cedar::node::Variable& variable) {
                return Kind("var").Add("var", OutputJson::String(cedar::ToString(variable.variable)));
            },
            [this](const cedar::node::Slot& slot) {
                const cedar::EntityUid* bound = policy_.Binding(slot.slot);
                if (bound == nullptr) { return Kind("unbound-slot"); }
                return Kind("concrete").Add("value", OutputJson::Object().Add("__entity", UidJson(*bound)));
            },
            [this](const cedar::node::If& node) {
                return Kind("if")
                    .Add("test_expr", Write(node.condition))
                    .Add("then_expr", Write(node.thenBranch))
                    .Add("else_expr", Write(node.elseBranch));
            },
            [this](const cedar::node::And& node) {
                return Kind("and").Add("left", Write(node.left)).Add("right", Write(node.right));
            },
            [this](const cedar::node::Or& node) {
                return Kind("or").Add("left", Write(node.left)).Add("right", Write(node.right));
            },
            [this](const cedar::node::Unary& node) {
                return Kind("unary-app")
                    .Add("op", OutputJson::String(UnaryName(node.operation)))
                    .Add("arg", Write(node.operand));
            },
            [this](const cedar::node::Binary& node) {
                return Kind("binary-app")
                    .Add("op", OutputJson::String(BinaryName(node.operation)))
                    .Add("arg1", Write(node.left))
                    .Add("arg2", Write(node.right));
            },
            [this](const cedar::node::ExtensionCall& node) {
                const std::span<const cedar::ExpressionIndex> arguments = arena_.Elements(node.arguments);
                const std::string name = node.function.ToString();
                if (name == "error" && arguments.empty()) { return Kind("error"); }
                std::vector<OutputJson> written;
                for (const cedar::ExpressionIndex argument : arguments) { written.push_back(Write(argument)); }
                return Kind("extension-function-app")
                    .Add("fn_name", OutputJson::String(name))
                    .Add("args", OutputJson::Array(std::move(written)));
            },
            [this](const cedar::node::GetAttribute& node) {
                return Kind("get-attribute").Add("expr", Write(node.object)).Add("attr", OutputJson::String(node.attribute));
            },
            [this](const cedar::node::HasAttribute& node) {
                const std::span<const std::string_view> path = arena_.Elements(node.path);
                if (!node.extended)
                {
                    return Kind("has-attribute").Add("expr", Write(node.object)).Add("attr", OutputJson::String(path[0]));
                }
                std::vector<OutputJson> attributes;
                for (const std::string_view attribute : path) { attributes.push_back(OutputJson::String(attribute)); }
                return Kind("extension-has-attribute")
                    .Add("expr", Write(node.object))
                    .Add("attrs", OutputJson::Array(std::move(attributes)));
            },
            [this](const cedar::node::Like& node) {
                const std::span<const std::string_view> literals = arena_.PatternOf(node).Literals();
                std::vector<OutputJson> pattern;
                for (std::size_t position = 0; position < literals.size(); ++position)
                {
                    if (position > 0) { pattern.push_back(OutputJson::Null()); }
                    AppendCharacters(pattern, literals[position]);
                }
                return Kind("like").Add("expr", Write(node.operand)).Add("pattern", OutputJson::Array(std::move(pattern)));
            },
            [this](const cedar::node::Is& node) {
                return Kind("is").Add("expr", Write(node.operand)).Add("entity_type", OutputJson::String(node.type.ToString()));
            },
            [this](const cedar::node::SetLiteral& node) {
                std::vector<OutputJson> elements;
                for (const cedar::ExpressionIndex element : arena_.Elements(node.elements))
                {
                    elements.push_back(Write(element));
                }
                return Kind("set").Add("elements", OutputJson::Array(std::move(elements)));
            },
            [this](const cedar::node::RecordLiteral& node) {
                OutputJson fields = OutputJson::Object();
                for (const cedar::RecordLiteralField& field : arena_.Elements(node.fields))
                {
                    fields.Add(std::string{field.key}, Write(field.value));
                }
                return Kind("record").Add("fields", std::move(fields));
            },
        });
    }

private:
    [[nodiscard]] static auto Kind(const std::string_view kind) -> OutputJson
    {
        return OutputJson::Object().Add("kind", OutputJson::String(kind));
    }

    [[nodiscard]] static auto Concrete(const cedar::Value& value) -> OutputJson
    {
        return Kind("concrete").Add("value", ValueJson(value));
    }

    const cedar::Policy& policy_;
    const cedar::ExpressionArena& arena_;
};

/// The residual tree of a residual policy: its `when` clause.
[[nodiscard]] auto ResidualJson(const cedar::Policy& policy) -> OutputJson
{
    const std::optional<cedar::ExpressionIndex> condition = policy.GetTemplate().NonScopeCondition();
    return ResidualWriter{policy}.Write(condition.value_or(policy.Condition()));
}

[[nodiscard]] auto PoliciesJson(const cedar::PartialAuthorizationResponse& response) -> OutputJson
{
    OutputJson policies = OutputJson::Object();
    for (const cedar::Policy& policy : response.Policies())
    {
        OutputJson annotations = OutputJson::Object();
        for (const cedar::Annotation& annotation : policy.GetAnnotations().Entries())
        {
            annotations.Add(std::string{annotation.key.Text()}, OutputJson::String(annotation.value.value_or("")));
        }
        policies.Add(std::string{policy.Id().Text()},
                     OutputJson::Object()
                         .Add("annotations", std::move(annotations))
                         .Add("effect", OutputJson::String(policy.GetEffect() == cedar::Effect::Permit ? "permit" : "forbid"))
                         .Add("residual", ResidualJson(policy)));
    }
    return policies;
}

// ---- Cases ----

/// How a request object's principal or resource may carry its id.
enum class IdRule : std::uint8_t
{
    Required,
    Optional,
    Forbidden,
};

/// The JSON object `node`, holding no key outside `allowed`.
[[nodiscard]] auto ObjectWithKeys(const JsonNode& node, const std::span<const std::string_view> allowed)
    -> Outcome<const JsonNode*>
{
    if (!node.IsObject()) { return Fail("input.manifest"); }
    for (const JsonMember& member : node.Members())
    {
        if (!std::ranges::contains(allowed, member.key)) { return Fail("input.manifest"); }
    }
    return &node;
}

[[nodiscard]] auto Field(const JsonNode& object, const std::string_view key) -> Outcome<const JsonNode*>
{
    const JsonNode* value = Member(object, key);
    if (value == nullptr) { return Fail("input.manifest"); }
    return value;
}

[[nodiscard]] auto StringField(const JsonNode& object, const std::string_view key) -> Outcome<std::string_view>
{
    const JsonNode* value = Member(object, key);
    if (value == nullptr || !value->IsString()) { return Fail("input.manifest"); }
    return value->text;
}

struct UidParts
{
    cedar::EntityType type;
    std::optional<cedar::Eid> id;
};

[[nodiscard]] auto ReadUidParts(const JsonNode& node, const IdRule rule) -> Outcome<UidParts>
{
    constexpr std::array<std::string_view, 2> Keys{"type", "id"};
    auto object = ObjectWithKeys(node, Keys);
    if (!object) { return std::unexpected{object.error()}; }
    auto type = StringField(**object, "type");
    if (!type) { return std::unexpected{type.error()}; }
    std::optional<std::string_view> id;
    if (const JsonNode* idNode = Member(**object, "id"))
    {
        if (!idNode->IsString()) { return Fail("input.manifest"); }
        id = idNode->text;
    }
    if ((rule == IdRule::Required && !id) || (rule == IdRule::Forbidden && id)) { return Fail("input.manifest"); }
    auto parsed = cedar::EntityType::Parse(*type);
    if (!parsed) { return Fail("input.uid"); }
    return UidParts{.type = *std::move(parsed), .id = id ? std::optional{cedar::Eid{*id}} : std::nullopt};
}

[[nodiscard]] auto ConcreteUid(const JsonNode& node) -> Outcome<cedar::EntityUid>
{
    auto parts = ReadUidParts(node, IdRule::Required);
    if (!parts) { return std::unexpected{parts.error()}; }
    return cedar::EntityUid{std::move(parts->type), *std::move(parts->id)};
}

[[nodiscard]] auto PartialUid(const JsonNode& node) -> Outcome<cedar::PartialEntityUid>
{
    auto parts = ReadUidParts(node, IdRule::Optional);
    if (!parts) { return std::unexpected{parts.error()}; }
    return cedar::PartialEntityUid{.type = std::move(parts->type), .id = std::move(parts->id)};
}

/// What a request context's legacy `unknown` stands for in Rust, when the context reaches a Cedar
/// API that refuses unknowns.
enum class ContextUnknowns : std::uint8_t
{
    InputContext,           ///< A concrete request: the refusal stays `input.context`.
    ContextContainsUnknowns ///< A partial or query request: `ContextContainsUnknowns`.
};

/// The request's context, or nothing when it has none. `action` set reads it by the schema's
/// context type for that action.
[[nodiscard]] auto ReadContext(const JsonNode& request, const cedar::Schema* schema, const cedar::EntityUid* action,
                               const ContextUnknowns unknowns) -> Outcome<std::optional<cedar::Context>>
{
    const JsonNode* node = Member(request, "context");
    if (node == nullptr) { return std::optional<cedar::Context>{}; }
    const std::string text = AsSerdeText(*node);
    auto context = schema != nullptr ? cedar::ContextFromJson(text, *schema, *action) : cedar::ContextFromJson(text);
    if (!context)
    {
        if (unknowns == ContextUnknowns::ContextContainsUnknowns && IsLegacyUnknownRefusal(context.error().Primary().code))
        {
            return Fail("partial-request-creation.context-contains-unknowns");
        }
        return Fail("input.context");
    }
    return std::optional{*std::move(context)};
}

constexpr std::array<std::string_view, 4> RequestKeys{"principal", "action", "resource", "context"};

/// A concrete request: the batched request, and every completion's (Rust's `Request::new` without a
/// schema, which refuses nothing here).
[[nodiscard]] auto ReadConcreteRequest(const JsonNode& node, const cedar::Schema& schema) -> Outcome<cedar::Request>
{
    auto request = ObjectWithKeys(node, RequestKeys);
    if (!request) { return std::unexpected{request.error()}; }
    std::array<std::optional<cedar::EntityUid>, 3> uids{};
    constexpr std::array<std::string_view, 3> Sides{"principal", "action", "resource"};
    for (std::size_t index = 0; index < Sides.size(); ++index)
    {
        auto field = Field(**request, Sides[index]);
        if (!field) { return std::unexpected{field.error()}; }
        auto uid = ConcreteUid(**field);
        if (!uid) { return std::unexpected{uid.error()}; }
        uids[index] = *std::move(uid);
    }
    auto context = ReadContext(**request, &schema, &*uids[1], ContextUnknowns::InputContext);
    if (!context) { return std::unexpected{context.error()}; }
    return cedar::Request{*std::move(uids[0]), *std::move(uids[1]), *std::move(uids[2]),
                          context->value_or(cedar::Context{})};
}

/// What a concrete entities file's legacy `unknown` stands for in Rust.
enum class EntitiesUnknowns : std::uint8_t
{
    InputEntities,       ///< The refusal stays `input.entities`.
    PermissionQueryValue ///< A principal or resource query converts them and refuses the unknown.
};

class CaseRunner final
{
public:
    CaseRunner(const JsonNode& object, fs::path directory) : case_{object}, directory_{std::move(directory)} {}

    [[nodiscard]] auto Run() -> Outcome<OutputJson>
    {
        const JsonNode* name = Member(case_, "name");
        if (name == nullptr || !name->IsString()) { return Fail("input.manifest"); }
        const JsonNode* operationNode = Member(case_, "operation");
        if (operationNode == nullptr || !operationNode->IsString()) { return Fail("input.manifest"); }
        const std::string_view operation = operationNode->text;
        constexpr std::array<std::string_view, 7> Common{"name",     "operation", "schema", "policies",
                                                         "links",    "entities",  "request"};
        std::string_view extra;
        if (operation == "partial-authorize") { extra = "completions"; }
        else if (operation == "batched") { extra = "maximum-iterations"; }
        else if (operation != "query-principal" && operation != "query-resource" && operation != "query-action")
        {
            return Fail("input.manifest");
        }
        for (const JsonMember& member : case_.Members())
        {
            if (!std::ranges::contains(Common, member.key) && (extra.empty() || member.key != extra))
            {
                return Fail("input.manifest");
            }
        }
        if (const JsonNode* completions = Member(case_, "completions"))
        {
            if (!completions->IsArray()) { return Fail("input.manifest"); }
            constexpr std::array<std::string_view, 2> CompletionKeys{"request", "entities"};
            for (const JsonNode& completion : completions->Elements())
            {
                if (auto checked = ObjectWithKeys(completion, CompletionKeys); !checked)
                {
                    return std::unexpected{checked.error()};
                }
            }
        }
        auto schema = ReadSchema();
        if (!schema) { return std::unexpected{schema.error()}; }
        auto policies = ReadPolicies();
        if (!policies) { return std::unexpected{policies.error()}; }
        if (operation == "partial-authorize") { return PartialAuthorize(*schema, *policies); }
        if (operation == "batched") { return Batched(*schema, *policies); }
        if (operation == "query-principal") { return QueryPrincipal(*schema, *policies); }
        if (operation == "query-resource") { return QueryResource(*schema, *policies); }
        return QueryAction(*schema, *policies);
    }

private:
    [[nodiscard]] auto Required(const std::string_view key) const -> Outcome<const JsonNode*>
    {
        return Field(case_, key);
    }

    [[nodiscard]] auto ReadNamedFile(const JsonNode& value) const -> Outcome<std::string>
    {
        if (!value.IsString()) { return Fail("input.manifest"); }
        auto text = ReadUtf8File(directory_ / fs::path{std::string{value.text}});
        if (!text) { return Fail("input.file"); }
        return *std::move(text);
    }

    [[nodiscard]] auto ReadSchema() const -> Outcome<cedar::Schema>
    {
        auto value = Required("schema");
        if (!value) { return std::unexpected{value.error()}; }
        auto text = ReadNamedFile(**value);
        if (!text) { return std::unexpected{text.error()}; }
        if ((*value)->text.ends_with(".json"))
        {
            auto schema = cedar::SchemaFromJson(*text);
            if (!schema) { return Fail("input.schema"); }
            return *std::move(schema);
        }
        auto schema = cedar::ParseSchema(*text);
        if (!schema) { return Fail("input.schema"); }
        return std::move(schema->value);
    }

    [[nodiscard]] auto ReadPolicies() const -> Outcome<cedar::PolicySet>
    {
        auto value = Required("policies");
        if (!value) { return std::unexpected{value.error()}; }
        auto text = ReadNamedFile(**value);
        if (!text) { return std::unexpected{text.error()}; }
        auto policies = cedar::ParsePolicySet(*text);
        if (!policies) { return Fail("input.policies"); }
        const JsonNode* links = Member(case_, "links");
        if (links == nullptr) { return *std::move(policies); }
        if (!links->IsArray()) { return Fail("input.manifest"); }
        constexpr std::array<std::string_view, 3> LinkKeys{"template", "id", "slots"};
        constexpr std::array<std::string_view, 2> SlotKeys{"?principal", "?resource"};
        for (const JsonNode& linkNode : links->Elements())
        {
            auto link = ObjectWithKeys(linkNode, LinkKeys);
            if (!link) { return std::unexpected{link.error()}; }
            auto templateId = StringField(**link, "template");
            if (!templateId) { return std::unexpected{templateId.error()}; }
            auto id = StringField(**link, "id");
            if (!id) { return std::unexpected{id.error()}; }
            auto slotsNode = Field(**link, "slots");
            if (!slotsNode) { return std::unexpected{slotsNode.error()}; }
            auto slots = ObjectWithKeys(**slotsNode, SlotKeys);
            if (!slots) { return std::unexpected{slots.error()}; }
            cedar::SlotBindings bindings;
            for (const auto& [key, slot] : {std::pair{std::string_view{"?principal"}, cedar::SlotId::Principal},
                                            std::pair{std::string_view{"?resource"}, cedar::SlotId::Resource}})
            {
                if (const JsonNode* uidNode = Member(**slots, key))
                {
                    auto uid = ConcreteUid(*uidNode);
                    if (!uid) { return std::unexpected{uid.error()}; }
                    bindings.Bind(slot, *std::move(uid));
                }
            }
            if (!policies->Link(*templateId, cedar::PolicyId{*id}, std::move(bindings))) { return Fail("input.links"); }
        }
        return *std::move(policies);
    }

    /// An entities file as the `serde_json::Value` Rust reads it into, written back as text.
    [[nodiscard]] auto ReadEntitiesJson(const JsonNode& value) const -> Outcome<std::string>
    {
        auto text = ReadNamedFile(value);
        if (!text) { return std::unexpected{text.error()}; }
        std::pmr::monotonic_buffer_resource resource;
        auto document = cedar::detail::ReadJson(*text, resource);
        if (!document || !IsStrict(document->Root())) { return Fail("input.entities"); }
        return AsSerdeText(document->Root());
    }

    [[nodiscard]] auto ReadConcreteEntities(const JsonNode& value, const cedar::Schema& schema,
                                            const EntitiesUnknowns unknowns) const -> Outcome<cedar::Entities>
    {
        auto json = ReadEntitiesJson(value);
        if (!json) { return std::unexpected{json.error()}; }
        auto entities = cedar::EntitiesFromJson(*json, schema);
        if (!entities)
        {
            if (unknowns == EntitiesUnknowns::PermissionQueryValue &&
                IsLegacyUnknownRefusal(entities.error().Primary().code))
            {
                return Fail("permission-query.entities.partial-value-to-value.contains-unknown");
            }
            return Fail("input.entities");
        }
        return *std::move(entities);
    }

    [[nodiscard]] auto ReadPartialEntities(const cedar::Schema& schema) const -> Outcome<cedar::PartialEntities>
    {
        auto value = Required("entities");
        if (!value) { return std::unexpected{value.error()}; }
        auto json = ReadEntitiesJson(**value);
        if (!json) { return std::unexpected{json.error()}; }
        auto entities = cedar::PartialEntitiesFromJson(*json, schema);
        if (!entities) { return Fail(PathOf(RaisedBy::PartialEntities, entities.error())); }
        return *std::move(entities);
    }

    [[nodiscard]] auto PartialAuthorize(const cedar::Schema& schema, const cedar::PolicySet& policies) const
        -> Outcome<OutputJson>
    {
        auto requestNode = Required("request");
        if (!requestNode) { return std::unexpected{requestNode.error()}; }
        auto request = ObjectWithKeys(**requestNode, RequestKeys);
        if (!request) { return std::unexpected{request.error()}; }
        auto principalNode = Field(**request, "principal");
        if (!principalNode) { return std::unexpected{principalNode.error()}; }
        auto principal = PartialUid(**principalNode);
        if (!principal) { return std::unexpected{principal.error()}; }
        auto actionNode = Field(**request, "action");
        if (!actionNode) { return std::unexpected{actionNode.error()}; }
        auto action = ConcreteUid(**actionNode);
        if (!action) { return std::unexpected{action.error()}; }
        auto resourceNode = Field(**request, "resource");
        if (!resourceNode) { return std::unexpected{resourceNode.error()}; }
        auto resource = PartialUid(**resourceNode);
        if (!resource) { return std::unexpected{resource.error()}; }
        auto context = ReadContext(**request, &schema, &*action, ContextUnknowns::ContextContainsUnknowns);
        if (!context) { return std::unexpected{context.error()}; }
        auto partialRequest =
            cedar::PartialRequest::Create(*std::move(principal), *action, *std::move(resource), *std::move(context), schema);
        if (!partialRequest) { return Fail(PathOf(RaisedBy::PartialRequestCreation, partialRequest.error())); }
        auto entities = ReadPartialEntities(schema);
        if (!entities) { return std::unexpected{entities.error()}; }
        auto response = cedar::IsAuthorizedPartial(*partialRequest, policies, *entities, schema);
        if (!response) { return Fail(PathOf(RaisedBy::PartialAuthorization, response.error())); }

        OutputJson result = OutputJson::Object();
        result.Add("decision", DecisionJson(response->GetDecision()));
        const auto reason = response->Reason();
        result.Add("reason", reason ? SortedIds(*reason) : OutputJson::Null());
        result.Add("true-permits", SortedIds(response->TruePermits()));
        result.Add("false-permits", SortedIds(response->FalsePermits()));
        result.Add("error-permits", SortedIds(response->ErrorPermits()));
        result.Add("residual-permits", SortedIds(response->ResidualPermits()));
        result.Add("true-forbids", SortedIds(response->TrueForbids()));
        result.Add("false-forbids", SortedIds(response->FalseForbids()));
        result.Add("error-forbids", SortedIds(response->ErrorForbids()));
        result.Add("residual-forbids", SortedIds(response->ResidualForbids()));
        result.Add("policies", PoliciesJson(*response));

        std::vector<OutputJson> reauthorizations;
        if (const JsonNode* completions = Member(case_, "completions"))
        {
            for (const JsonNode& completion : completions->Elements())
            {
                auto entry = Reauthorize(schema, *response, completion);
                reauthorizations.push_back(entry ? *std::move(entry)
                                                 : OutputJson::Object().Add("error", OutputJson::String(entry.error())));
            }
        }
        result.Add("reauthorizations", OutputJson::Array(std::move(reauthorizations)));
        return result;
    }

    [[nodiscard]] auto Reauthorize(const cedar::Schema& schema, const cedar::PartialAuthorizationResponse& response,
                                   const JsonNode& completion) const -> Outcome<OutputJson>
    {
        auto requestNode = Field(completion, "request");
        if (!requestNode) { return std::unexpected{requestNode.error()}; }
        auto request = ReadConcreteRequest(**requestNode, schema);
        if (!request) { return std::unexpected{request.error()}; }
        auto entitiesNode = Field(completion, "entities");
        if (!entitiesNode) { return std::unexpected{entitiesNode.error()}; }
        auto entities = ReadConcreteEntities(**entitiesNode, schema, EntitiesUnknowns::InputEntities);
        if (!entities) { return std::unexpected{entities.error()}; }
        auto answer = response.Reauthorize(*request, *entities);
        if (!answer) { return Fail(PathOf(RaisedBy::Reauthorization, answer.error())); }
        std::vector<std::string> erroring;
        for (const cedar::PolicyEvaluationError& error : answer->errors) { erroring.emplace_back(error.policyId.Text()); }
        return OutputJson::Object()
            .Add("decision", DecisionJson(answer->decision))
            .Add("determining", SortedIds(answer->reason))
            .Add("erroring", SortedIds(std::move(erroring)));
    }

    /// Answers the batched loop from concrete entities, and records the uids of every call.
    class RecordingLoader final
    {
    public:
        explicit RecordingLoader(const cedar::Entities& entities) noexcept : entities_{entities} {}

        [[nodiscard]] auto Load(const std::span<const cedar::EntityUid> uids) -> std::vector<cedar::LoadedEntity>
        {
            rounds_.push_back(SortedUids(uids));
            std::vector<cedar::LoadedEntity> loaded;
            loaded.reserve(uids.size());
            for (const cedar::EntityUid& uid : uids)
            {
                const cedar::Entity* found = entities_.Find(uid);
                loaded.push_back(cedar::LoadedEntity{
                    .uid = uid,
                    .entity = found == nullptr ? nullptr : std::make_shared<const cedar::Entity>(*found),
                });
            }
            return loaded;
        }

        [[nodiscard]] auto TakeRounds() -> std::vector<OutputJson> { return std::move(rounds_); }

    private:
        const cedar::Entities& entities_;
        std::vector<OutputJson> rounds_{};
    };

    [[nodiscard]] auto Batched(const cedar::Schema& schema, const cedar::PolicySet& policies) const
        -> Outcome<OutputJson>
    {
        auto iterationsNode = Required("maximum-iterations");
        if (!iterationsNode) { return std::unexpected{iterationsNode.error()}; }
        if ((*iterationsNode)->kind != JsonKind::Number) { return Fail("input.manifest"); }
        const cedar::detail::JsonNumber number = cedar::detail::ClassifyJsonNumber((*iterationsNode)->text);
        if (number.kind != cedar::detail::JsonNumberKind::Long || number.value < 0 ||
            number.value > std::numeric_limits<std::uint32_t>::max())
        {
            return Fail("input.manifest");
        }
        const auto maximumIterations = static_cast<std::uint32_t>(number.value);
        auto requestNode = Required("request");
        if (!requestNode) { return std::unexpected{requestNode.error()}; }
        auto request = ReadConcreteRequest(**requestNode, schema);
        if (!request) { return std::unexpected{request.error()}; }
        auto entitiesNode = Required("entities");
        if (!entitiesNode) { return std::unexpected{entitiesNode.error()}; }
        auto entities = ReadConcreteEntities(**entitiesNode, schema, EntitiesUnknowns::InputEntities);
        if (!entities) { return std::unexpected{entities.error()}; }
        RecordingLoader loader{*entities};
        auto answer = cedar::IsAuthorizedBatched(*request, policies, schema, loader, maximumIterations);
        OutputJson result = OutputJson::Object();
        if (answer) { result.Add("decision", DecisionJson(*answer)); }
        else { result.Add("error", OutputJson::String(PathOf(RaisedBy::BatchedAuthorization, answer.error()))); }
        result.Add("rounds", OutputJson::Array(loader.TakeRounds()));
        return result;
    }

    /// The request of a principal or resource query: the queried side is a type only.
    struct QueryParts
    {
        UidParts principal;
        cedar::EntityUid action;
        UidParts resource;
        cedar::Context context;
    };

    [[nodiscard]] auto ReadQueryParts(const cedar::Schema& schema, const IdRule principalRule,
                                      const IdRule resourceRule) const -> Outcome<QueryParts>
    {
        auto requestNode = Required("request");
        if (!requestNode) { return std::unexpected{requestNode.error()}; }
        auto request = ObjectWithKeys(**requestNode, RequestKeys);
        if (!request) { return std::unexpected{request.error()}; }
        auto principalNode = Field(**request, "principal");
        if (!principalNode) { return std::unexpected{principalNode.error()}; }
        auto principal = ReadUidParts(**principalNode, principalRule);
        if (!principal) { return std::unexpected{principal.error()}; }
        auto actionNode = Field(**request, "action");
        if (!actionNode) { return std::unexpected{actionNode.error()}; }
        auto action = ConcreteUid(**actionNode);
        if (!action) { return std::unexpected{action.error()}; }
        auto resourceNode = Field(**request, "resource");
        if (!resourceNode) { return std::unexpected{resourceNode.error()}; }
        auto resource = ReadUidParts(**resourceNode, resourceRule);
        if (!resource) { return std::unexpected{resource.error()}; }
        auto context = ReadContext(**request, &schema, &*action, ContextUnknowns::ContextContainsUnknowns);
        if (!context) { return std::unexpected{context.error()}; }
        return QueryParts{.principal = *std::move(principal),
                          .action = *std::move(action),
                          .resource = *std::move(resource),
                          .context = context->value_or(cedar::Context{})};
    }

    [[nodiscard]] auto QueryPrincipal(const cedar::Schema& schema, const cedar::PolicySet& policies) const
        -> Outcome<OutputJson>
    {
        auto parts = ReadQueryParts(schema, IdRule::Forbidden, IdRule::Required);
        if (!parts) { return std::unexpected{parts.error()}; }
        auto query = cedar::PrincipalQueryRequest::Create(
            parts->principal.type, parts->action, cedar::EntityUid{parts->resource.type, *parts->resource.id},
            parts->context, schema);
        if (!query) { return Fail(PathOf(RaisedBy::PartialRequestCreation, query.error())); }
        auto entitiesNode = Required("entities");
        if (!entitiesNode) { return std::unexpected{entitiesNode.error()}; }
        auto entities = ReadConcreteEntities(**entitiesNode, schema, EntitiesUnknowns::PermissionQueryValue);
        if (!entities) { return std::unexpected{entities.error()}; }
        auto uids = cedar::QueryPrincipal(*query, policies, *entities, schema);
        if (!uids) { return Fail(PathOf(RaisedBy::PermissionQuery, uids.error())); }
        return OutputJson::Object().Add("uids", SortedUids(*uids));
    }

    [[nodiscard]] auto QueryResource(const cedar::Schema& schema, const cedar::PolicySet& policies) const
        -> Outcome<OutputJson>
    {
        auto parts = ReadQueryParts(schema, IdRule::Required, IdRule::Forbidden);
        if (!parts) { return std::unexpected{parts.error()}; }
        auto query = cedar::ResourceQueryRequest::Create(
            cedar::EntityUid{parts->principal.type, *parts->principal.id}, parts->action, parts->resource.type,
            parts->context, schema);
        if (!query) { return Fail(PathOf(RaisedBy::PartialRequestCreation, query.error())); }
        auto entitiesNode = Required("entities");
        if (!entitiesNode) { return std::unexpected{entitiesNode.error()}; }
        auto entities = ReadConcreteEntities(**entitiesNode, schema, EntitiesUnknowns::PermissionQueryValue);
        if (!entities) { return std::unexpected{entities.error()}; }
        auto uids = cedar::QueryResource(*query, policies, *entities, schema);
        if (!uids) { return Fail(PathOf(RaisedBy::PermissionQuery, uids.error())); }
        return OutputJson::Object().Add("uids", SortedUids(*uids));
    }

    [[nodiscard]] auto QueryAction(const cedar::Schema& schema, const cedar::PolicySet& policies) const
        -> Outcome<OutputJson>
    {
        auto requestNode = Required("request");
        if (!requestNode) { return std::unexpected{requestNode.error()}; }
        constexpr std::array<std::string_view, 3> Keys{"principal", "resource", "context"};
        auto request = ObjectWithKeys(**requestNode, Keys);
        if (!request) { return std::unexpected{request.error()}; }
        auto principalNode = Field(**request, "principal");
        if (!principalNode) { return std::unexpected{principalNode.error()}; }
        auto principal = PartialUid(**principalNode);
        if (!principal) { return std::unexpected{principal.error()}; }
        auto resourceNode = Field(**request, "resource");
        if (!resourceNode) { return std::unexpected{resourceNode.error()}; }
        auto resource = PartialUid(**resourceNode);
        if (!resource) { return std::unexpected{resource.error()}; }
        auto context = ReadContext(**request, nullptr, nullptr, ContextUnknowns::ContextContainsUnknowns);
        if (!context) { return std::unexpected{context.error()}; }
        const cedar::ActionQueryRequest query{*std::move(principal), *std::move(resource), *std::move(context), schema};
        auto entities = ReadPartialEntities(schema);
        if (!entities) { return std::unexpected{entities.error()}; }
        auto answers = cedar::QueryAction(query, policies, *entities);
        if (!answers) { return Fail(PathOf(RaisedBy::PermissionQuery, answers.error())); }
        std::vector<std::pair<UidKey, OutputJson>> keyed;
        for (const cedar::ActionQueryResult& answer : *answers)
        {
            keyed.emplace_back(KeyOf(answer.action), OutputJson::Object()
                                                         .Add("action", UidJson(answer.action))
                                                         .Add("decision", DecisionJson(answer.decision)));
        }
        std::ranges::stable_sort(keyed, {}, &std::pair<UidKey, OutputJson>::first);
        std::vector<OutputJson> actions;
        for (auto& [key, value] : keyed) { actions.push_back(std::move(value)); }
        return OutputJson::Object().Add("actions", OutputJson::Array(std::move(actions)));
    }

    const JsonNode& case_;
    fs::path directory_;
};

/// One manifest line's output line, without its newline.
[[nodiscard]] auto RunLine(const std::string_view line, const fs::path& directory) -> std::string
{
    // Rust's serde_json reads a line strictly: a string with an unpaired surrogate escape fails it.
    std::pmr::monotonic_buffer_resource resource;
    auto document = cedar::detail::ReadJson(line, resource);
    const JsonNode* object = nullptr;
    if (document && IsStrict(document->Root()) && document->Root().IsObject()) { object = &document->Root(); }

    const auto echoed = [object](const std::string_view key) {
        const JsonNode* value = object == nullptr ? nullptr : Member(*object, key);
        return value != nullptr && value->IsString() ? OutputJson::String(value->text) : OutputJson::Null();
    };
    Outcome<OutputJson> outcome = object == nullptr ? Fail("input.manifest") : CaseRunner{*object, directory}.Run();
    OutputJson output = OutputJson::Object().Add("name", echoed("name")).Add("operation", echoed("operation"));
    if (outcome) { output.Add("result", *std::move(outcome)); }
    else { output.Add("error", OutputJson::String(outcome.error())); }
    std::string text;
    WriteCanonical(output, text);
    return text;
}

[[nodiscard]] auto IsBlank(const std::string_view line) -> bool
{
    return line.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos;
}

} // namespace

auto main(const int argumentCount, char** argumentValues) -> int
{
    if (argumentCount != 2)
    {
        std::fputs("usage: cedar-cpp-partial-evaluation-runner <manifest.jsonl>\n", stderr);
        return 2;
    }
    const fs::path manifestPath{argumentValues[1]};
    const std::optional<std::string> text = ReadUtf8File(manifestPath);
    if (!text)
    {
        std::fputs(std::format("cannot read {}\n", manifestPath.string()).c_str(), stderr);
        return 2;
    }
    const fs::path directory = manifestPath.parent_path();
    std::string_view remaining = *text;
    while (!remaining.empty())
    {
        const std::size_t end = remaining.find('\n');
        std::string_view line = remaining.substr(0, end);
        remaining = end == std::string_view::npos ? std::string_view{} : remaining.substr(end + 1);
        if (line.ends_with('\r')) { line.remove_suffix(1); }
        if (IsBlank(line)) { continue; }
        std::string output = RunLine(line, directory);
        output.push_back('\n');
        // Each line is flushed as it is written, so a case that stops the process is the one after
        // the last line written.
        std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);
    }
    return 0;
}
