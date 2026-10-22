#include <cedar/Names.hpp>

#include "model/EscapeDebug.hpp"
#include "model/SymbolTable.hpp"

namespace cedar
{

namespace
{

constexpr std::string_view Separator = "::";

// Splits the next component off the front of a namespace path.
auto PopComponent(std::string_view& path) noexcept -> std::string_view
{
    const std::size_t separator = path.find(Separator);
    const std::string_view component = path.substr(0, separator);
    path = separator == std::string_view::npos ? std::string_view{} : path.substr(separator + Separator.size());
    return component;
}

} // namespace

// ---- Symbol ----

auto Symbol::Intern(const std::string_view text) -> Symbol
{
    return Symbol{detail::SymbolTable::Instance().Intern(text)};
}

auto Symbol::Find(const std::string_view text) -> std::optional<Symbol>
{
    if (const auto index = detail::SymbolTable::Instance().Find(text)) { return Symbol{*index}; }
    return std::nullopt;
}

auto Symbol::Text() const noexcept -> std::string_view
{
    return detail::SymbolTable::Instance().Text(index_);
}

auto Symbol::Hash() const noexcept -> std::uint64_t
{
    return detail::SymbolTable::Instance().Hash(index_);
}

// ---- Identifier ----

auto Identifier::Parse(const std::string_view text) -> std::expected<Identifier, NameError>
{
    if (const auto checked = CheckIdentifier(text); !checked) { return std::unexpected{checked.error()}; }
    return Identifier{Symbol::Intern(text)};
}

// ---- Name ----

namespace
{

// Precondition: `text` passed CheckName.
auto FromNormalizedText(const std::string_view text) -> Name
{
    const std::size_t separator = text.rfind(Separator);
    if (separator == std::string_view::npos) { return Name::Unqualified(*Identifier::Parse(text)); }
    const std::string_view basename = text.substr(separator + Separator.size());
    std::vector<Identifier> path;
    std::string_view namespaceText = text.substr(0, separator);
    while (!namespaceText.empty()) { path.push_back(*Identifier::Parse(PopComponent(namespaceText))); }
    return Name::Qualified(path, *Identifier::Parse(basename));
}

} // namespace

auto Name::Parse(const std::string_view text) -> std::expected<Name, NameError>
{
    if (const auto checked = CheckName(text); !checked) { return std::unexpected{checked.error()}; }
    return FromNormalizedText(text);
}

auto Name::ParseInternal(const std::string_view text) -> std::expected<Name, NameError>
{
    if (const auto checked = CheckName(text, true); !checked) { return std::unexpected{checked.error()}; }
    return FromNormalizedText(text);
}

auto Name::Find(const std::string_view text) -> std::optional<Name>
{
    if (!CheckName(text)) { return std::nullopt; }
    const std::optional<Symbol> qualified = Symbol::Find(text);
    if (!qualified) { return std::nullopt; }
    const std::size_t separator = text.rfind(Separator);
    const std::string_view basenameText =
        separator == std::string_view::npos ? text : text.substr(separator + Separator.size());
    // Making a name interns its basename, so a qualified symbol without one was interned as
    // something other than a name.
    const std::optional<Symbol> basename = Symbol::Find(basenameText);
    if (!basename) { return std::nullopt; }
    return Name{*qualified, *basename};
}

auto Name::Unqualified(const Identifier basename) -> Name
{
    return Name{basename.GetSymbol(), basename.GetSymbol()};
}

auto Name::Qualified(const std::span<const Identifier> namespacePath, const Identifier basename) -> Name
{
    if (namespacePath.empty()) { return Unqualified(basename); }
    std::string text;
    for (const Identifier component : namespacePath)
    {
        text += component.Text();
        text += Separator;
    }
    text += basename.Text();
    return Name{Symbol::Intern(text), basename.GetSymbol()};
}

auto Name::NamespaceText() const noexcept -> std::string_view
{
    if (IsUnqualified()) { return {}; }
    const std::string_view qualified = QualifiedText();
    return qualified.substr(0, qualified.size() - basename_.Text().size() - Separator.size());
}

auto Name::NamespaceComponents() const -> std::vector<Identifier>
{
    std::vector<Identifier> components;
    std::string_view path = NamespaceText();
    while (!path.empty()) { components.push_back(Identifier{Symbol::Intern(PopComponent(path))}); }
    return components;
}

auto Name::IsReserved() const noexcept -> bool
{
    std::string_view rest = QualifiedText();
    while (!rest.empty())
    {
        if (PopComponent(rest) == ReservedNamespaceName) { return true; }
    }
    return false;
}

auto Name::QualifyWith(const Name& namespaceName) const -> Name
{
    if (!IsUnqualified()) { return *this; }
    std::string text{namespaceName.QualifiedText()};
    text += Separator;
    text += basename_.Text();
    return Name{Symbol::Intern(text), basename_};
}

auto operator<=>(const Name& left, const Name& right) noexcept -> std::strong_ordering
{
    if (left == right) { return std::strong_ordering::equal; }
    if (left.basename_ != right.basename_)
    {
        if (const auto byBasename = left.basename_.Text() <=> right.basename_.Text(); byBasename != 0)
        {
            return byBasename;
        }
    }
    // Lexicographic over the path components, as Rust's `Vec<Id>` compares.
    std::string_view leftPath = left.NamespaceText();
    std::string_view rightPath = right.NamespaceText();
    while (!leftPath.empty() && !rightPath.empty())
    {
        if (const auto byComponent = PopComponent(leftPath) <=> PopComponent(rightPath); byComponent != 0)
        {
            return byComponent;
        }
    }
    return !leftPath.empty() <=> !rightPath.empty();
}

// ---- EntityType ----

auto EntityType::Parse(const std::string_view text) -> std::expected<EntityType, NameError>
{
    return Name::Parse(text).transform([](const Name name) { return EntityType{name}; });
}

auto EntityType::Find(const std::string_view text) -> std::optional<EntityType>
{
    return Name::Find(text).transform([](const Name name) { return EntityType{name}; });
}

auto EntityType::IsAction() const noexcept -> bool
{
    return name_.Basename().Text() == "Action";
}

// ---- Eid ----

auto Eid::Escaped() const -> std::string
{
    std::string escaped;
    escaped.reserve(value_.size());
    detail::AppendEscapeDebug(escaped, value_);
    return escaped;
}

// ---- EntityUid ----

EntityUid::EntityUid(EntityType type, Eid id) noexcept
    : type_(type), id_(std::move(id))
{
    // boost::hash_combine's mixing step, widened to 64 bits.
    std::uint64_t hash = type_.Hash();
    hash ^= id_.Hash() + 0x9E37'79B9'7F4A'7C15ULL + (hash << 6) + (hash >> 2);
    hash_ = hash;
}

auto EntityUid::Create(const std::string_view typeName, const std::string_view id) -> std::expected<EntityUid, NameError>
{
    return EntityType::Parse(typeName).transform([&](const EntityType type) { return EntityUid{type, Eid{id}}; });
}

auto EntityUid::ToString() const -> std::string
{
    std::string text{type_.GetName().QualifiedText()};
    text += "::\"";
    detail::AppendEscapeDebug(text, id_.Text());
    text += '"';
    return text;
}

} // namespace cedar
