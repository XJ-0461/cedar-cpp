#include "validation/StringChecks.hpp"

#include "model/EscapeDebug.hpp"
#include "validation/UnicodeSecurityTables.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar::detail
{

namespace
{

constexpr char32_t ReplacementCharacter = 0xFFFD;

struct DecodedCharacter
{
    char32_t value;
    std::string_view bytes;
};

// One code point from the front of `text`, which is not empty. A byte that does not start a
// well-formed sequence decodes as U+FFFD on its own.
[[nodiscard]] auto DecodeFront(const std::string_view text) noexcept -> DecodedCharacter
{
    const auto byteAt = [&](const std::size_t position) { return static_cast<std::uint8_t>(text[position]); };
    const std::uint8_t lead = byteAt(0);
    if (lead < 0x80) { return {lead, text.substr(0, 1)}; }
    struct Form
    {
        std::size_t length;
        std::uint8_t leadMask;
        char32_t minimum;
    };
    Form form{};
    if ((lead & 0xE0) == 0xC0) { form = {2, 0x1F, 0x80}; }
    else if ((lead & 0xF0) == 0xE0) { form = {3, 0x0F, 0x800}; }
    else if ((lead & 0xF8) == 0xF0) { form = {4, 0x07, 0x10000}; }
    else { return {ReplacementCharacter, text.substr(0, 1)}; }
    const std::size_t length = form.length;
    const char32_t minimum = form.minimum;
    char32_t value = lead & form.leadMask;
    if (text.size() < length) { return {ReplacementCharacter, text.substr(0, 1)}; }
    for (std::size_t position = 1; position < length; ++position)
    {
        const std::uint8_t continuation = byteAt(position);
        if ((continuation & 0xC0) != 0x80) { return {ReplacementCharacter, text.substr(0, 1)}; }
        value = (value << 6) | (continuation & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
    {
        return {ReplacementCharacter, text.substr(0, 1)};
    }
    return {value, text.substr(0, length)};
}

[[nodiscard]] auto Decode(std::string_view text) -> std::vector<DecodedCharacter>
{
    std::vector<DecodedCharacter> characters;
    while (!text.empty())
    {
        const DecodedCharacter character = DecodeFront(text);
        characters.push_back(character);
        text.remove_prefix(character.bytes.size());
    }
    return characters;
}

// `str_checks.rs` BIDI_CHARS: the embedding, override and isolate controls and their terminators.
[[nodiscard]] constexpr auto IsBidirectionalControl(const char32_t character) noexcept -> bool
{
    constexpr std::array<char32_t, 9> Controls{0x202A, 0x202B, 0x202D, 0x202E, 0x2066,
                                               0x2067, 0x2068, 0x202C, 0x2069};
    return std::ranges::find(Controls, character) != Controls.end();
}

[[nodiscard]] auto IdentifierAllowed(const char32_t character) noexcept -> bool
{
    const std::span<const CodePointRange> ranges = IdentifierAllowedRanges();
    const auto found = std::ranges::upper_bound(ranges, static_cast<std::uint32_t>(character), {},
                                                [](const CodePointRange& range) { return range.first; });
    return found != ranges.begin() && character <= std::prev(found)->last;
}

// `char::script_extension` as an augmented script set; Unknown is the empty set.
[[nodiscard]] auto ScriptSetOf(const char32_t character) noexcept -> ScriptSet
{
    const std::span<const ScriptSetRange> ranges = ScriptSetRanges();
    const auto found = std::ranges::upper_bound(ranges, static_cast<std::uint32_t>(character), {},
                                                [](const ScriptSetRange& range) { return range.first; });
    if (found == ranges.begin() || character > std::prev(found)->last) { return ScriptSet{{0, 0, 0}, 0}; }
    return ScriptSets()[std::prev(found)->set];
}

// `MixedScript::is_single_script`: the intersection of every character's augmented script set,
// starting from the full set, is not empty.
[[nodiscard]] auto IsSingleScript(const std::span<const DecodedCharacter> characters) noexcept -> bool
{
    constexpr std::uint64_t Every = std::numeric_limits<std::uint64_t>::max();
    ScriptSet resolved{{Every, Every, Every}, 0b111};
    for (const DecodedCharacter& character : characters)
    {
        const ScriptSet set = ScriptSetOf(character.value);
        for (std::size_t word = 0; word < resolved.scripts.size(); ++word) { resolved.scripts[word] &= set.scripts[word]; }
        resolved.writingSystems &= set.writingSystems;
    }
    return resolved.writingSystems != 0 ||
           std::ranges::any_of(resolved.scripts, [](const std::uint64_t word) { return word != 0; });
}

[[nodiscard]] constexpr auto IsAsciiGraphic(const char32_t character) noexcept -> bool
{
    return character >= 0x21 && character <= 0x7E;
}

[[nodiscard]] auto Escaped(const std::string_view text) -> std::string
{
    std::string escaped;
    AppendEscapeDebug(escaped, text);
    return escaped;
}

// Rust's `Display` for a pattern: each character through `char::escape_debug`, a literal `*` as
// `\*`, and a wildcard as `*`.
[[nodiscard]] auto PatternText(const Pattern& pattern) -> std::string
{
    std::string text;
    bool firstLiteral = true;
    for (const std::string_view literal : pattern.Literals())
    {
        if (!firstLiteral) { text += '*'; }
        firstLiteral = false;
        std::string_view rest = literal;
        while (true)
        {
            const std::size_t star = rest.find('*');
            AppendCharacterEscapeDebug(text, rest.substr(0, star));
            if (star == std::string_view::npos) { break; }
            text += "\\*";
            rest.remove_prefix(star + 1);
        }
    }
    return text;
}

class Checker
{
public:
    explicit Checker(const Template& body) noexcept : body_{body} {}

    // Rust's `permissable_str`.
    void CheckString(const std::string_view text, const std::optional<SourceSpan> span)
    {
        const std::vector<DecodedCharacter> characters = Decode(text);
        if (std::ranges::any_of(characters, [](const DecodedCharacter& c) { return IsBidirectionalControl(c.value); }))
        {
            Add(DiagnosticCode::BidirectionalCharactersInString,
                std::format("string `\"{}\"` contains bidirectional control characters", Escaped(text)), span);
        }
        else if (!IsSingleScript(characters))
        {
            Add(DiagnosticCode::MixedScriptString, std::format("string `\"{}\"` contains mixed scripts", Escaped(text)),
                span);
        }
    }

    // Rust's `permissable_ident`.
    void CheckIdentifier(const std::string_view text, const std::optional<SourceSpan> span)
    {
        const std::vector<DecodedCharacter> characters = Decode(text);
        const auto confusable = std::ranges::find_if(characters, [](const DecodedCharacter& c) {
            return c.value != U' ' && !IsAsciiGraphic(c.value) && !IdentifierAllowed(c.value);
        });
        if (std::ranges::any_of(characters, [](const DecodedCharacter& c) { return IsBidirectionalControl(c.value); }))
        {
            Add(DiagnosticCode::BidirectionalCharactersInIdentifier,
                std::format("identifier `{}` contains bidirectional control characters", Escaped(text)), span);
        }
        else if (confusable != characters.end())
        {
            std::string character;
            AppendCharacterEscapeDebug(character, confusable->bytes);
            Add(DiagnosticCode::ConfusableIdentifier,
                std::format("identifier `{}` contains the character `{}`, which is not printable ASCII and is not "
                            "allowed in identifiers by the Unicode General Security Profile",
                            Escaped(text), character),
                span);
        }
        else if (!IsSingleScript(characters))
        {
            Add(DiagnosticCode::MixedScriptIdentifier,
                std::format("identifier `{}` contains mixed scripts", Escaped(text)), span);
        }
    }

    // Rust's `text_in_name`: each namespace component, then the basename.
    void CheckName(const Name& name, const std::optional<SourceSpan> span)
    {
        for (const Identifier& component : name.NamespaceComponents()) { CheckIdentifier(component.Text(), span); }
        CheckIdentifier(name.Basename().Text(), span);
    }

    [[nodiscard]] auto Take() noexcept -> std::vector<Diagnostic> { return std::move(warnings_); }

private:
    void Add(const DiagnosticCode code, std::string message, const std::optional<SourceSpan> span)
    {
        warnings_.push_back(Diagnostic{.code = code,
                                       .severity = Severity::Warning,
                                       .message = std::move(message),
                                       .help = std::nullopt,
                                       .primary = span,
                                       .labels = {},
                                       .source = body_.Source()});
    }

    const Template& body_;
    std::vector<Diagnostic> warnings_;
};

} // namespace

auto StringCheckWarnings(const Template& body) -> std::vector<Diagnostic>
{
    const ExpressionArena& arena = body.Arena();
    Checker checker{body};

    // Rust's `ExprIterator` over `Template::condition`: a node's text, then its children from the
    // last pushed to the first. Record values are pushed in key order, as Rust's BTreeMap holds
    // them. A folded node stands for its written form.
    std::vector<ExpressionIndex> pending{body.Condition()};
    std::vector<RecordLiteralField> sortedFields;
    while (!pending.empty())
    {
        const ExpressionIndex index = pending.back();
        pending.pop_back();
        const std::optional<SourceSpan> span = arena.Span(index);
        arena.Visit(index, Overloaded{
            [&](const node::Literal& literal) {
                if (literal.value.Kind() == ValueKind::String) { checker.CheckString(literal.value.AsString(), span); }
                else if (literal.value.Kind() == ValueKind::Entity)
                {
                    const EntityUid& uid = literal.value.AsEntity();
                    checker.CheckName(uid.Type().GetName(), span);
                    checker.CheckIdentifier(uid.Id().Text(), span);
                }
            },
            [&](const node::Folded& folded) { pending.push_back(folded.written); },
            [](const node::Variable&) {},
            [](const node::Slot&) {},
            [&](const node::If& branch) {
                pending.push_back(branch.condition);
                pending.push_back(branch.thenBranch);
                pending.push_back(branch.elseBranch);
            },
            [&](const node::And& conjunction) {
                pending.push_back(conjunction.left);
                pending.push_back(conjunction.right);
            },
            [&](const node::Or& disjunction) {
                pending.push_back(disjunction.left);
                pending.push_back(disjunction.right);
            },
            [&](const node::Unary& unary) { pending.push_back(unary.operand); },
            [&](const node::Binary& binary) {
                pending.push_back(binary.left);
                pending.push_back(binary.right);
            },
            [&](const node::ExtensionCall& call) {
                checker.CheckName(call.function, span);
                const std::span<const ExpressionIndex> arguments = arena.Elements(call.arguments);
                pending.insert(pending.end(), arguments.begin(), arguments.end());
            },
            [&](const node::GetAttribute& get) {
                checker.CheckIdentifier(get.attribute, span);
                pending.push_back(get.object);
            },
            [&](const node::HasAttribute& has) {
                for (const std::string_view attribute : arena.Elements(has.path)) { checker.CheckIdentifier(attribute, span); }
                pending.push_back(has.object);
            },
            [&](const node::Like& like) {
                checker.CheckString(PatternText(arena.PatternOf(like)), span);
                pending.push_back(like.operand);
            },
            [&](const node::Is& is) { pending.push_back(is.operand); },
            [&](const node::SetLiteral& set) {
                const std::span<const ExpressionIndex> elements = arena.Elements(set.elements);
                pending.insert(pending.end(), elements.begin(), elements.end());
            },
            [&](const node::RecordLiteral& record) {
                const std::span<const RecordLiteralField> fields = arena.Elements(record.fields);
                sortedFields.assign(fields.begin(), fields.end());
                std::ranges::sort(sortedFields, {}, &RecordLiteralField::key);
                for (const RecordLiteralField& field : sortedFields) { checker.CheckIdentifier(field.key, span); }
                for (const RecordLiteralField& field : sortedFields) { pending.push_back(field.value); }
            },
        });
    }
    return checker.Take();
}

} // namespace cedar::detail
