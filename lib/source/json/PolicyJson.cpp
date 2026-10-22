// The JSON policy format (Rust's EST): policies, templates and policy sets in and out
// (`cedar-policy-core/src/est.rs`, `est/{expr,scope_constraints,policy_set,annotation}.rs`).
//
// The reader follows serde's derived deserializers over a `serde_json::Value`, which is how the Rust
// CLI and `Policy::from_json` see a document: every repeated key has already become its last value,
// so a document is first checked strictly (CheckStrictJson) and then each object is read through
// its effective members. The shapes, oracle-checked:
// - A derived struct (policy, clause, template link, policy set, the `is` scope constraint) refuses
//   unknown fields and also reads serde's sequence form (fields in declaration order).
// - Expression operators are externally tagged struct variants: objects only, unknown fields
//   refused. `has` is an untagged enum: unknown fields are ignored.
// - Scope constraints are internally tagged by `op`; their `==` and `in` payloads are untagged
//   enums, so unknown fields are ignored and the sequence form never matches.
// - A unit variant (`effect`, `Var`, `Slot`, a clause's `kind`, `Wildcard`) is a string or an
//   object of one key whose value is null; a tag (`op`, a clause's `kind` in sequence form) is a
//   string only.

#include "json/PolicyJson.hpp"

#include "extensions/ExtensionCalls.hpp"
#include "json/JsonReader.hpp"
#include "json/ValueJson.hpp"
#include "model/PolicySetAccess.hpp"
#include "syntax/Folding.hpp"

