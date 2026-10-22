#include "json/JsonReader.hpp"

#include <simdjson.h>

#include <charconv>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <system_error>
#include <vector>

namespace cedar::detail
{

namespace
{

namespace ondemand = simdjson::ondemand;

constexpr std::size_t Padding = simdjson::SIMDJSON_PADDING;

[[nodiscard]] constexpr auto IsJsonWhitespace(const char character) noexcept -> bool
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

[[nodiscard]] constexpr auto TrimTrailingWhitespace(std::string_view text) noexcept -> std::string_view
{
    while (!text.empty() && IsJsonWhitespace(text.back())) { text.remove_suffix(1); }
    return text;
}

[[nodiscard]] constexpr auto IsDigit(const char character) noexcept -> bool
{
    return character >= '0' && character <= '9';
}

/// Whether `token` is a JSON number: `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?`.
[[nodiscard]] constexpr auto IsNumberSyntax(const std::string_view token) noexcept -> bool
{
    std::size_t position = 0;
    const auto digits = [&] {
        const std::size_t start = position;
        while (position < token.size() && IsDigit(token[position])) { ++position; }
        return position - start;
    };
    if (position < token.size() && token[position] == '-') { ++position; }
    if (position >= token.size()) { return false; }
    if (token[position] == '0') { ++position; }
    else if (digits() == 0) { return false; }
    if (position < token.size() && token[position] == '.')
    {
        ++position;
        if (digits() == 0) { return false; }
    }
    if (position < token.size() && (token[position] == 'e' || token[position] == 'E'))
    {
        ++position;
        if (position < token.size() && (token[position] == '+' || token[position] == '-')) { ++position; }
        if (digits() == 0) { return false; }
    }
    return position == token.size();
}

static_assert(IsNumberSyntax("0") && IsNumberSyntax("-0") && IsNumberSyntax("12") && IsNumberSyntax("1.5e-3"));
static_assert(IsNumberSyntax("1E+2") && !IsNumberSyntax("01") && !IsNumberSyntax("1.") && !IsNumberSyntax("-"));
static_assert(!IsNumberSyntax("1e") && !IsNumberSyntax(".5") && !IsNumberSyntax("+1") && !IsNumberSyntax("--1"));

[[nodiscard]] constexpr auto HexValue(const char character) noexcept -> int
{
    if (character >= '0' && character <= '9') { return character - '0'; }
    if (character >= 'a' && character <= 'f') { return character - 'a' + 10; }
    if (character >= 'A' && character <= 'F') { return character - 'A' + 10; }
    return -1;
}

void AppendUtf8(std::string& output, const std::uint32_t scalar)
{
    if (scalar < 0x80) { output.push_back(static_cast<char>(scalar)); }
    else if (scalar < 0x800)
    {
        output.push_back(static_cast<char>(0xC0 | (scalar >> 6)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
    else if (scalar < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0 | (scalar >> 12)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0 | (scalar >> 18)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (scalar & 0x3F)));
    }
}

/// A string's content checked and, when it has escapes, decoded.
struct DecodedString
{
    std::string_view text; ///< Decoded (in the resource) when `strict`, the raw content otherwise.
    bool strict;
};

class TreeBuilder
{
public:
    TreeBuilder(const char* begin, std::pmr::memory_resource& resource) : begin_{begin}, resource_{resource} {}

    [[nodiscard]] auto Error() && -> JsonSyntaxError { return std::move(error_); }

    /// Reads `value` (which nests at `depth` containers) into `node`.
    [[nodiscard]] auto Read(ondemand::value value, const std::uint32_t depth, JsonNode& node) -> bool
    {
        ondemand::json_type type{};
        const char* location = nullptr;
        if (value.current_location().get(location) != simdjson::SUCCESS) { location = begin_; }
        node.offset = OffsetOf(location);
        if (value.type().get(type) != simdjson::SUCCESS) { return Fail(location, "expected a JSON value"); }
        switch (type)
        {
        case ondemand::json_type::array: return ReadArray(value, depth + 1, node);
        case ondemand::json_type::object: return ReadObject(value, depth + 1, node);
        case ondemand::json_type::number:
        {
            std::string_view token = TrimTrailingWhitespace(value.raw_json_token());
            return ReadNumber(token, node);
        }
        case ondemand::json_type::string:
        {
            std::string_view token = TrimTrailingWhitespace(value.raw_json_token());
            return ReadString(token, node);
        }
        case ondemand::json_type::boolean:
        {
            bool flag = false;
            if (value.get_bool().get(flag) != simdjson::SUCCESS) { return Fail(location, "expected `true` or `false`"); }
            node.kind = flag ? JsonKind::True : JsonKind::False;
            node.length = flag ? 4 : 5;
            return true;
        }
        case ondemand::json_type::null:
        {
            bool isNull = false;
            if (value.is_null().get(isNull) != simdjson::SUCCESS || !isNull) { return Fail(location, "expected `null`"); }
            node.kind = JsonKind::Null;
            node.length = 4;
            return true;
        }
        default: break;
        }
        return Fail(location, "expected a JSON value");
    }

    /// Reads a scalar top-level document, which ends at `end`, into `node`.
    [[nodiscard]] auto ReadScalarDocument(ondemand::document& document, const char* end, JsonNode& node) -> bool
    {
        std::string_view token{};
        if (document.raw_json_token().get(token) != simdjson::SUCCESS) { return Fail(begin_, "expected a JSON value"); }
        node.offset = OffsetOf(token.data());
        // A scalar document's token runs to the end of the text; anything after the value is in it.
        if (token.data() + token.size() != end) { return Fail(token.data() + token.size(), "trailing characters after the JSON value"); }
        token = TrimTrailingWhitespace(token);
        ondemand::json_type type{};
        if (document.type().get(type) != simdjson::SUCCESS) { return Fail(token.data(), "expected a JSON value"); }
        switch (type)
        {
        case ondemand::json_type::number: return ReadNumber(token, node);
        case ondemand::json_type::string: return ReadString(token, node);
        case ondemand::json_type::boolean:
        {
            bool flag = false;
            if (document.get_bool().get(flag) != simdjson::SUCCESS) { return Fail(token.data(), "expected `true` or `false`"); }
            node.kind = flag ? JsonKind::True : JsonKind::False;
            return true;
        }
        case ondemand::json_type::null:
        {
            bool isNull = false;
            if (document.is_null().get(isNull) != simdjson::SUCCESS || !isNull) { return Fail(token.data(), "expected `null`"); }
            node.kind = JsonKind::Null;
            return true;
        }
        default: break;
        }
        return Fail(token.data(), "expected a JSON value");
    }

    auto Fail(const char* location, std::string message) -> bool
    {
        error_ = JsonSyntaxError{.offset = OffsetOf(location), .message = std::move(message)};
        return false;
    }

    auto Fail(const simdjson::error_code code, const char* location) -> bool
    {
        return Fail(location, std::format("malformed JSON ({})", simdjson::error_message(code)));
    }

private:
    [[nodiscard]] auto OffsetOf(const char* location) const noexcept -> std::uint32_t
    {
        return static_cast<std::uint32_t>(location - begin_);
    }

    [[nodiscard]] auto ReadArray(ondemand::value value, const std::uint32_t depth, JsonNode& node) -> bool
    {
        const char* location = begin_ + node.offset;
        node.kind = JsonKind::Array;
        node.depth = depth;
        ondemand::array array;
        if (const auto code = value.get_array().get(array)) { return Fail(code, location); }
        std::vector<JsonNode> elements;
        for (auto element : array)
        {
            ondemand::value child;
            if (const auto code = std::move(element).get(child)) { return Fail(code, location); }
            JsonNode& added = elements.emplace_back();
            if (!Read(child, depth, added)) { return false; }
        }
        if (elements.empty()) { return true; }
        auto* storage = static_cast<JsonNode*>(resource_.allocate(sizeof(JsonNode) * elements.size(), alignof(JsonNode)));
        std::uninitialized_copy(elements.begin(), elements.end(), storage);
        node.elements = storage;
        node.count = static_cast<std::uint32_t>(elements.size());
        return true;
    }

    [[nodiscard]] auto ReadObject(ondemand::value value, const std::uint32_t depth, JsonNode& node) -> bool
    {
        const char* location = begin_ + node.offset;
        node.kind = JsonKind::Object;
        node.depth = depth;
        ondemand::object object;
        if (const auto code = value.get_object().get(object)) { return Fail(code, location); }
        std::vector<JsonMember> members;
        for (auto field : object)
        {
            ondemand::field member;
            if (const auto code = std::move(field).get(member)) { return Fail(code, location); }
            const std::string_view rawKey = member.escaped_key();
            JsonMember& added = members.emplace_back();
            added.keyOffset = OffsetOf(rawKey.data());
            const auto key = Decode(rawKey);
            if (!key) { return false; }
            added.key = key->text;
            added.strictKey = key->strict;
            if (!Read(member.value(), depth, added.value)) { return false; }
        }
        if (members.empty()) { return true; }
        auto* storage =
            static_cast<JsonMember*>(resource_.allocate(sizeof(JsonMember) * members.size(), alignof(JsonMember)));
        std::uninitialized_copy(members.begin(), members.end(), storage);
        node.members = storage;
        node.count = static_cast<std::uint32_t>(members.size());
        return true;
    }

    [[nodiscard]] auto ReadNumber(const std::string_view token, JsonNode& node) -> bool
    {
        if (!IsNumberSyntax(token)) { return Fail(token.data(), "malformed number"); }
        node.kind = JsonKind::Number;
        node.text = token;
        node.length = static_cast<std::uint32_t>(token.size());
        return true;
    }

    [[nodiscard]] auto ReadString(const std::string_view token, JsonNode& node) -> bool
    {
        if (token.size() < 2 || token.front() != '"' || token.back() != '"')
        {
            return Fail(token.data(), "malformed string");
        }
        const auto decoded = Decode(token.substr(1, token.size() - 2));
        if (!decoded) { return false; }
        node.kind = JsonKind::String;
        node.text = decoded->text;
        node.strictString = decoded->strict;
        node.length = static_cast<std::uint32_t>(token.size());
        return true;
    }

    /// Checks a string's raw content the way serde_json skips a string (no unescaped control
    /// character, every escape well formed) and decodes it the way serde_json reads one (a `\u`
    /// escape of a surrogate must be a high surrogate followed by a `\u` low surrogate).
    [[nodiscard]] auto Decode(const std::string_view raw) -> std::optional<DecodedString>
    {
        bool hasEscape = false;
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            const char character = raw[index];
            if (static_cast<unsigned char>(character) < 0x20)
            {
                Fail(raw.data() + index, "control character (U+0000 to U+001F) in a string");
                return std::nullopt;
            }
            if (character != '\\') { continue; }
            hasEscape = true;
            if (index + 1 >= raw.size())
            {
                Fail(raw.data() + index, "malformed escape in a string");
                return std::nullopt;
            }
            const char escape = raw[++index];
            if (escape == 'u')
            {
                for (std::size_t digit = 1; digit <= 4; ++digit)
                {
                    if (index + digit >= raw.size() || HexValue(raw[index + digit]) < 0)
                    {
                        Fail(raw.data() + index, "malformed `\\u` escape in a string");
                        return std::nullopt;
                    }
                }
                index += 4;
                continue;
            }
            if (std::string_view{"\"\\/bfnrt"}.find(escape) == std::string_view::npos)
            {
                Fail(raw.data() + index - 1, "malformed escape in a string");
                return std::nullopt;
            }
        }
        if (!hasEscape) { return DecodedString{raw, true}; }

        std::string decoded;
        decoded.reserve(raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            if (raw[index] != '\\')
            {
                decoded.push_back(raw[index]);
                continue;
            }
            const char escape = raw[++index];
            switch (escape)
            {
            case 'b': decoded.push_back('\b'); continue;
            case 'f': decoded.push_back('\f'); continue;
            case 'n': decoded.push_back('\n'); continue;
            case 'r': decoded.push_back('\r'); continue;
            case 't': decoded.push_back('\t'); continue;
            case 'u': break;
            default: decoded.push_back(escape); continue;
            }
            const auto hex = [&](const std::size_t at) {
                std::uint32_t result = 0;
                for (std::size_t digit = 0; digit < 4; ++digit)
                {
                    result = result * 16 + static_cast<std::uint32_t>(HexValue(raw[at + digit]));
                }
                return result;
            };
            std::uint32_t scalar = hex(index + 1);
            index += 4;
            if (scalar >= 0xDC00 && scalar <= 0xDFFF) { return DecodedString{raw, false}; }
            if (scalar >= 0xD800 && scalar <= 0xDBFF)
            {
                if (index + 6 >= raw.size()) { return DecodedString{raw, false}; }
                if (raw[index + 1] != '\\' || raw[index + 2] != 'u') { return DecodedString{raw, false}; }
                const std::uint32_t low = hex(index + 3);
                if (low < 0xDC00 || low > 0xDFFF) { return DecodedString{raw, false}; }
                scalar = 0x10000 + ((scalar - 0xD800) << 10) + (low - 0xDC00);
                index += 6;
            }
            AppendUtf8(decoded, scalar);
        }
        auto* storage = static_cast<char*>(resource_.allocate(decoded.size() == 0 ? 1 : decoded.size(), 1));
        std::memcpy(storage, decoded.data(), decoded.size());
        return DecodedString{std::string_view{storage, decoded.size()}, true};
    }

    const char* begin_;
    std::pmr::memory_resource& resource_;
    JsonSyntaxError error_{};
};

} // namespace

auto ReadJson(const std::string_view text, std::pmr::memory_resource& resource)
    -> std::expected<JsonDocument, JsonSyntaxError>
{
    if (text.size() >= std::numeric_limits<std::uint32_t>::max() - Padding)
    {
        return std::unexpected{JsonSyntaxError{.offset = 0, .message = "the JSON text is 4 GiB or larger"}};
    }
    // simdjson skips a leading byte order mark; serde_json, like RFC 8259 parsers generally, does not.
    if (text.starts_with("\xEF\xBB\xBF"))
    {
        return std::unexpected{JsonSyntaxError{.offset = 0, .message = "a byte order mark is not JSON"}};
    }
    JsonDocument document;
    document.padded_ = std::make_unique_for_overwrite<char[]>(text.size() + Padding);
    std::memcpy(document.padded_.get(), text.data(), text.size());
    std::memset(document.padded_.get() + text.size(), 0, Padding);

    const char* begin = document.padded_.get();
    TreeBuilder builder{begin, resource};
    ondemand::parser parser;
    ondemand::document parsed;
    if (const auto code = parser.iterate(begin, text.size(), text.size() + Padding).get(parsed))
    {
        builder.Fail(code, begin);
        return std::unexpected{std::move(builder).Error()};
    }
    ondemand::json_type type{};
    if (const auto code = parsed.type().get(type))
    {
        builder.Fail(code, begin);
        return std::unexpected{std::move(builder).Error()};
    }
    if (type != ondemand::json_type::array && type != ondemand::json_type::object)
    {
        if (!builder.ReadScalarDocument(parsed, begin + text.size(), document.root_))
        {
            return std::unexpected{std::move(builder).Error()};
        }
        return document;
    }
    ondemand::value root;
    if (const auto code = parsed.get_value().get(root))
    {
        builder.Fail(code, begin);
        return std::unexpected{std::move(builder).Error()};
    }
    if (!builder.Read(root, 0, document.root_)) { return std::unexpected{std::move(builder).Error()}; }
    if (!parsed.at_end())
    {
        const char* location = begin + text.size();
        if (const char* current = nullptr; parsed.current_location().get(current) == simdjson::SUCCESS)
        {
            location = current;
        }
        builder.Fail(location, "trailing characters after the JSON value");
        return std::unexpected{std::move(builder).Error()};
    }
    return document;
}

auto ClassifyJsonNumber(const std::string_view token) noexcept -> JsonNumber
{
    const bool isInteger = token.find_first_of(".eE") == std::string_view::npos;
    if (isInteger)
    {
        // serde_json reads `-0` as the float -0.0, not the integer 0.
        if (token == "-0") { return {JsonNumberKind::Float, 0}; }
        std::int64_t value = 0;
        if (const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            error == std::errc{} && end == token.data() + token.size())
        {
            return {JsonNumberKind::Long, value};
        }
        std::uint64_t unsignedValue = 0;
        if (const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), unsignedValue);
            error == std::errc{} && end == token.data() + token.size())
        {
            return {JsonNumberKind::UnsignedLong, 0};
        }
    }
    double value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc::result_out_of_range) { return {JsonNumberKind::Float, 0}; }
    // Out of range is overflow or underflow. serde_json reads an underflow as 0 and refuses an
    // overflow. The decimal exponent of the leading significant digit tells them apart.
    std::size_t position = token.starts_with('-') ? 1 : 0;
    std::int64_t leadingExponent = 0;
    std::int64_t integerDigits = 0;
    while (position < token.size() && IsDigit(token[position]))
    {
        ++integerDigits;
        ++position;
    }
    // Skip the integer part's leading zeros ("0" is the only form JSON allows with a leading zero).
    bool significantFound = !(integerDigits == 1 && token[token.starts_with('-') ? 1 : 0] == '0');
    if (significantFound) { leadingExponent = integerDigits - 1; }
    if (position < token.size() && token[position] == '.')
    {
        ++position;
        std::int64_t fractionIndex = 0;
        while (position < token.size() && IsDigit(token[position]))
        {
            ++fractionIndex;
            if (!significantFound && token[position] != '0')
            {
                significantFound = true;
                leadingExponent = -fractionIndex;
            }
            ++position;
        }
    }
    if (position < token.size() && (token[position] == 'e' || token[position] == 'E'))
    {
        ++position;
        const bool negative = position < token.size() && token[position] == '-';
        if (position < token.size() && (token[position] == '+' || token[position] == '-')) { ++position; }
        std::int64_t exponent = 0;
        while (position < token.size() && IsDigit(token[position]))
        {
            if (exponent < 1'000'000'000) { exponent = exponent * 10 + (token[position] - '0'); }
            ++position;
        }
        leadingExponent += negative ? -exponent : exponent;
    }
    if (!significantFound || leadingExponent < 0) { return {JsonNumberKind::Float, 0}; }
    return {JsonNumberKind::OutOfRange, 0};
}

void AppendJsonString(std::string& output, const std::string_view text)
{
    constexpr std::string_view Hex = "0123456789abcdef";
    output.push_back('"');
    for (const char character : text)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20)
            {
                const auto byte = static_cast<unsigned char>(character);
                output += "\\u00";
                output.push_back(Hex[byte >> 4]);
                output.push_back(Hex[byte & 0xF]);
            }
            else
            {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
}

} // namespace cedar::detail
