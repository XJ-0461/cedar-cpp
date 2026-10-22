#pragma once

#include "json/JsonReader.hpp"

#include <cedar/Diagnostic.hpp>
#include <cedar/Names.hpp>
#include <cedar/Value.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cedar::detail
{

struct Type;

// Cedar's entity JSON value format (Rust's `CedarValueJson` and `EntityUidJson`,
// `cedar-policy-core/src/entities/json/value.rs`) over a JsonNode tree, without a schema.
//
// Rust reads JSON with serde in three ways, and which one applies decides what is accepted:
// - A `serde_json::Value` (a context, an entity uid read by `EntityUid::from_json`): every number
//   must fit an f64, every string must decode, and a key given twice keeps its last value.
// - serde's buffered `Content` (an entity's `uid` and `parents`): as strict, but a repeated key is
//   kept, so a struct field given twice fails that struct and an ignored key may repeat.
// - `JsonValueWithNoDuplicateKeys` (an entity's `attrs` and `tags`): as strict, and a key given
//   twice anywhere inside fails.
// An object then reads as an escape only in exactly the shapes Rust's untagged enums accept.

enum class DuplicateKeys : std::uint8_t
{
    Reject,   ///< `JsonValueWithNoDuplicateKeys`.
    Keep,     ///< serde `Content`.
    LastWins, ///< `serde_json::Value`.
};

/// A failure while reading values, before it is placed in an entities or context error.
struct JsonReadError
{
    DiagnosticCode code;
    std::uint32_t offset;
    std::uint32_t length;
    std::string message;
    std::optional<std::string> help{};
    /// True when an extension function call failed; `code` is then the evaluation error's.
    bool evaluationFailed = false;
};

/// Checks `node` and everything under it as serde reads it into one of the three forms above:
/// nesting within MaximumJsonDepth, strings and keys that decode, numbers that fit an f64, and,
/// for DuplicateKeys::Reject, no key twice in an object.
[[nodiscard]] auto CheckStrictJson(const JsonNode& node, DuplicateKeys duplicates) -> std::expected<void, JsonReadError>;

/// The members of an object as they count for a read under `duplicates`: sorted by key, and, for
/// LastWins, only the last member of each key.
void EffectiveMembers(const JsonNode& object, DuplicateKeys duplicates, std::vector<const JsonMember*>& members);

/// Rust's `TypeAndId` read from serde `Content` or a `serde_json::Value`: an object with string
/// fields `type` and `id` (other keys ignored), or a sequence of exactly those two strings.
struct TypeAndId
{
    std::string_view type;
    std::string_view id;
    const JsonNode* typeNode;
};

/// How a reader turns an entity type's text into an EntityType.
enum class EntityTypeNames : std::uint8_t
{
    /// Any valid name, interned when new (reading without a schema).
    Intern,
    /// Only names already interned: a name nobody has made yet cannot be declared by the schema,
    /// so it fails with `entities.unexpected-entity-type` instead of growing the symbol table
    /// (reading entities with a schema).
    Existing,
};

/// The entity uid `parts` names. Fails when the type is not a valid entity type name, or, for
/// EntityTypeNames::Existing, a name not interned yet.
[[nodiscard]] auto MakeEntityUid(const TypeAndId& parts, EntityTypeNames names = EntityTypeNames::Intern)
    -> std::expected<EntityUid, JsonReadError>;

/// The escapes Rust's `RawCedarValueJson` -> `CedarValueJson` conversion recognizes in an object
/// (`value.rs`, `From<RawCedarValueJson>`): only an object with exactly one key, and only in these
/// shapes; any other object is a record, even one whose key is `__entity` or `__extn`.
enum class CedarValueEscapeKind : std::uint8_t
{
    None,
    Extension,
    Expression,
    Entity,
};

struct CedarValueEscape
{
    CedarValueEscapeKind kind = CedarValueEscapeKind::None;
    const JsonNode* function = nullptr;  ///< `fn`.
    const JsonNode* argument = nullptr;  ///< `arg`, the single-argument form.
    const JsonNode* arguments = nullptr; ///< `args`, the multiple-argument form.
    TypeAndId entity{};
};

/// Which escape the object `object` is, if any.
[[nodiscard]] auto ClassifyCedarValueEscape(const JsonNode& object, DuplicateKeys duplicates) -> CedarValueEscape;

/// Rust deserializes a whole Cedar value as `RawCedarValueJson` before reading any escape, so every
/// number in it must be an `i64` integer, including those in the parts of an escape that are then
/// dropped (an `__extn` object's extra keys). Checks that of `node` and everything under it.
[[nodiscard]] auto CheckCedarNumbers(const JsonNode& node, DuplicateKeys duplicates) -> std::expected<void, JsonReadError>;

/// Reads `node` as a Cedar value (Rust's `CedarValueJson::into_expr` followed by evaluating the
/// restricted expression), building it in `arena`. `node` must have passed CheckStrictJson.
/// Extension function calls are evaluated; a failing call is an error whose code is the
/// evaluation error's.
[[nodiscard]] auto ReadCedarValue(const JsonNode& node, DuplicateKeys duplicates, ValueArena& arena,
                                  EntityTypeNames names = EntityTypeNames::Intern)
    -> std::expected<Value, JsonReadError>;

/// Reads `node` as a value of the schema type `expected` (Rust's `ValueParser::val_into_restricted_expr`
/// with an expected type, followed by evaluating it), building it in `arena`. The type decides the
/// form: for an entity type the `__entity` escape may be left out, for an extension type the
/// `__extn` escape and even the constructor (a plain string becomes the type's one-argument
/// constructor called on it), a set reads its elements and a record its declared attributes by
/// their types (a missing required attribute or an undeclared one fails). Any other type reads as
/// ReadCedarValue does. The value is not checked to have the type beyond that: entity types,
/// extension function results and primitive values are left to the conformance checks. `node` must
/// have passed CheckStrictJson.
[[nodiscard]] auto ReadTypedCedarValue(const JsonNode& node, const Type& expected, DuplicateKeys duplicates,
                                       ValueArena& arena, EntityTypeNames names)
    -> std::expected<Value, JsonReadError>;

/// Whether a ReadCedarValue of `node` gives a record: an object that is not an `__extn`,
/// `__entity` or `__expr` escape.
[[nodiscard]] auto IsCedarRecord(const JsonNode& node, DuplicateKeys duplicates) -> bool;

/// Reads `node` as an entity uid in any form Rust's `EntityUidJson` accepts: `{"type": .., "id":
/// ..}`, `{"__entity": {...}}`, or serde's sequence form `[type, id]`. `node` must have passed
/// CheckStrictJson.
[[nodiscard]] auto ReadEntityUidJson(const JsonNode& node, DuplicateKeys duplicates,
                                     EntityTypeNames names = EntityTypeNames::Intern)
    -> std::expected<EntityUid, JsonReadError>;

/// The code a JsonSyntaxError reports under, as a JsonReadError.
[[nodiscard]] auto FromSyntaxError(JsonSyntaxError error) -> JsonReadError;

/// `error` as a diagnostic about `source`, prefixed with `context` when it is not empty.
[[nodiscard]] auto ToDiagnostic(JsonReadError error, const SharedSourceText& source, std::string_view context = {})
    -> Diagnostic;

/// A short, single-line rendering of `node` for messages, cut off when long.
[[nodiscard]] auto DescribeJson(const JsonNode& node) -> std::string;

} // namespace cedar::detail
