#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace cedar::detail
{

// A syntax tree of one JSON document, read with simdjson. The tree keeps what Cedar's readers need
// to reproduce serde_json's behavior exactly, which no single strict JSON parse does: Rust reads
// some parts of a document strictly (a number must fit an f64, a `\u` escape must pair its
// surrogates, nesting is limited) and skips others leniently (an ignored field of an entity object
// is checked for syntax only). So the tree checks syntax everywhere and records, per node, what the
// strict checks need: the number token as written, whether a string decodes strictly, and the
// container nesting depth. Duplicate object keys are kept, in document order.

enum class JsonKind : std::uint8_t
{
    Null,
    False,
    True,
    Number,
    String,
    Array,
    Object,
};

struct JsonMember;

struct JsonNode
{
    JsonKind kind = JsonKind::Null;
    /// For a string: whether its escapes decode strictly (no unpaired surrogate). `text` is the
    /// decoded string when true and the raw content when false.
    bool strictString = true;
    /// For a container: its nesting level, 1 for a top-level array or object.
    std::uint32_t depth = 0;
    /// Byte offset of the node's first character in the document, and the token length for a scalar.
    std::uint32_t offset = 0;
    std::uint32_t length = 1;
    /// A number's token as written, or a string's text.
    std::string_view text{};
    /// An array's elements or an object's members (`count` of them).
    const JsonNode* elements = nullptr;
    const JsonMember* members = nullptr;
    std::uint32_t count = 0;

    [[nodiscard]] auto Elements() const noexcept -> std::span<const JsonNode> { return {elements, count}; }
    [[nodiscard]] auto Members() const noexcept -> std::span<const JsonMember>;
    [[nodiscard]] auto IsObject() const noexcept -> bool { return kind == JsonKind::Object; }
    [[nodiscard]] auto IsArray() const noexcept -> bool { return kind == JsonKind::Array; }
    [[nodiscard]] auto IsString() const noexcept -> bool { return kind == JsonKind::String; }
};

struct JsonMember
{
    /// The key, decoded when `strictKey` (raw otherwise; see JsonNode::strictString).
    std::string_view key;
    bool strictKey = true;
    std::uint32_t keyOffset = 0;
    JsonNode value;
};

inline auto JsonNode::Members() const noexcept -> std::span<const JsonMember> { return {members, count}; }

/// A syntax error: where it is and what is wrong.
struct JsonSyntaxError
{
    std::uint32_t offset = 0;
    std::string message;
};

/// The parsed document. Nodes and decoded strings live in the memory resource the reader was
/// given; undecoded text points into the document's padded copy, which this object owns.
class JsonDocument
{
public:
    JsonDocument() = default;
    JsonDocument(const JsonDocument&) = delete;
    auto operator=(const JsonDocument&) -> JsonDocument& = delete;
    JsonDocument(JsonDocument&&) noexcept = default;
    auto operator=(JsonDocument&&) noexcept -> JsonDocument& = default;
    ~JsonDocument() = default;

    [[nodiscard]] auto Root() const noexcept -> const JsonNode& { return root_; }

private:
    friend auto ReadJson(std::string_view text, std::pmr::memory_resource& resource)
        -> std::expected<JsonDocument, JsonSyntaxError>;

    std::unique_ptr<char[]> padded_{};
    JsonNode root_{};
};

/// Parses `text` into a tree whose nodes live in `resource`. Fails on anything that is not one
/// well-formed JSON value (RFC 8259, UTF-8) or that nests deeper than simdjson's limit.
[[nodiscard]] auto ReadJson(std::string_view text, std::pmr::memory_resource& resource)
    -> std::expected<JsonDocument, JsonSyntaxError>;

/// What a number token is, as serde_json reads it (without its `arbitrary_precision` feature).
enum class JsonNumberKind : std::uint8_t
{
    /// An integer that fits `i64`.
    Long,
    /// An integer from 2^63 to 2^64 - 1 (serde_json's `u64`).
    UnsignedLong,
    /// A finite float: a token with a fraction or exponent, `-0`, or an integer past 64 bits.
    Float,
    /// A float too large for an f64. serde_json refuses it ("number out of range").
    OutOfRange,
};

struct JsonNumber
{
    JsonNumberKind kind;
    std::int64_t value; ///< The integer when `kind` is Long.
};

/// Classifies a number token the tree accepted.
[[nodiscard]] auto ClassifyJsonNumber(std::string_view token) noexcept -> JsonNumber;

/// serde_json's nesting limit: it refuses a container nested deeper than this in any part of a
/// document it reads into a value (`remaining_depth` starts at 128).
inline constexpr std::uint32_t MaximumJsonDepth = 127;

/// Appends `text` as a JSON string literal, escaped as serde_json writes it.
void AppendJsonString(std::string& output, std::string_view text);

} // namespace cedar::detail
