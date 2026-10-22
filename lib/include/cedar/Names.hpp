#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cedar
{

// ---- Lexical rules ----

/// Why a string is not a valid identifier or name.
enum class NameError : std::uint8_t
{
    /// Not of the form `[_a-zA-Z][_a-zA-Z0-9]*`, or for a name, such identifiers joined by `::`
    /// with no whitespace.
    InvalidSyntax,
    /// A keyword Cedar reserves: `true false if then else in is like has`.
    ReservedKeyword,
    /// `__cedar`, which only Cedar's own built-in names may use.
    ReservedNamespace,
};

/// The namespace that holds Cedar's built-in types (`__cedar::Long` and so on).
inline constexpr std::string_view ReservedNamespaceName = "__cedar";

/// The keywords that can never be an identifier (`cedar-policy-core/src/ast/name.rs`,
/// RESERVED_KEYWORDS). Every other identifier-shaped word, including `principal`, `permit` and
/// `when`, is a valid identifier.
inline constexpr std::array<std::string_view, 9> ReservedKeywords{
    "true", "false", "if", "then", "else", "in", "is", "like", "has",
};

/// True when `text` matches `[_a-zA-Z][_a-zA-Z0-9]*`, the lexical shape of every identifier.
/// Attribute names and annotation keys need only this; reserved keywords pass.
[[nodiscard]] constexpr auto IsIdentifierSyntax(const std::string_view text) noexcept -> bool
{
    const auto isHead = [](const char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto isTail = [&](const char c) { return isHead(c) || (c >= '0' && c <= '9'); };
    if (text.empty() || !isHead(text.front())) { return false; }
    for (const char c : text.substr(1))
    {
        if (!isTail(c)) { return false; }
    }
    return true;
}

/// True when `text` is one of ReservedKeywords.
[[nodiscard]] constexpr auto IsReservedKeyword(const std::string_view text) noexcept -> bool
{
    for (const std::string_view keyword : ReservedKeywords)
    {
        if (text == keyword) { return true; }
    }
    return false;
}

/// Checks one identifier the way Cedar's `Id` does: identifier syntax and not a reserved keyword.
/// `__cedar` is accepted here; Name::Parse is where it is refused.
[[nodiscard]] constexpr auto CheckIdentifier(const std::string_view text) noexcept -> std::expected<void, NameError>
{
    if (!IsIdentifierSyntax(text)) { return std::unexpected{NameError::InvalidSyntax}; }
    if (IsReservedKeyword(text)) { return std::unexpected{NameError::ReservedKeyword}; }
    return {};
}

/// Checks a normalized qualified name `A::B::C` (no whitespace, no comments). Every component must
/// pass CheckIdentifier. `__cedar` as a component is refused unless `allowReservedNamespace`,
/// which only Cedar's built-in schema names use (Rust's `InternalName`).
[[nodiscard]] constexpr auto CheckName(std::string_view text, const bool allowReservedNamespace = false) noexcept
    -> std::expected<void, NameError>
{
    std::optional<NameError> firstReservation;
    while (true)
    {
        const std::size_t separator = text.find("::");
        const std::string_view component = text.substr(0, separator);
        if (!IsIdentifierSyntax(component)) { return std::unexpected{NameError::InvalidSyntax}; }
        if (!firstReservation)
        {
            if (IsReservedKeyword(component)) { firstReservation = NameError::ReservedKeyword; }
            else if (!allowReservedNamespace && component == ReservedNamespaceName)
            {
                firstReservation = NameError::ReservedNamespace;
            }
        }
        if (separator == std::string_view::npos) { break; }
        text.remove_prefix(separator + 2);
    }
    if (firstReservation) { return std::unexpected{*firstReservation}; }
    return {};
}

// ---- Symbol ----

/// An interned identifier: a 32-bit handle into one process-wide, append-only table.
///
/// Two symbols are equal exactly when their text is equal, so comparing names, entity types,
/// action names and annotation keys is an integer compare. Interned text lives until the process
/// exits. Only identifier-shaped text belongs here (the vocabulary of policies and schemas); entity
/// ids, attribute keys and string values come from request data and are never interned.
///
/// Entity type names are interned wherever they come from, request data included, so the table
/// grows with the distinct type names a process sees (not with the number of requests or
/// entities). Readers that check data against a schema look type names up with Name::Find and
/// EntityType::Find, which never intern, so a name the schema does not declare is rejected without
/// growing the table.
///
/// Symbol has equality but no ordering, because its index reflects interning order. Order by Text().
/// The default-constructed symbol is the empty string. All member functions are thread-safe.
class Symbol final
{
public:
    constexpr Symbol() noexcept = default;

    /// Returns the symbol for `text`, adding it to the table if it is new.
    [[nodiscard]] static auto Intern(std::string_view text) -> Symbol;

    /// Returns the symbol for `text` if it has been interned, without adding it.
    [[nodiscard]] static auto Find(std::string_view text) -> std::optional<Symbol>;

    /// The interned text. The view stays valid for the life of the process.
    [[nodiscard]] auto Text() const noexcept -> std::string_view;

    /// A hash of the text, computed once at interning. It depends only on the text, not on the
    /// interning order, so hash-based iteration order is reproducible across runs.
    [[nodiscard]] auto Hash() const noexcept -> std::uint64_t;

    /// The dense table index, for tables keyed by symbol. Not an ordering.
    [[nodiscard]] constexpr auto Index() const noexcept -> std::uint32_t { return index_; }

    friend constexpr auto operator==(Symbol, Symbol) noexcept -> bool = default;

private:
    explicit constexpr Symbol(const std::uint32_t index) noexcept : index_(index) {}

    std::uint32_t index_ = 0;
};

// ---- Identifier ----

/// A validated, interned identifier (Rust's `Id`): identifier syntax, not a reserved keyword. It may
/// be `__cedar`; IsReserved() reports that. Ordered by text, byte-wise.
class Identifier final
{
public:
    /// Validates with CheckIdentifier and interns.
    [[nodiscard]] static auto Parse(std::string_view text) -> std::expected<Identifier, NameError>;

    [[nodiscard]] constexpr auto GetSymbol() const noexcept -> Symbol { return symbol_; }
    [[nodiscard]] auto Text() const noexcept -> std::string_view { return symbol_.Text(); }

    /// True for `__cedar`.
    [[nodiscard]] auto IsReserved() const noexcept -> bool { return Text() == ReservedNamespaceName; }

    friend constexpr auto operator==(Identifier, Identifier) noexcept -> bool = default;
    friend auto operator<=>(const Identifier left, const Identifier right) noexcept -> std::strong_ordering
    {
        return left == right ? std::strong_ordering::equal : left.Text() <=> right.Text();
    }

private:
    friend class Name;
    explicit constexpr Identifier(const Symbol symbol) noexcept : symbol_(symbol) {}

    Symbol symbol_;
};

// ---- Name ----

/// A possibly qualified name `A::B::C`: a namespace path (`A::B`) and a basename (`C`). Used for
/// entity types, extension function names, schema common types and namespaces.
///
/// Stored as two symbols, the full qualified text and the basename, so equality and hashing are
/// integer operations and the namespace text is a view into interned storage.
///
/// The order matches Rust's derived `Ord` on `InternalName`: basename first, then the namespace
/// path compared component by component (a shorter path that is a prefix comes first). This is not
/// the byte order of the qualified text, and Set ordering over entity uids depends on it.
class Name final
{
public:
    /// Parses a normalized name (Rust's `Name::from_normalized_str`): CheckName with `__cedar`
    /// refused.
    [[nodiscard]] static auto Parse(std::string_view text) -> std::expected<Name, NameError>;

    /// Parses a normalized name that may use the `__cedar` namespace (Rust's `InternalName`), for
    /// Cedar's built-in schema names.
    [[nodiscard]] static auto ParseInternal(std::string_view text) -> std::expected<Name, NameError>;

    /// The name `text` (a normalized name, as Parse takes) if one has been made already, without
    /// interning anything. Nothing when `text` is not a valid name (CheckName tells the cases
    /// apart) or no such name exists yet. Schema-based loaders use it: a type the schema declares
    /// exists, so a type name that is not found is not declared.
    [[nodiscard]] static auto Find(std::string_view text) -> std::optional<Name>;

    /// The name with no namespace.
    [[nodiscard]] static auto Unqualified(Identifier basename) -> Name;

    /// `namespacePath[0]::...::namespacePath[n-1]::basename`.
    [[nodiscard]] static auto Qualified(std::span<const Identifier> namespacePath, Identifier basename) -> Name;

    [[nodiscard]] constexpr auto Basename() const noexcept -> Identifier { return Identifier{basename_}; }

    /// The full text, `A::B::C`. Valid for the life of the process.
    [[nodiscard]] auto QualifiedText() const noexcept -> std::string_view { return qualified_.Text(); }

    /// The namespace path as text, `A::B`, or empty when unqualified.
    [[nodiscard]] auto NamespaceText() const noexcept -> std::string_view;

    /// The namespace path, outermost first.
    [[nodiscard]] auto NamespaceComponents() const -> std::vector<Identifier>;

    [[nodiscard]] constexpr auto IsUnqualified() const noexcept -> bool { return qualified_ == basename_; }

    /// True when any component is `__cedar`.
    [[nodiscard]] auto IsReserved() const noexcept -> bool;

    /// Prefixes `namespaceName` when this name is unqualified; returns this name unchanged when it
    /// is already qualified (Rust's `qualify_with`).
    [[nodiscard]] auto QualifyWith(const Name& namespaceName) const -> Name;

    /// The interned qualified text, for tables keyed by name.
    [[nodiscard]] constexpr auto QualifiedSymbol() const noexcept -> Symbol { return qualified_; }

    [[nodiscard]] auto Hash() const noexcept -> std::uint64_t { return qualified_.Hash(); }

    [[nodiscard]] auto ToString() const -> std::string { return std::string{QualifiedText()}; }

    friend constexpr auto operator==(const Name& left, const Name& right) noexcept -> bool
    {
        return left.qualified_ == right.qualified_;
    }
    friend auto operator<=>(const Name& left, const Name& right) noexcept -> std::strong_ordering;

private:
    constexpr Name(const Symbol qualified, const Symbol basename) noexcept : qualified_(qualified), basename_(basename) {}

    Symbol qualified_;
    Symbol basename_;
};

// ---- EntityType ----

/// The type of an entity: a Name. Ordered as its Name.
class EntityType final
{
public:
    explicit EntityType(const Name name) noexcept : name_(name) {}

    /// Parses a normalized name; `__cedar` is refused.
    [[nodiscard]] static auto Parse(std::string_view text) -> std::expected<EntityType, NameError>;

    /// The entity type `text` if its name exists already, without interning (Name::Find).
    [[nodiscard]] static auto Find(std::string_view text) -> std::optional<EntityType>;

    [[nodiscard]] constexpr auto GetName() const noexcept -> const Name& { return name_; }

    /// True when the basename is `Action`, in any namespace (Rust's `EntityType::is_action`).
    [[nodiscard]] auto IsAction() const noexcept -> bool;

    [[nodiscard]] auto Hash() const noexcept -> std::uint64_t { return name_.Hash(); }
    [[nodiscard]] auto ToString() const -> std::string { return name_.ToString(); }

    friend constexpr auto operator==(const EntityType&, const EntityType&) noexcept -> bool = default;
    friend auto operator<=>(const EntityType& left, const EntityType& right) noexcept -> std::strong_ordering
    {
        return left.name_ <=> right.name_;
    }

private:
    Name name_;
};

// ---- Eid ----

/// An entity id: any string, compared by content and never interned (entity ids come from request
/// data). Ordered byte-wise, which for UTF-8 is code point order, matching Rust's `str` order.
///
/// The text must be valid UTF-8 (a precondition, not checked here): the readers that make ids
/// from input (the policy parser, the JSON readers) validate it.
class Eid final
{
public:
    explicit Eid(std::string value) noexcept : value_(std::move(value)) {}
    explicit Eid(const std::string_view value) : value_(value) {}
    explicit Eid(const char* value) : value_(value) {}

    [[nodiscard]] auto Text() const noexcept -> std::string_view { return value_; }

    /// The id escaped as Cedar prints it between quotes (Rust's `Eid::escaped`, `str::escape_debug`).
    [[nodiscard]] auto Escaped() const -> std::string;

    [[nodiscard]] auto Hash() const noexcept -> std::uint64_t
    {
        return static_cast<std::uint64_t>(std::hash<std::string_view>{}(value_));
    }

    friend auto operator==(const Eid&, const Eid&) noexcept -> bool = default;
    friend auto operator<=>(const Eid&, const Eid&) noexcept -> std::strong_ordering = default;

private:
    std::string value_;
};

// ---- EntityUid ----

/// An entity's unique id: its type and its id, with the hash computed once at construction.
///
/// Equality compares the hash first. The order is type, then id (Rust's derived `Ord` on
/// `EntityUID`), so a set of uids sorts by Name order of the type, then byte order of the id.
class EntityUid final
{
public:
    EntityUid(EntityType type, Eid id) noexcept;

    /// `Type::"id"`. Fails when `typeName` is not a valid normalized name.
    [[nodiscard]] static auto Create(std::string_view typeName, std::string_view id)
        -> std::expected<EntityUid, NameError>;

    [[nodiscard]] constexpr auto Type() const noexcept -> const EntityType& { return type_; }
    [[nodiscard]] constexpr auto Id() const noexcept -> const Eid& { return id_; }

    /// True when the type is an action type (EntityType::IsAction).
    [[nodiscard]] auto IsAction() const noexcept -> bool { return type_.IsAction(); }

    [[nodiscard]] constexpr auto Hash() const noexcept -> std::uint64_t { return hash_; }

    /// Cedar syntax, `A::B::"escaped id"` (Rust's `Display for EntityUID`).
    [[nodiscard]] auto ToString() const -> std::string;

    friend auto operator==(const EntityUid& left, const EntityUid& right) noexcept -> bool
    {
        return left.hash_ == right.hash_ && left.type_ == right.type_ && left.id_ == right.id_;
    }
    friend auto operator<=>(const EntityUid& left, const EntityUid& right) noexcept -> std::strong_ordering
    {
        if (const auto byType = left.type_ <=> right.type_; byType != 0) { return byType; }
        return left.id_ <=> right.id_;
    }

private:
    EntityType type_;
    Eid id_;
    std::uint64_t hash_;
};

// ---- SlotId ----

/// A template slot. Principal orders before Resource, as in Rust.
enum class SlotId : std::uint8_t
{
    Principal,
    Resource,
};

/// `?principal` or `?resource`.
[[nodiscard]] constexpr auto ToString(const SlotId slot) noexcept -> std::string_view
{
    return slot == SlotId::Principal ? "?principal" : "?resource";
}

} // namespace cedar

template <>
struct std::hash<cedar::Symbol>
{
    [[nodiscard]] auto operator()(const cedar::Symbol symbol) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(symbol.Hash());
    }
};

template <>
struct std::hash<cedar::Identifier>
{
    [[nodiscard]] auto operator()(const cedar::Identifier identifier) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(identifier.GetSymbol().Hash());
    }
};

template <>
struct std::hash<cedar::Name>
{
    [[nodiscard]] auto operator()(const cedar::Name& name) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(name.Hash());
    }
};

template <>
struct std::hash<cedar::EntityType>
{
    [[nodiscard]] auto operator()(const cedar::EntityType& type) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(type.Hash());
    }
};

template <>
struct std::hash<cedar::Eid>
{
    [[nodiscard]] auto operator()(const cedar::Eid& id) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(id.Hash());
    }
};

template <>
struct std::hash<cedar::EntityUid>
{
    [[nodiscard]] auto operator()(const cedar::EntityUid& uid) const noexcept -> std::size_t
    {
        return static_cast<std::size_t>(uid.Hash());
    }
};