#include <cedar/Json.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <memory_resource>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cedar::detail
{

namespace
{

template <typename T>
using Read = std::expected<T, JsonReadError>;

[[nodiscard]] auto ErrorAt(const JsonNode& node, const DiagnosticCode code, std::string message,
                           std::optional<std::string> help = std::nullopt) -> std::unexpected<JsonReadError>
{
    return std::unexpected{JsonReadError{
        .code = code, .offset = node.offset, .length = node.length, .message = std::move(message), .help = std::move(help)}};
}

[[nodiscard]] auto ShapeError(const JsonNode& node, std::string message) -> std::unexpected<JsonReadError>
{
    return ErrorAt(node, DiagnosticCode::JsonUnexpectedShape, std::move(message));
}

/// The members of `object` as a `serde_json::Value` holds them: each key once, with its last
/// value, in the order the keys first appear (the CLI's serde_json keeps insertion order).
[[nodiscard]] auto ValueMembers(const JsonNode& object) -> std::vector<const JsonMember*>
{
    std::vector<const JsonMember*> members;
    const std::span<const JsonMember> all = object.Members();
    if (all.size() <= 1)
    {
        for (const JsonMember& member : all) { members.push_back(&member); }
        return members;
    }
    std::unordered_map<std::string_view, std::size_t> last;
    last.reserve(all.size());
    for (std::size_t index = 0; index < all.size(); ++index) { last[all[index].key] = index; }
    std::unordered_map<std::string_view, bool> emitted;
    emitted.reserve(last.size());
    members.reserve(last.size());
    for (const JsonMember& member : all)
    {
        if (emitted.try_emplace(member.key, true).second) { members.push_back(&all[last[member.key]]); }
    }
    return members;
}

[[nodiscard]] auto FindMember(const std::vector<const JsonMember*>& members, const std::string_view key)
    -> const JsonNode*
{
    for (const JsonMember* member : members)
    {
        if (member->key == key) { return &member->value; }
    }
    return nullptr;
}

/// The fields of a derived struct read from `node`: an object (`denyUnknown`: a key that is not a
/// field fails) or, with `allowSequence`, an array of `minimumElements` to `Count` elements in
/// field order. A field not given is null; the caller decides which are required.
template <std::size_t Count>
[[nodiscard]] auto ReadStruct(const JsonNode& node, const std::string_view structName,
                              const std::array<std::string_view, Count>& names, const bool allowSequence,
                              const std::size_t minimumElements, const bool denyUnknown)
    -> Read<std::array<const JsonNode*, Count>>
{
    std::array<const JsonNode*, Count> fields{};
    if (node.IsArray() && allowSequence)
    {
        if (node.count < minimumElements || node.count > Count)
        {
            return ShapeError(node, std::format("invalid length {}, expected {} written as an array of {} to {} elements",
                                                node.count, structName, minimumElements, Count));
        }
        for (std::size_t index = 0; index < node.count; ++index) { fields[index] = &node.Elements()[index]; }
        return fields;
    }
    if (!node.IsObject())
    {
        return ShapeError(node, std::format("expected {} as an object, but got `{}`", structName, DescribeJson(node)));
    }
    for (const JsonMember* member : ValueMembers(node))
    {
        bool known = false;
        for (std::size_t index = 0; index < Count; ++index)
        {
            if (member->key != names[index]) { continue; }
            fields[index] = &member->value;
            known = true;
        }
        if (!known && denyUnknown)
        {
            return ErrorAt(member->value, DiagnosticCode::JsonUnexpectedShape,
                           std::format("unknown field `{}` in {}", member->key, structName));
        }
    }
    return fields;
}

[[nodiscard]] auto Require(const JsonNode* field, const JsonNode& owner, const std::string_view name) -> Read<const JsonNode*>
{
    if (field == nullptr) { return ShapeError(owner, std::format("missing field `{}`", name)); }
    return field;
}

[[nodiscard]] auto ReadString(const JsonNode& node, const std::string_view what) -> Read<std::string_view>
{
    if (!node.IsString()) { return ShapeError(node, std::format("expected {} as a string, but got `{}`", what, DescribeJson(node))); }
    return node.text;
}

/// A unit variant of an externally tagged enum: the variant's name as a string, or, when
/// `allowObject`, an object of exactly one key (the name) whose value is null.
template <std::size_t Count>
[[nodiscard]] auto ReadUnitVariant(const JsonNode& node, const std::string_view enumName,
                                   const std::array<std::string_view, Count>& variants, const bool allowObject)
    -> Read<std::size_t>
{
    std::string_view name;
    if (node.IsString()) { name = node.text; }
    else if (node.IsObject() && allowObject)
    {
        const std::vector<const JsonMember*> members = ValueMembers(node);
        if (members.size() != 1)
        {
            return ShapeError(node, std::format("expected {} as an object with a single key", enumName));
        }
        if (members.front()->value.kind != JsonKind::Null)
        {
            return ShapeError(members.front()->value, std::format("the {} `{}` takes no value; expected `null`",
                                                                  enumName, members.front()->key));
        }
        name = members.front()->key;
    }
    else { return ShapeError(node, std::format("expected {}, but got `{}`", enumName, DescribeJson(node))); }
    for (std::size_t index = 0; index < Count; ++index)
    {
        if (variants[index] == name) { return index; }
    }
    return ShapeError(node, std::format("unknown {} `{}`", enumName, name));
}

[[nodiscard]] auto ReadSlotId(const JsonNode& node) -> Read<SlotId>
{
    constexpr std::array<std::string_view, 2> Slots{"?principal", "?resource"};
    auto slot = ReadUnitVariant(node, "slot", Slots, true);
    if (!slot) { return std::unexpected{std::move(slot).error()}; }
    return *slot == 0 ? SlotId::Principal : SlotId::Resource;
}

[[nodiscard]] auto ReadEntityType(const JsonNode& node) -> Read<EntityType>
{
    auto text = ReadString(node, "an entity type");
    if (!text) { return std::unexpected{std::move(text).error()}; }
    auto type = EntityType::Parse(*text);
    if (!type)
    {
        return ErrorAt(node, DiagnosticCode::InvalidIsType, std::format("invalid entity type `{}`", *text),
                       "an entity type is a name such as `User` or `Namespace::User`, outside the `__cedar` namespace");
    }
    return *std::move(type);
}

// ---- Expressions ----

enum class Operator : std::uint8_t
{
    Binary,
    NotEqual,
    Greater,
    GreaterEqual,
    And,
    Or,
};

struct BinaryOperatorName
{
    std::string_view name;
    Operator form;
    BinaryOperator operation;
};

constexpr std::array BinaryOperatorNames{
    BinaryOperatorName{"==", Operator::Binary, BinaryOperator::Equal},
    BinaryOperatorName{"!=", Operator::NotEqual, BinaryOperator::Equal},
    BinaryOperatorName{"in", Operator::Binary, BinaryOperator::In},
    BinaryOperatorName{"<", Operator::Binary, BinaryOperator::Less},
    BinaryOperatorName{"<=", Operator::Binary, BinaryOperator::LessEqual},
    BinaryOperatorName{">", Operator::Greater, BinaryOperator::LessEqual},
    BinaryOperatorName{">=", Operator::GreaterEqual, BinaryOperator::Less},
    BinaryOperatorName{"&&", Operator::And, BinaryOperator::Equal},
    BinaryOperatorName{"||", Operator::Or, BinaryOperator::Equal},
    BinaryOperatorName{"+", Operator::Binary, BinaryOperator::Add},
    BinaryOperatorName{"-", Operator::Binary, BinaryOperator::Subtract},
    BinaryOperatorName{"*", Operator::Binary, BinaryOperator::Multiply},
    BinaryOperatorName{"contains", Operator::Binary, BinaryOperator::Contains},
    BinaryOperatorName{"containsAll", Operator::Binary, BinaryOperator::ContainsAll},
    BinaryOperatorName{"containsAny", Operator::Binary, BinaryOperator::ContainsAny},
    BinaryOperatorName{"getTag", Operator::Binary, BinaryOperator::GetTag},
    BinaryOperatorName{"hasTag", Operator::Binary, BinaryOperator::HasTag},
};

/// Builds the expressions of one policy into one arena (Rust's `est::Expr` deserialization followed
/// by `try_into_ast`).
class ExpressionReader final
{
public:
    explicit ExpressionReader(ExpressionArena& arena) noexcept : arena_{&arena} {}

    /// Whether a slot was read since the last ClearSlots.
    [[nodiscard]] auto SawSlot() const noexcept -> bool { return sawSlot_; }
    [[nodiscard]] auto Arena() const noexcept -> ExpressionArena& { return *arena_; }
    auto ClearSlots() noexcept -> void { sawSlot_ = false; }

    [[nodiscard]] auto ReadExpression(const JsonNode& node) -> Read<ExpressionIndex>
    {
        if (!node.IsObject())
        {
            return ShapeError(node, std::format("expected a JSON object representing an expression, but got `{}`",
                                                DescribeJson(node)));
        }
        const std::vector<const JsonMember*> members = ValueMembers(node);
        if (members.empty())
        {
            return ErrorAt(node, DiagnosticCode::JsonMissingOperator, "an empty object is not an expression",
                           "an expression is an object with one key, its operator, such as `{\"Var\": \"principal\"}`");
        }
        if (members.size() > 1)
        {
            return ErrorAt(node, DiagnosticCode::JsonMultipleOperators,
                           std::format("an expression object has exactly one key, its operator, but this one has `{}` "
                                       "and `{}`",
                                       members[0]->key, members[1]->key));
        }
        const std::string_view key = members.front()->key;
        const JsonNode& value = members.front()->value;
        // No operator name is an extension function name, so a known function name is a call.
        if (const ExtensionCall* function = FindExtensionCall(key)) { return ReadExtensionCall(*function, value); }
        for (const BinaryOperatorName& entry : BinaryOperatorNames)
        {
            if (entry.name == key) { return ReadBinary(entry, value); }
        }
        if (key == "Value")
        {
            if (auto checked = CheckCedarNumbers(value, DuplicateKeys::LastWins); !checked)
            {
                return std::unexpected{std::move(checked).error()};
            }
            return ReadCedarValue(value);
        }
        if (key == "Var") { return ReadVariable(value); }
        if (key == "Slot")
        {
            auto slot = ReadSlotId(value);
            if (!slot) { return std::unexpected{std::move(slot).error()}; }
            sawSlot_ = true;
            return Builder().Slot(*slot);
        }
        if (key == "!" || key == "neg" || key == "isEmpty")
        {
            const UnaryOperator operation = key == "!"     ? UnaryOperator::Not
                                            : key == "neg" ? UnaryOperator::Negate
                                                           : UnaryOperator::IsEmpty;
            return ReadUnary(operation, key, value);
        }
        if (key == ".") { return ReadGetAttribute(value); }
        if (key == "has") { return ReadHasAttribute(value); }
        if (key == "like") { return ReadLike(value); }
        if (key == "is") { return ReadIs(value); }
        if (key == "if-then-else") { return ReadIf(value); }
        if (key == "Set") { return ReadSet(value); }
        if (key == "Record") { return ReadRecord(value); }
        return ShapeError(node, std::format("unknown expression operator `{}`", key));
    }

    /// Rust's `CedarValueJson::into_expr`: a literal, set, record, entity or extension call, kept
    /// as an expression (an extension call is not evaluated, so a bad argument fails at evaluation).
    [[nodiscard]] auto ReadCedarValue(const JsonNode& node) -> Read<ExpressionIndex>
    {
        switch (node.kind)
        {
        case JsonKind::Null:
            return ErrorAt(node, DiagnosticCode::NullValue, "found a JSON `null`, which is not a Cedar value");
        case JsonKind::False: return Builder().Bool(false);
        case JsonKind::True: return Builder().Bool(true);
        case JsonKind::Number: return Builder().Long(ClassifyJsonNumber(node.text).value);
        case JsonKind::String: return Builder().String(node.text);
        case JsonKind::Array:
        {
            std::vector<ExpressionIndex> elements;
            elements.reserve(node.count);
            for (const JsonNode& element : node.Elements())
            {
                auto read = ReadCedarValue(element);
                if (!read) { return read; }
                elements.push_back(*read);
            }
            return BuildSet(elements);
        }
        case JsonKind::Object: break;
        }
        const CedarValueEscape escape = ClassifyCedarValueEscape(node, DuplicateKeys::LastWins);
        switch (escape.kind)
        {
        case CedarValueEscapeKind::Expression:
            return ErrorAt(node, DiagnosticCode::ExpressionTag, "the `__expr` escape is not supported",
                           "write an entity reference with `__entity`, an extension value with `__extn`, and any "
                           "other value as plain JSON");
        case CedarValueEscapeKind::Entity:
        {
            auto uid = MakeEntityUid(escape.entity);
            if (!uid) { return std::unexpected{std::move(uid).error()}; }
            return Builder().Entity(*uid);
        }
        case CedarValueEscapeKind::Extension:
        {
            const std::string_view text = escape.function->text;
            auto function = Name::Parse(text);
            if (!function)
            {
                return ErrorAt(*escape.function, DiagnosticCode::InvalidEscapeValue,
                               std::format("`{}` is not a valid extension function name", text));
            }
            std::vector<ExpressionIndex> arguments;
            const auto read = [&](const JsonNode& argument) -> Read<void> {
                auto value = ReadCedarValue(argument);
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
            return BuildCall(*function, arguments);
        }
        case CedarValueEscapeKind::None: break;
        }
        std::vector<RecordLiteralField> fields;
        for (const JsonMember* member : ValueMembers(node))
        {
            auto value = ReadCedarValue(member->value);
            if (!value) { return value; }
            fields.push_back(RecordLiteralField{member->key, *value});
        }
        return BuildRecord(node, fields);
    }

private:
    [[nodiscard]] auto Builder() const noexcept -> ExpressionBuilder { return ExpressionBuilder{*arena_}; }

    [[nodiscard]] auto BuildSet(const std::span<const ExpressionIndex> elements) -> ExpressionIndex
    {
        const ExpressionIndex set = Builder().SetLiteral(elements);
        return FoldConstantSet(*arena_, Builder(), set, elements, std::nullopt);
    }

    [[nodiscard]] auto BuildRecord(const JsonNode& node, const std::span<const RecordLiteralField> fields)
        -> Read<ExpressionIndex>
    {
        auto record = Builder().RecordLiteral(fields);
        if (!record)
        {
            return ErrorAt(node, DiagnosticCode::DuplicateRecordKey,
                           std::format("the key `{}` appears more than once in a record", record.error().key));
        }
        return *record;
    }

    [[nodiscard]] auto BuildCall(const Name& function, const std::span<const ExpressionIndex> arguments)
        -> ExpressionIndex
    {
        const ExpressionIndex call = Builder().ExtensionCall(function, arguments);
        const ExtensionCall* row = function.IsUnqualified() ? FindExtensionCall(function.QualifiedText()) : nullptr;
        if (row == nullptr) { return call; }
        return FoldConstructorCall(*arena_, Builder(), *row, call, arguments, std::nullopt);
    }

    /// `{"name": [arguments...]}`: Rust's `ExtFuncCall`. A method's receiver is the first argument.
    [[nodiscard]] auto ReadExtensionCall(const ExtensionCall& function, const JsonNode& value) -> Read<ExpressionIndex>
    {
        if (!value.IsArray())
        {
            return ShapeError(value, std::format("the arguments of `{}` are written as an array, but got `{}`",
                                                 function.name, DescribeJson(value)));
        }
        std::vector<ExpressionIndex> arguments;
        arguments.reserve(value.count);
        for (const JsonNode& element : value.Elements())
        {
            auto argument = ReadExpression(element);
            if (!argument) { return argument; }
            arguments.push_back(*argument);
        }
        const auto name = Name::Parse(function.name);
        return BuildCall(*name, arguments);
    }

    [[nodiscard]] auto ReadOperand(const JsonNode* field, const JsonNode& owner, const std::string_view name)
        -> Read<ExpressionIndex>
    {
        auto required = Require(field, owner, name);
        if (!required) { return std::unexpected{std::move(required).error()}; }
        return ReadExpression(**required);
    }

    [[nodiscard]] auto ReadBinary(const BinaryOperatorName& entry, const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 2> Names{"left", "right"};
        auto fields = ReadStruct(value, std::format("the operands of `{}`", entry.name), Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        auto left = ReadOperand((*fields)[0], value, "left");
        if (!left) { return left; }
        auto right = ReadOperand((*fields)[1], value, "right");
        if (!right) { return right; }
        ExpressionBuilder builder = Builder();
        switch (entry.form)
        {
        case Operator::Binary: return builder.Binary(entry.operation, *left, *right);
        case Operator::NotEqual: return builder.NotEqual(*left, *right);
        case Operator::Greater: return builder.Greater(*left, *right);
        case Operator::GreaterEqual: return builder.GreaterEqual(*left, *right);
        case Operator::And: return builder.And(*left, *right);
        case Operator::Or: return builder.Or(*left, *right);
        }
        return *left;
    }

    [[nodiscard]] auto ReadUnary(const UnaryOperator operation, const std::string_view key, const JsonNode& value)
        -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 1> Names{"arg"};
        auto fields = ReadStruct(value, std::format("the operand of `{}`", key), Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        auto operand = ReadOperand((*fields)[0], value, "arg");
        if (!operand) { return operand; }
        return Builder().Unary(operation, *operand);
    }

    [[nodiscard]] auto ReadVariable(const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 4> Variables{"principal", "action", "resource", "context"};
        auto variable = ReadUnitVariant(value, "variable", Variables, true);
        if (!variable) { return std::unexpected{std::move(variable).error()}; }
        return Builder().Variable(static_cast<Variable>(*variable));
    }

    [[nodiscard]] auto ReadGetAttribute(const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 2> Names{"left", "attr"};
        auto fields = ReadStruct(value, "the operands of `.`", Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        auto object = ReadOperand((*fields)[0], value, "left");
        if (!object) { return object; }
        auto attributeNode = Require((*fields)[1], value, "attr");
        if (!attributeNode) { return std::unexpected{std::move(attributeNode).error()}; }
        auto attribute = ReadString(**attributeNode, "the attribute");
        if (!attribute) { return std::unexpected{std::move(attribute).error()}; }
        return Builder().GetAttribute(*object, *attribute);
    }

    /// Rust's untagged `HasAttrRepr`: `{"left", "attr"}` with `attr` an attribute or a non-empty
    /// array of them (the extended `has`); other keys are ignored.
    [[nodiscard]] auto ReadHasAttribute(const JsonNode& value) -> Read<ExpressionIndex>
    {
        if (!value.IsObject())
        {
            return ShapeError(value, std::format("expected the operands of `has` as an object, but got `{}`",
                                                 DescribeJson(value)));
        }
        const std::vector<const JsonMember*> members = ValueMembers(value);
        auto object = ReadOperand(FindMember(members, "left"), value, "left");
        if (!object) { return object; }
        auto attributeNode = Require(FindMember(members, "attr"), value, "attr");
        if (!attributeNode) { return std::unexpected{std::move(attributeNode).error()}; }
        const JsonNode& attribute = **attributeNode;
        if (attribute.IsString()) { return Builder().HasAttribute(*object, attribute.text); }
        if (!attribute.IsArray() || attribute.count == 0)
        {
            return ShapeError(attribute, std::format("`has` tests an attribute name or a non-empty array of them, but "
                                                     "got `{}`",
                                                     DescribeJson(attribute)));
        }
        std::vector<std::string_view> path;
        path.reserve(attribute.count);
        for (const JsonNode& element : attribute.Elements())
        {
            auto name = ReadString(element, "an attribute name");
            if (!name) { return std::unexpected{std::move(name).error()}; }
            path.push_back(*name);
        }
        return Builder().HasAttribute(*object, path);
    }

    [[nodiscard]] auto ReadLike(const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 2> Names{"left", "pattern"};
        auto fields = ReadStruct(value, "the operands of `like`", Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        auto operand = ReadOperand((*fields)[0], value, "left");
        if (!operand) { return operand; }
        auto patternNode = Require((*fields)[1], value, "pattern");
        if (!patternNode) { return std::unexpected{std::move(patternNode).error()}; }
        const JsonNode& pattern = **patternNode;
        if (!pattern.IsArray())
        {
            return ShapeError(pattern, std::format("a pattern is an array of `\"Wildcard\"` and `{{\"Literal\": \"...\"}}` "
                                                   "elements, but got `{}`",
                                                   DescribeJson(pattern)));
        }
        // Pattern keeps the literal runs between wildcards: n wildcards, n + 1 runs.
        std::vector<std::string> runs(1);
        for (const JsonNode& element : pattern.Elements())
        {
            auto piece = ReadPatternElement(element);
            if (!piece) { return std::unexpected{std::move(piece).error()}; }
            if (*piece) { runs.back() += **piece; }
            else { runs.emplace_back(); }
        }
        const std::vector<std::string_view> literals(runs.begin(), runs.end());
        return Builder().Like(*operand, literals);
    }

    /// One pattern element: the literal text, or nothing for a wildcard (Rust's externally tagged
    /// `PatternElem`: the unit `Wildcard` and the newtype `Literal`).
    [[nodiscard]] static auto ReadPatternElement(const JsonNode& element) -> Read<std::optional<std::string_view>>
    {
        if (element.IsString())
        {
            if (element.text == "Wildcard") { return std::nullopt; }
            if (element.text == "Literal") { return ShapeError(element, "`Literal` takes the literal text"); }
            return ShapeError(element, std::format("unknown pattern element `{}`", element.text));
        }
        if (!element.IsObject())
        {
            return ShapeError(element, std::format("expected a pattern element, but got `{}`", DescribeJson(element)));
        }
        const std::vector<const JsonMember*> members = ValueMembers(element);
        if (members.size() != 1) { return ShapeError(element, "expected a pattern element as an object with a single key"); }
        const JsonNode& payload = members.front()->value;
        if (members.front()->key == "Wildcard")
        {
            if (payload.kind != JsonKind::Null) { return ShapeError(payload, "`Wildcard` takes no value; expected `null`"); }
            return std::nullopt;
        }
        if (members.front()->key == "Literal")
        {
            auto text = ReadString(payload, "the literal text of a pattern");
            if (!text) { return std::unexpected{std::move(text).error()}; }
            return std::optional{*text};
        }
        return ShapeError(element, std::format("unknown pattern element `{}`", members.front()->key));
    }

    [[nodiscard]] auto ReadIs(const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 3> Names{"left", "entity_type", "in"};
        auto fields = ReadStruct(value, "the operands of `is`", Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        auto operand = ReadOperand((*fields)[0], value, "left");
        if (!operand) { return operand; }
        auto typeNode = Require((*fields)[1], value, "entity_type");
        if (!typeNode) { return std::unexpected{std::move(typeNode).error()}; }
        auto type = ReadEntityType(**typeNode);
        if (!type) { return std::unexpected{std::move(type).error()}; }
        const JsonNode* in = (*fields)[2];
        if (in == nullptr || in->kind == JsonKind::Null) { return Builder().Is(*operand, *type); }
        auto target = ReadExpression(*in);
        if (!target) { return target; }
        return Builder().IsIn(*operand, *type, *target);
    }

    [[nodiscard]] auto ReadIf(const JsonNode& value) -> Read<ExpressionIndex>
    {
        constexpr std::array<std::string_view, 3> Names{"if", "then", "else"};
        auto fields = ReadStruct(value, "the operands of `if-then-else`", Names, false, 0, true);
        if (!fields) { return std::unexpected{std::move(fields).error()}; }
        std::array<ExpressionIndex, 3> parts{};
        for (std::size_t index = 0; index < parts.size(); ++index)
        {
            auto part = ReadOperand((*fields)[index], value, Names[index]);
            if (!part) { return part; }
            parts[index] = *part;
        }
        return Builder().If(parts[0], parts[1], parts[2]);
    }

    [[nodiscard]] auto ReadSet(const JsonNode& value) -> Read<ExpressionIndex>
    {
        if (!value.IsArray())
        {
            return ShapeError(value, std::format("the elements of `Set` are written as an array, but got `{}`",
                                                 DescribeJson(value)));
        }
        std::vector<ExpressionIndex> elements;
        elements.reserve(value.count);
        for (const JsonNode& element : value.Elements())
        {
            auto read = ReadExpression(element);
            if (!read) { return read; }
            elements.push_back(*read);
        }
        return BuildSet(elements);
    }

    [[nodiscard]] auto ReadRecord(const JsonNode& value) -> Read<ExpressionIndex>
    {
        if (!value.IsObject())
        {
            return ShapeError(value, std::format("the fields of `Record` are written as an object, but got `{}`",
                                                 DescribeJson(value)));
        }
        std::vector<RecordLiteralField> fields;
        for (const JsonMember* member : ValueMembers(value))
        {
            auto field = ReadExpression(member->value);
            if (!field) { return field; }
            fields.push_back(RecordLiteralField{member->key, *field});
        }
        return BuildRecord(value, fields);
    }

    ExpressionArena* arena_;
    bool sawSlot_ = false;
};

// ---- Scope constraints ----

/// A scope constraint split at its `op` tag (serde's internally tagged enum): from an object, the
/// tag and the other members; from an array (serde's sequence form), the tag and the elements after it.
struct TaggedConstraint
{
    std::string_view tag;
    const JsonNode* tagNode = nullptr;
    bool fromSequence = false;
    std::vector<const JsonMember*> members;
    std::span<const JsonNode> elements;
};

[[nodiscard]] auto SplitConstraint(const JsonNode& node) -> Read<TaggedConstraint>
{
    TaggedConstraint split;
    const JsonNode* tag = nullptr;
    if (node.IsObject())
    {
        for (const JsonMember* member : ValueMembers(node))
        {
            if (member->key == "op") { tag = &member->value; }
            else { split.members.push_back(member); }
        }
    }
    else if (node.IsArray())
    {
        split.fromSequence = true;
        if (node.count > 0)
        {
            tag = &node.Elements()[0];
            split.elements = node.Elements().subspan(1);
        }
    }
    else
    {
        return ShapeError(node, std::format("expected a scope constraint as an object with an `op`, but got `{}`",
                                            DescribeJson(node)));
    }
    if (tag == nullptr) { return ShapeError(node, "missing field `op`"); }
    if (!tag->IsString())
    {
        return ShapeError(*tag, std::format("expected the scope operator `op` as a string, but got `{}`", DescribeJson(*tag)));
    }
    split.tag = tag->text;
    split.tagNode = tag;
    return split;
}

/// An entity or a slot, Rust's untagged `EqConstraint` / `PrincipalOrResourceInConstraint`:
/// `{"entity": uid}` or `{"slot": slot}`; other keys are ignored. Rust's `EntityUidJson` takes any
/// JSON value and checks it only when converting, so an `entity` key always makes the entity form.
using EntityOrSlot = std::variant<EntityUid, SlotId>;

[[nodiscard]] auto ReadEntityOrSlot(const std::vector<const JsonMember*>& members, const JsonNode& owner,
                                    const std::string_view what) -> Read<EntityOrSlot>
{
    if (const JsonNode* entity = FindMember(members, "entity"))
    {
        auto uid = ReadEntityUidJson(*entity, DuplicateKeys::LastWins);
        if (!uid) { return std::unexpected{std::move(uid).error()}; }
        return EntityOrSlot{*std::move(uid)};
    }
    if (const JsonNode* slot = FindMember(members, "slot"))
    {
        if (auto id = ReadSlotId(*slot)) { return EntityOrSlot{*id}; }
    }
    return ShapeError(owner, std::format("{} names an entity with `\"entity\": {{\"type\": ..., \"id\": ...}}` or a slot "
                                         "with `\"slot\": \"?principal\"`",
                                         what));
}

[[nodiscard]] auto ReadEntityOrSlotObject(const JsonNode& node, const std::string_view what) -> Read<EntityOrSlot>
{
    if (!node.IsObject())
    {
        return ShapeError(node, std::format("{} names an entity or a slot in an object, but got `{}`", what,
                                            DescribeJson(node)));
    }
    return ReadEntityOrSlot(ValueMembers(node), node, what);
}

[[nodiscard]] auto ToReference(const EntityOrSlot& target, const SlotId position, const JsonNode& owner)
    -> Read<EntityReference>
{
    if (const auto* uid = std::get_if<EntityUid>(&target)) { return EntityReference{*uid}; }
    if (std::get<SlotId>(target) != position)
    {
        return ErrorAt(owner, DiagnosticCode::JsonInvalidSlotName, "invalid slot name or slot used in the wrong position",
                       "principal slots are named `?principal` and resource slots `?resource`");
    }
    return EntityReference{SlotReference{}};
}

[[nodiscard]] auto ReadPrincipalOrResource(const JsonNode& node, const SlotId position)
    -> Read<PrincipalOrResourceConstraint>
{
    auto split = SplitConstraint(node);
    if (!split) { return std::unexpected{std::move(split).error()}; }
    const std::string_view tag = split->tag;
    if (tag == "All" || tag == "all")
    {
        if (split->fromSequence && !split->elements.empty())
        {
            return ShapeError(node, "a scope constraint `All` written as an array has no elements after the tag");
        }
        return PrincipalOrResourceConstraint{ScopeAny{}};
    }
    if (tag == "==" || tag == "in")
    {
        if (split->fromSequence)
        {
            return ShapeError(node, std::format("a scope constraint `{}` is written as an object", tag));
        }
        auto target = ReadEntityOrSlot(split->members, node, std::format("the scope constraint `{}`", tag));
        if (!target) { return std::unexpected{std::move(target).error()}; }
        auto reference = ToReference(*target, position, node);
        if (!reference) { return std::unexpected{std::move(reference).error()}; }
        if (tag == "==") { return PrincipalOrResourceConstraint{ScopeEqual{*std::move(reference)}}; }
        return PrincipalOrResourceConstraint{ScopeIn{*std::move(reference)}};
    }
    if (tag == "is")
    {
        // Rust's `PrincipalOrResourceIsConstraint`: `entity_type` and an optional `in`; as an array
        // both elements are needed (null for no `in`).
        const JsonNode* typeNode = nullptr;
        const JsonNode* inNode = nullptr;
        if (split->fromSequence)
        {
            if (split->elements.size() != 2)
            {
                return ShapeError(node, std::format("invalid length {}, expected a scope constraint `is` written as "
                                                    "[\"is\", entity_type, in]",
                                                    split->elements.size()));
            }
            typeNode = &split->elements[0];
            inNode = &split->elements[1];
        }
        else
        {
            for (const JsonMember* member : split->members)
            {
                if (member->key == "entity_type") { typeNode = &member->value; }
                else if (member->key == "in") { inNode = &member->value; }
                else
                {
                    return ErrorAt(member->value, DiagnosticCode::JsonUnexpectedShape,
                                   std::format("unknown field `{}` in the scope constraint `is`", member->key));
                }
            }
        }
        if (typeNode == nullptr) { return ShapeError(node, "missing field `entity_type`"); }
        auto type = ReadEntityType(*typeNode);
        if (!type) { return std::unexpected{std::move(type).error()}; }
        if (inNode == nullptr || inNode->kind == JsonKind::Null)
        {
            return PrincipalOrResourceConstraint{ScopeIs{*std::move(type)}};
        }
        auto target = ReadEntityOrSlotObject(*inNode, "the `in` of a scope constraint `is`");
        if (!target) { return std::unexpected{std::move(target).error()}; }
        auto reference = ToReference(*target, position, node);
        if (!reference) { return std::unexpected{std::move(reference).error()}; }
        return PrincipalOrResourceConstraint{ScopeIsIn{*std::move(type), *std::move(reference)}};
    }
    return ShapeError(*split->tagNode, std::format("unknown scope operator `{}`", tag));
}

[[nodiscard]] auto CheckActions(const std::vector<EntityUid>& actions, const JsonNode& node) -> Read<void>
{
    for (const EntityUid& uid : actions)
    {
        if (!uid.IsAction())
        {
            return ErrorAt(node, DiagnosticCode::InvalidActionType,
                           std::format("expected an entity uid with type `Action` but got `{}`", uid.ToString()),
                           "action entities have type `Action`, optionally in a namespace");
        }
    }
    return {};
}

[[nodiscard]] auto ReadAction(const JsonNode& node) -> Read<ActionConstraint>
{
    auto split = SplitConstraint(node);
    if (!split) { return std::unexpected{std::move(split).error()}; }
    const std::string_view tag = split->tag;
    if (tag == "All" || tag == "all")
    {
        if (split->fromSequence && !split->elements.empty())
        {
            return ShapeError(node, "a scope constraint `All` written as an array has no elements after the tag");
        }
        return ActionConstraint{ActionAny{}};
    }
    if (tag != "==" && tag != "in") { return ShapeError(*split->tagNode, std::format("unknown action scope operator `{}`", tag)); }
    if (split->fromSequence) { return ShapeError(node, std::format("a scope constraint `{}` is written as an object", tag)); }
    if (tag == "==")
    {
        auto target = ReadEntityOrSlot(split->members, node, "the action scope constraint `==`");
        if (!target) { return std::unexpected{std::move(target).error()}; }
        if (std::holds_alternative<SlotId>(*target))
        {
            return ErrorAt(node, DiagnosticCode::JsonActionSlot, "slots are not allowed for actions");
        }
        std::vector<EntityUid> actions{std::get<EntityUid>(*std::move(target))};
        if (auto checked = CheckActions(actions, node); !checked) { return std::unexpected{std::move(checked).error()}; }
        return ActionConstraint{ActionEqual{std::move(actions.front())}};
    }
    // Rust's untagged `ActionInConstraint`: `{"entity": uid}`, else `{"entities": [uid, ...]}`;
    // as above, an `entity` key or an `entities` array decides the form before any uid is read.
    if (const JsonNode* entity = FindMember(split->members, "entity"))
    {
        auto uid = ReadEntityUidJson(*entity, DuplicateKeys::LastWins);
        if (!uid) { return std::unexpected{std::move(uid).error()}; }
        std::vector<EntityUid> actions{*std::move(uid)};
        if (auto checked = CheckActions(actions, node); !checked) { return std::unexpected{std::move(checked).error()}; }
        return ActionConstraint{ActionIn{std::move(actions), false}};
    }
    if (const JsonNode* entities = FindMember(split->members, "entities"); entities != nullptr && entities->IsArray())
    {
        std::vector<EntityUid> actions;
        for (const JsonNode& element : entities->Elements())
        {
            auto uid = ReadEntityUidJson(element, DuplicateKeys::LastWins);
            if (!uid) { return std::unexpected{std::move(uid).error()}; }
            actions.push_back(*std::move(uid));
        }
        if (auto checked = CheckActions(actions, node); !checked) { return std::unexpected{std::move(checked).error()}; }
        return ActionConstraint{ActionIn{std::move(actions), true}};
    }
    return ShapeError(node, "the action scope constraint `in` names an entity with `\"entity\"` or a list with "
                            "`\"entities\"`");
}

// ---- Policies ----

[[nodiscard]] auto ReadAnnotations(const JsonNode& node) -> Read<Annotations>
{
    if (!node.IsObject())
    {
        return ShapeError(node, std::format("expected the annotations as an object, but got `{}`", DescribeJson(node)));
    }
    Annotations annotations;
    for (const JsonMember* member : ValueMembers(node))
    {
        if (!IsIdentifierSyntax(member->key))
        {
            return ErrorAt(member->value, DiagnosticCode::InvalidIdentifier,
                           std::format("`{}` is not a valid annotation key", member->key));
        }
        std::optional<std::string> value;
        if (member->value.kind != JsonKind::Null)
        {
            auto text = ReadString(member->value, "an annotation value");
            if (!text) { return std::unexpected{std::move(text).error()}; }
            value = std::string{*text};
        }
        if (!annotations.Insert(Symbol::Intern(member->key), std::move(value)))
        {
            return ErrorAt(member->value, DiagnosticCode::DuplicateAnnotation,
                           std::format("the annotation `{}` appears more than once", member->key));
        }
    }
    return annotations;
}

/// Rust's `Clause`, adjacently tagged by `kind` with the expression in `body`, or the sequence
/// `[kind, body]`. Returns the condition it contributes: the body, negated for `unless`.
[[nodiscard]] auto ReadClause(const JsonNode& node, ExpressionReader& reader) -> Read<ExpressionIndex>
{
    constexpr std::array<std::string_view, 2> Names{"kind", "body"};
    constexpr std::array<std::string_view, 2> Kinds{"when", "unless"};
    auto fields = ReadStruct(node, "a condition", Names, true, 2, true);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    auto kindNode = Require((*fields)[0], node, "kind");
    if (!kindNode) { return std::unexpected{std::move(kindNode).error()}; }
    auto bodyNode = Require((*fields)[1], node, "body");
    if (!bodyNode) { return std::unexpected{std::move(bodyNode).error()}; }
    auto kind = ReadUnitVariant(**kindNode, "condition kind", Kinds, !node.IsArray());
    if (!kind) { return std::unexpected{std::move(kind).error()}; }
    reader.ClearSlots();
    auto body = reader.ReadExpression(**bodyNode);
    if (!body) { return body; }
    if (reader.SawSlot())
    {
        return ErrorAt(**bodyNode, DiagnosticCode::SlotsInConditionClause,
                       std::format("found a template slot in a `{}` clause", Kinds[*kind]),
                       "slots are only allowed in the scope of a template");
    }
    if (*kind == 1) { return ExpressionBuilder{reader.Arena()}.Not(*body); }
    return *body;
}

/// Rust's `est::Policy` (struct or sequence `[effect, principal, action, resource, conditions,
/// annotations?]`) converted by `try_into_ast_policy_or_template`: the conditions joined right to
/// left, `c1 && (c2 && c3)`, as Rust folds them.
[[nodiscard]] auto ReadPolicyBody(const JsonNode& node, PolicyId id) -> Read<std::shared_ptr<const Template>>
{
    constexpr std::array<std::string_view, 6> Names{"effect", "principal", "action", "resource", "conditions",
                                                    "annotations"};
    auto fields = ReadStruct(node, "a policy", Names, true, 5, true);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    for (std::size_t index = 0; index < 5; ++index)
    {
        if (auto required = Require((*fields)[index], node, Names[index]); !required)
        {
            return std::unexpected{std::move(required).error()};
        }
    }
    TemplateDefinition definition{.id = std::move(id)};
    constexpr std::array<std::string_view, 2> Effects{"permit", "forbid"};
    auto effect = ReadUnitVariant(*(*fields)[0], "effect", Effects, true);
    if (!effect) { return std::unexpected{std::move(effect).error()}; }
    definition.effect = *effect == 0 ? Effect::Permit : Effect::Forbid;

    auto principal = ReadPrincipalOrResource(*(*fields)[1], SlotId::Principal);
    if (!principal) { return std::unexpected{std::move(principal).error()}; }
    definition.principal.constraint = *std::move(principal);
    auto action = ReadAction(*(*fields)[2]);
    if (!action) { return std::unexpected{std::move(action).error()}; }
    definition.action = *std::move(action);
    auto resource = ReadPrincipalOrResource(*(*fields)[3], SlotId::Resource);
    if (!resource) { return std::unexpected{std::move(resource).error()}; }
    definition.resource.constraint = *std::move(resource);

    const JsonNode& conditions = *(*fields)[4];
    if (!conditions.IsArray())
    {
        return ShapeError(conditions, std::format("expected the conditions as an array, but got `{}`", DescribeJson(conditions)));
    }
    ExpressionReader reader{definition.arena};
    std::vector<ExpressionIndex> clauses;
    clauses.reserve(conditions.count);
    for (const JsonNode& clause : conditions.Elements())
    {
        auto condition = ReadClause(clause, reader);
        if (!condition) { return std::unexpected{std::move(condition).error()}; }
        clauses.push_back(*condition);
    }
    if (!clauses.empty())
    {
        ExpressionBuilder builder{definition.arena};
        ExpressionIndex combined = clauses.back();
        for (std::size_t index = clauses.size() - 1; index-- > 0;) { combined = builder.And(clauses[index], combined); }
        definition.nonScopeCondition = combined;
    }

    if (const JsonNode* annotations = (*fields)[5])
    {
        auto read = ReadAnnotations(*annotations);
        if (!read) { return std::unexpected{std::move(read).error()}; }
        definition.annotations = *std::move(read);
    }
    return Template::Create(std::move(definition));
}

/// The parsed, strictly checked document, or the error as a diagnostic about `source`.
template <typename Error>
[[nodiscard]] auto ReadDocument(const std::string_view json, const DuplicateKeys duplicates,
                                std::pmr::memory_resource& resource, const SharedSourceText& source)
    -> std::expected<JsonDocument, Error>
{
    auto document = ReadJson(json, resource);
    if (!document) { return std::unexpected{Error{ToDiagnostic(FromSyntaxError(std::move(document).error()), source)}}; }
    if (auto checked = CheckStrictJson(document->Root(), duplicates); !checked)
    {
        return std::unexpected{Error{ToDiagnostic(std::move(checked).error(), source)}};
    }
    return std::move(*document);
}

/// Rust's `TemplateLink` (struct or sequence `[templateId, newId, values]`): the link's ids and
/// the uid bound to each slot named in `values`.
struct LinkFields
{
    std::string_view templateId;
    std::string_view newId;
    SlotBindings bindings;
};

[[nodiscard]] auto ReadTemplateLink(const JsonNode& node) -> Read<LinkFields>
{
    constexpr std::array<std::string_view, 3> Names{"templateId", "newId", "values"};
    auto fields = ReadStruct(node, "a template link", Names, true, 3, true);
    if (!fields) { return std::unexpected{std::move(fields).error()}; }
    for (std::size_t index = 0; index < Names.size(); ++index)
    {
        if (auto required = Require((*fields)[index], node, Names[index]); !required)
        {
            return std::unexpected{std::move(required).error()};
        }
    }
    auto templateId = ReadString(*(*fields)[0], "`templateId`");
    if (!templateId) { return std::unexpected{std::move(templateId).error()}; }
    auto newId = ReadString(*(*fields)[1], "`newId`");
    if (!newId) { return std::unexpected{std::move(newId).error()}; }
    const JsonNode& values = *(*fields)[2];
    if (!values.IsObject())
    {
        return ShapeError(values, std::format("expected the link's `values` as an object from slots to entity uids, but "
                                              "got `{}`",
                                              DescribeJson(values)));
    }
    LinkFields link{.templateId = *templateId, .newId = *newId, .bindings = {}};
    for (const JsonMember* member : ValueMembers(values))
    {
        SlotId slot{};
        if (member->key == "?principal") { slot = SlotId::Principal; }
        else if (member->key == "?resource") { slot = SlotId::Resource; }
        else
        {
            return ErrorAt(member->value, DiagnosticCode::JsonUnexpectedShape,
                           std::format("unknown slot `{}`; expected `?principal` or `?resource`", member->key));
        }
        auto uid = ReadEntityUidJson(member->value, DuplicateKeys::LastWins);
        if (!uid) { return std::unexpected{std::move(uid).error()}; }
        link.bindings.Bind(slot, *std::move(uid));
    }
    return link;
}

[[nodiscard]] auto ReadPolicyMap(const JsonNode& node, const std::string_view what) -> Read<std::vector<const JsonMember*>>
{
    if (!node.IsObject())
    {
        return ShapeError(node, std::format("expected {} as an object from ids to policies, but got `{}`", what,
                                            DescribeJson(node)));
    }
    return ValueMembers(node);
}

} // namespace

auto ReadPolicyOrTemplateJson(const std::string_view json, const DuplicateKeys duplicates, PolicyId id,
                              const std::string_view sourceName) -> std::expected<std::shared_ptr<const Template>, ParseError>
{
    const SharedSourceText source = MakeSourceText(std::string{sourceName}, std::string{json});
    std::pmr::monotonic_buffer_resource scratch;
    auto document = ReadDocument<ParseError>(json, duplicates, scratch, source);
    if (!document) { return std::unexpected{std::move(document).error()}; }
    auto body = ReadPolicyBody(document->Root(), std::move(id));
    if (!body) { return std::unexpected{ParseError{ToDiagnostic(std::move(body).error(), source)}}; }
    return *std::move(body);
}

auto ReadPolicySetJson(const std::string_view json, const DuplicateKeys duplicates, const std::string_view sourceName)
    -> std::expected<PolicySet, PolicySetError>
{
    const SharedSourceText source = MakeSourceText(std::string{sourceName}, std::string{json});
    const auto fail = [&](JsonReadError error) {
        return std::unexpected{PolicySetError{ToDiagnostic(std::move(error), source)}};
    };
    std::pmr::monotonic_buffer_resource scratch;
    auto document = ReadDocument<PolicySetError>(json, duplicates, scratch, source);
    if (!document) { return std::unexpected{std::move(document).error()}; }
    const JsonNode& root = document->Root();

    constexpr std::array<std::string_view, 3> Names{"templates", "staticPolicies", "templateLinks"};
    auto fields = ReadStruct(root, "a policy set", Names, true, 3, true);
    if (!fields) { return fail(std::move(fields).error()); }
    for (std::size_t index = 0; index < Names.size(); ++index)
    {
        if (auto required = Require((*fields)[index], root, Names[index]); !required) { return fail(std::move(required).error()); }
    }
    auto templates = ReadPolicyMap(*(*fields)[0], "`templates`");
    if (!templates) { return fail(std::move(templates).error()); }
    auto statics = ReadPolicyMap(*(*fields)[1], "`staticPolicies`");
    if (!statics) { return fail(std::move(statics).error()); }
    const JsonNode& linksNode = *(*fields)[2];
    if (!linksNode.IsArray())
    {
        return fail(ShapeError(linksNode, std::format("expected `templateLinks` as an array, but got `{}`",
                                                      DescribeJson(linksNode)))
                        .error());
    }
    std::vector<LinkFields> links;
    links.reserve(linksNode.count);
    for (const JsonNode& element : linksNode.Elements())
    {
        auto link = ReadTemplateLink(element);
        if (!link) { return fail(std::move(link).error()); }
        links.push_back(*std::move(link));
    }

    // `TryFrom<est::PolicySet> for ast::PolicySet`: static policies, then templates, then links.
    PolicySet set;
    for (const JsonMember* member : *statics)
    {
        auto body = ReadPolicyBody(member->value, PolicyId{member->key});
        if (!body) { return fail(std::move(body).error()); }
        if (!(*body)->IsStatic())
        {
            return fail(ErrorAt(member->value, DiagnosticCode::ExpectedStaticPolicy,
                                std::format("`{}` is in `staticPolicies` but has slots", member->key),
                                "a policy with slots is a template; put it in `templates`")
                            .error());
        }
        auto policy = Policy::Static(*std::move(body));
        if (!policy) { return std::unexpected{std::move(policy).error()}; }
        if (auto added = set.Add(*std::move(policy)); !added) { return std::unexpected{std::move(added).error()}; }
    }
    for (const JsonMember* member : *templates)
    {
        auto body = ReadPolicyBody(member->value, PolicyId{member->key});
        if (!body) { return fail(std::move(body).error()); }
        if (auto added = set.AddTemplate(*std::move(body)); !added) { return std::unexpected{std::move(added).error()}; }
    }
    for (LinkFields& link : links)
    {
        if (auto linked = set.Link(link.templateId, PolicyId{link.newId}, std::move(link.bindings)); !linked)
        {
            return std::unexpected{std::move(linked).error()};
        }
    }
    PolicySetAccess::HideSlotFreeTemplates(set);
    return set;
}

} // namespace cedar::detail

// ---- Writer ----

namespace cedar
{

namespace
{

void AppendUid(std::string& output, const EntityUid& uid)
{
    output += "{\"type\":";
    detail::AppendJsonString(output, uid.Type().GetName().QualifiedText());
    output += ",\"id\":";
    detail::AppendJsonString(output, uid.Id().Text());
    output.push_back('}');
}

void AppendEscapedUid(std::string& output, const EntityUid& uid)
{
    output += "{\"__entity\":";
    AppendUid(output, uid);
    output.push_back('}');
}

[[nodiscard]] constexpr auto SlotText(const SlotId slot) noexcept -> std::string_view
{
    return slot == SlotId::Principal ? "?principal" : "?resource";
}

[[nodiscard]] constexpr auto OperatorKey(const BinaryOperator operation) noexcept -> std::string_view
{
    switch (operation)
    {
    case BinaryOperator::Equal: return "==";
    case BinaryOperator::Less: return "<";
    case BinaryOperator::LessEqual: return "<=";
    case BinaryOperator::Add: return "+";
    case BinaryOperator::Subtract: return "-";
    case BinaryOperator::Multiply: return "*";
    case BinaryOperator::In: return "in";
    case BinaryOperator::Contains: return "contains";
    case BinaryOperator::ContainsAll: return "containsAll";
    case BinaryOperator::ContainsAny: return "containsAny";
    case BinaryOperator::GetTag: return "getTag";
    case BinaryOperator::HasTag: return "hasTag";
    }
    return {};
}

/// Writes the expressions of one template as Rust converts an AST to the EST
/// (`From<ast::Expr> for est::Expr`): a literal is `{"Value": ...}`, a set or record literal
/// `{"Set": ...}`/`{"Record": ...}`, an extension call `{"name": [arguments]}`. A folded node is
/// written as written. With bindings, a bound slot is written as the entity bound to it.
class ExpressionWriter final
{
public:
    ExpressionWriter(std::string& output, const ExpressionArena& arena, const SlotBindings* bindings) noexcept
        : output_{&output}, arena_{&arena}, bindings_{bindings}
    {
    }

    void Write(const ExpressionIndex root)
    {
        arena_->Visit(root, Overloaded{
                                 [&](const node::Literal& literal) { WriteValue(literal.value); },
                                 [&](const node::Folded& folded) { Write(folded.written); },
                                 [&](const node::Variable& variable) {
                                     Out() += "{\"Var\":\"";
                                     Out() += ToString(variable.variable);
                                     Out() += "\"}";
                                 },
                                 [&](const node::Slot& slot) {
                                     const EntityUid* bound = bindings_ != nullptr ? bindings_->Get(slot.slot) : nullptr;
                                     if (bound != nullptr)
                                     {
                                         Out() += "{\"Value\":";
                                         AppendEscapedUid(Out(), *bound);
                                         Out().push_back('}');
                                         return;
                                     }
                                     Out() += "{\"Slot\":\"";
                                     Out() += SlotText(slot.slot);
                                     Out() += "\"}";
                                 },
                                 [&](const node::If& branch) {
                                     Out() += "{\"if-then-else\":{\"if\":";
                                     Write(branch.condition);
                                     Out() += ",\"then\":";
                                     Write(branch.thenBranch);
                                     Out() += ",\"else\":";
                                     Write(branch.elseBranch);
                                     Out() += "}}";
                                 },
                                 [&](const node::And& both) { WriteBinary("&&", both.left, both.right); },
                                 [&](const node::Or& either) { WriteBinary("||", either.left, either.right); },
                                 [&](const node::Unary& unary) {
                                     Out() += unary.operation == UnaryOperator::Not      ? "{\"!\":{\"arg\":"
                                              : unary.operation == UnaryOperator::Negate ? "{\"neg\":{\"arg\":"
                                                                                          : "{\"isEmpty\":{\"arg\":";
                                     Write(unary.operand);
                                     Out() += "}}";
                                 },
                                 [&](const node::Binary& binary) {
                                     WriteBinary(OperatorKey(binary.operation), binary.left, binary.right);
                                 },
                                 [&](const node::ExtensionCall& call) {
                                     // A function outside the extension table comes only from an
                                     // `__extn` escape, and only that form reads back as a call to it.
                                     if (!call.function.IsUnqualified() ||
                                         detail::FindExtensionCall(call.function.QualifiedText()) == nullptr)
                                     {
                                         std::string escape = "{\"Value\":";
                                         if (AppendValueJson(escape, root))
                                         {
                                             Out() += escape;
                                             Out().push_back('}');
                                             return;
                                         }
                                     }
                                     Out().push_back('{');
                                     detail::AppendJsonString(Out(), call.function.QualifiedText());
                                     Out() += ":[";
                                     bool first = true;
                                     for (const ExpressionIndex argument : arena_->Elements(call.arguments))
                                     {
                                         if (!first) { Out().push_back(','); }
                                         first = false;
                                         Write(argument);
                                     }
                                     Out() += "]}";
                                 },
                                 [&](const node::GetAttribute& access) {
                                     Out() += "{\".\":{\"left\":";
                                     Write(access.object);
                                     Out() += ",\"attr\":";
                                     detail::AppendJsonString(Out(), access.attribute);
                                     Out() += "}}";
                                 },
                                 [&](const node::HasAttribute& test) {
                                     Out() += "{\"has\":{\"left\":";
                                     Write(test.object);
                                     Out() += ",\"attr\":";
                                     const auto path = arena_->Elements(test.path);
                                     if (path.size() == 1) { detail::AppendJsonString(Out(), path.front()); }
                                     else
                                     {
                                         Out().push_back('[');
                                         for (std::size_t index = 0; index < path.size(); ++index)
                                         {
                                             if (index > 0) { Out().push_back(','); }
                                             detail::AppendJsonString(Out(), path[index]);
                                         }
                                         Out().push_back(']');
                                     }
                                     Out() += "}}";
                                 },
                                 [&](const node::Like& like) {
                                     Out() += "{\"like\":{\"left\":";
                                     Write(like.operand);
                                     Out() += ",\"pattern\":[";
                                     const auto literals = arena_->PatternOf(like).Literals();
                                     bool first = true;
                                     for (std::size_t index = 0; index < literals.size(); ++index)
                                     {
                                         if (index > 0)
                                         {
                                             if (!first) { Out().push_back(','); }
                                             first = false;
                                             Out() += "\"Wildcard\"";
                                         }
                                         if (literals[index].empty()) { continue; }
                                         if (!first) { Out().push_back(','); }
                                         first = false;
                                         Out() += "{\"Literal\":";
                                         detail::AppendJsonString(Out(), literals[index]);
                                         Out().push_back('}');
                                     }
                                     Out() += "]}}";
                                 },
                                 [&](const node::Is& test) {
                                     Out() += "{\"is\":{\"left\":";
                                     Write(test.operand);
                                     Out() += ",\"entity_type\":";
                                     detail::AppendJsonString(Out(), test.type.GetName().QualifiedText());
                                     Out() += "}}";
                                 },
                                 [&](const node::SetLiteral& set) {
                                     Out() += "{\"Set\":[";
                                     bool first = true;
                                     for (const ExpressionIndex element : arena_->Elements(set.elements))
                                     {
                                         if (!first) { Out().push_back(','); }
                                         first = false;
                                         Write(element);
                                     }
                                     Out() += "]}";
                                 },
                                 [&](const node::RecordLiteral& record) {
                                     Out() += "{\"Record\":{";
                                     bool first = true;
                                     for (const RecordLiteralField& field : arena_->Elements(record.fields))
                                     {
                                         if (!first) { Out().push_back(','); }
                                         first = false;
                                         detail::AppendJsonString(Out(), field.key);
                                         Out().push_back(':');
                                         Write(field.value);
                                     }
                                     Out() += "}}";
                                 },
                             });
    }

private:
    [[nodiscard]] auto Out() const noexcept -> std::string& { return *output_; }

    /// Appends the subtree `index` in the entity JSON value format (`CedarValueJson`), with
    /// extension calls as `__extn` escapes. False, leaving `output` unspecified, when the subtree is
    /// not a constant the format can hold (a variable, an operator, a record whose one key would
    /// read as an escape).
    [[nodiscard]] auto AppendValueJson(std::string& output, const ExpressionIndex index) const -> bool
    {
        const ExpressionNode& node = arena_->Node(index);
        if (const auto* folded = std::get_if<node::Folded>(&node)) { return AppendValueJson(output, folded->written); }
        if (const auto* literal = std::get_if<node::Literal>(&node))
        {
            const Value& value = literal->value;
            switch (value.Kind())
            {
            case ValueKind::Bool: output += value.AsBool() ? "true" : "false"; return true;
            case ValueKind::Long: output += std::format("{}", value.AsLong()); return true;
            case ValueKind::String: detail::AppendJsonString(output, value.AsString()); return true;
            case ValueKind::Entity: AppendEscapedUid(output, value.AsEntity()); return true;
            default: return false;
            }
        }
        if (const auto* set = std::get_if<node::SetLiteral>(&node))
        {
            output.push_back('[');
            bool first = true;
            for (const ExpressionIndex element : arena_->Elements(set->elements))
            {
                if (!first) { output.push_back(','); }
                first = false;
                if (!AppendValueJson(output, element)) { return false; }
            }
            output.push_back(']');
            return true;
        }
        if (const auto* record = std::get_if<node::RecordLiteral>(&node))
        {
            const auto fields = arena_->Elements(record->fields);
            if (fields.size() == 1 &&
                (fields.front().key == "__extn" || fields.front().key == "__entity" || fields.front().key == "__expr"))
            {
                return false;
            }
            output.push_back('{');
            bool first = true;
            for (const RecordLiteralField& field : fields)
            {
                if (!first) { output.push_back(','); }
                first = false;
                detail::AppendJsonString(output, field.key);
                output.push_back(':');
                if (!AppendValueJson(output, field.value)) { return false; }
            }
            output.push_back('}');
            return true;
        }
        if (const auto* call = std::get_if<node::ExtensionCall>(&node))
        {
            output += "{\"__extn\":{\"fn\":";
            detail::AppendJsonString(output, call->function.QualifiedText());
            output += ",\"args\":[";
            bool first = true;
            for (const ExpressionIndex argument : arena_->Elements(call->arguments))
            {
                if (!first) { output.push_back(','); }
                first = false;
                if (!AppendValueJson(output, argument)) { return false; }
            }
            output += "]}}";
            return true;
        }
        return false;
    }

    void WriteBinary(const std::string_view key, const ExpressionIndex left, const ExpressionIndex right)
    {
        Out().push_back('{');
        detail::AppendJsonString(Out(), key);
        Out() += ":{\"left\":";
        Write(left);
        Out() += ",\"right\":";
        Write(right);
        Out() += "}}";
    }

    /// A value held by a literal node, written as the expression that builds it.
    void WriteValue(const Value& value)
    {
        const auto call = [&](const std::string_view function, const std::string_view argument) {
            Out() += "{\"";
            Out() += function;
            Out() += "\":[{\"Value\":";
            detail::AppendJsonString(Out(), argument);
            Out() += "}]}";
        };
        std::string text;
        switch (value.Kind())
        {
        case ValueKind::Bool: Out() += value.AsBool() ? "{\"Value\":true}" : "{\"Value\":false}"; return;
        case ValueKind::Long: Out() += std::format("{{\"Value\":{}}}", value.AsLong()); return;
        case ValueKind::String:
            Out() += "{\"Value\":";
            detail::AppendJsonString(Out(), value.AsString());
            Out().push_back('}');
            return;
        case ValueKind::Entity:
            Out() += "{\"Value\":";
            AppendEscapedUid(Out(), value.AsEntity());
            Out().push_back('}');
            return;
        case ValueKind::Set:
        {
            Out() += "{\"Set\":[";
            bool first = true;
            for (const Value& element : value.AsSet())
            {
                if (!first) { Out().push_back(','); }
                first = false;
                WriteValue(element);
            }
            Out() += "]}";
            return;
        }
        case ValueKind::Record:
        {
            Out() += "{\"Record\":{";
            bool first = true;
            for (const RecordEntry& entry : value.AsRecord())
            {
                if (!first) { Out().push_back(','); }
                first = false;
                detail::AppendJsonString(Out(), entry.key);
                Out().push_back(':');
                WriteValue(entry.value);
            }
            Out() += "}}";
            return;
        }
        case ValueKind::Decimal:
            AppendCanonicalString(text, value.AsDecimal());
            call("decimal", text);
            return;
        case ValueKind::Duration:
            AppendCanonicalString(text, value.AsDuration());
            call("duration", text);
            return;
        case ValueKind::IpAddress:
            AppendCanonicalString(text, value.AsIpAddress());
            call("ip", text);
            return;
        case ValueKind::Datetime:
            // `datetime("1970-01-01").offset(duration("<n>ms"))`, the canonical form.
            AppendCanonicalString(text, Duration{value.AsDatetime().millisecondsSinceEpoch});
            Out() += "{\"offset\":[{\"datetime\":[{\"Value\":\"1970-01-01\"}]},";
            call("duration", text);
            Out() += "]}";
            return;
        }
    }

    std::string* output_;
    const ExpressionArena* arena_;
    const SlotBindings* bindings_;
};

/// The entity or slot of a principal or resource constraint: `"entity": uid` or `"slot": name`,
/// or the bound entity when `bindings` binds the slot.
void AppendReference(std::string& output, const EntityReference& reference, const SlotId slot,
                     const SlotBindings* bindings)
{
    const EntityUid* uid = std::get_if<EntityUid>(&reference);
    if (uid == nullptr && bindings != nullptr) { uid = bindings->Get(slot); }
    if (uid != nullptr)
    {
        output += "\"entity\":";
        AppendUid(output, *uid);
        return;
    }
    output += "\"slot\":\"";
    output += SlotText(slot);
    output.push_back('"');
}

void AppendScope(std::string& output, const PrincipalOrResourceConstraint& constraint, const SlotId slot,
                 const SlotBindings* bindings)
{
    std::visit(Overloaded{
                   [&](const ScopeAny&) { output += "{\"op\":\"All\"}"; },
                   [&](const ScopeEqual& equal) {
                       output += "{\"op\":\"==\",";
                       AppendReference(output, equal.target, slot, bindings);
                       output.push_back('}');
                   },
                   [&](const ScopeIn& in) {
                       output += "{\"op\":\"in\",";
                       AppendReference(output, in.target, slot, bindings);
                       output.push_back('}');
                   },
                   [&](const ScopeIs& is) {
                       output += "{\"op\":\"is\",\"entity_type\":";
                       detail::AppendJsonString(output, is.type.GetName().QualifiedText());
                       output.push_back('}');
                   },
                   [&](const ScopeIsIn& isIn) {
                       output += "{\"op\":\"is\",\"entity_type\":";
                       detail::AppendJsonString(output, isIn.type.GetName().QualifiedText());
                       output += ",\"in\":{";
                       AppendReference(output, isIn.target, slot, bindings);
                       output += "}}";
                   },
               },
               constraint);
}

void AppendAction(std::string& output, const ActionConstraint& constraint)
{
    std::visit(Overloaded{
                   [&](const ActionAny&) { output += "{\"op\":\"All\"}"; },
                   [&](const ActionEqual& equal) {
                       output += "{\"op\":\"==\",\"entity\":";
                       AppendUid(output, equal.action);
                       output.push_back('}');
                   },
                   [&](const ActionIn& in) {
                       if (!in.writtenAsList && in.actions.size() == 1)
                       {
                           output += "{\"op\":\"in\",\"entity\":";
                           AppendUid(output, in.actions.front());
                           output.push_back('}');
                           return;
                       }
                       output += "{\"op\":\"in\",\"entities\":[";
                       for (std::size_t index = 0; index < in.actions.size(); ++index)
                       {
                           if (index > 0) { output.push_back(','); }
                           AppendUid(output, in.actions[index]);
                       }
                       output += "]}";
                   },
               },
               constraint);
}

/// `est::Policy` as Rust serializes it: effect, scope, the conditions (the non-scope condition as
/// one `when` clause, or none) and the annotations when there are any.
void AppendTemplate(std::string& output, const Template& body, const SlotBindings* bindings)
{
    output += "{\"effect\":\"";
    output += ToString(body.GetEffect());
    output += "\",\"principal\":";
    AppendScope(output, body.Principal().constraint, SlotId::Principal, bindings);
    output += ",\"action\":";
    AppendAction(output, body.Action());
    output += ",\"resource\":";
    AppendScope(output, body.Resource().constraint, SlotId::Resource, bindings);
    output += ",\"conditions\":[";
    if (const auto condition = body.NonScopeCondition())
    {
        output += "{\"kind\":\"when\",\"body\":";
        ExpressionWriter{output, body.Arena(), bindings}.Write(*condition);
        output.push_back('}');
    }
    output.push_back(']');
    if (!body.GetAnnotations().IsEmpty())
    {
        output += ",\"annotations\":{";
        bool first = true;
        for (const Annotation& annotation : body.GetAnnotations())
        {
            if (!first) { output.push_back(','); }
            first = false;
            detail::AppendJsonString(output, annotation.key.Text());
            output.push_back(':');
            if (annotation.value) { detail::AppendJsonString(output, *annotation.value); }
            else { output += "null"; }
        }
        output.push_back('}');
    }
    output.push_back('}');
}

} // namespace

auto PolicyFromJson(const std::string_view json, std::optional<PolicyId> id, const std::string_view sourceName)
    -> std::expected<Policy, ParseError>
{
    // Rust's `Policy::from_json` takes a `serde_json::Value`, in which a repeated key has its last value.
    auto body = detail::ReadPolicyOrTemplateJson(json, detail::DuplicateKeys::LastWins,
                                                 id ? *std::move(id) : PolicyId{"JSON policy"}, sourceName);
    if (!body) { return std::unexpected{std::move(body).error()}; }
    if (!(*body)->IsStatic())
    {
        return std::unexpected{ParseError{Diagnostic{
            .code = DiagnosticCode::ExpectedStaticPolicy,
            .message = std::format("expected a static policy, but `{}` is a template (it has slots)", (*body)->Id().Text()),
            .source = (*body)->Source(),
        }}};
    }
    auto policy = Policy::Static(*std::move(body));
    if (!policy) { return std::unexpected{ParseError{std::move(policy).error()}}; }
    return *std::move(policy);
}

auto TemplateFromJson(const std::string_view json, std::optional<PolicyId> id, const std::string_view sourceName)
    -> std::expected<std::shared_ptr<const Template>, ParseError>
{
    auto body = detail::ReadPolicyOrTemplateJson(json, detail::DuplicateKeys::LastWins,
                                                 id ? *std::move(id) : PolicyId{"JSON policy"}, sourceName);
    if (!body) { return std::unexpected{std::move(body).error()}; }
    if ((*body)->IsStatic())
    {
        return std::unexpected{ParseError{Diagnostic{
            .code = DiagnosticCode::ExpectedTemplate,
            .message = std::format("expected a template, but `{}` has no slots", (*body)->Id().Text()),
        }}};
    }
    return *std::move(body);
}

auto PolicySetFromJson(const std::string_view json, const std::string_view sourceName)
    -> std::expected<PolicySet, PolicySetError>
{
    // Rust's `PolicySet::from_json_str` streams the text, refusing a repeated policy id, field or
    // record key.
    return detail::ReadPolicySetJson(json, detail::DuplicateKeys::Reject, sourceName);
}

auto ToJson(const Template& body) -> std::string
{
    std::string output;
    AppendTemplate(output, body, nullptr);
    return output;
}

auto ToJson(const Policy& policy) -> std::string
{
    std::string output;
    AppendTemplate(output, policy.GetTemplate(), policy.IsStatic() ? nullptr : &policy.Bindings());
    return output;
}

auto ToJson(const PolicySet& policies) -> std::string
{
    std::string output = "{\"templates\":{";
    bool first = true;
    for (const std::shared_ptr<const Template>& body : policies.Templates())
    {
        if (!first) { output.push_back(','); }
        first = false;
        detail::AppendJsonString(output, body->Id().Text());
        output.push_back(':');
        AppendTemplate(output, *body, nullptr);
    }
    output += "},\"staticPolicies\":{";
    first = true;
    for (const std::shared_ptr<const Policy>& policy : policies.Policies())
    {
        if (!policy->IsStatic()) { continue; }
        if (!first) { output.push_back(','); }
        first = false;
        detail::AppendJsonString(output, policy->Id().Text());
        output.push_back(':');
        AppendTemplate(output, policy->GetTemplate(), nullptr);
    }
    output += "},\"templateLinks\":[";
    first = true;
    for (const std::shared_ptr<const Policy>& policy : policies.Policies())
    {
        if (policy->IsStatic()) { continue; }
        if (!first) { output.push_back(','); }
        first = false;
        output += "{\"templateId\":";
        detail::AppendJsonString(output, policy->GetTemplate().Id().Text());
        output += ",\"newId\":";
        detail::AppendJsonString(output, policy->Id().Text());
        output += ",\"values\":{";
        bool firstValue = true;
        for (const SlotId slot : {SlotId::Principal, SlotId::Resource})
        {
            const EntityUid* bound = policy->Binding(slot);
            if (bound == nullptr) { continue; }
            if (!firstValue) { output.push_back(','); }
            firstValue = false;
            output.push_back('"');
            output += SlotText(slot);
            output += "\":";
            AppendEscapedUid(output, *bound);
        }
        output += "}}";
    }
    output += "]}";
    return output;
}

} // namespace cedar
