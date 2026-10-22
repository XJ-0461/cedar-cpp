#pragma once

#include <cedar/ExtensionValues.hpp>
#include <cedar/Names.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cedar
{

// Ownership model
//
// A Value is a 24-byte trivially copyable view. Scalars (bool, long, the extension values) are held
// inline. A string, an entity uid, a set's elements and a record's entries are held by pointer into
// storage the Value does not own. That storage belongs to whatever outlives every view of it:
//
// - the entity store (attribute and tag values, built once at load),
// - the request (principal, action, resource, context),
// - a policy (literals in its expression arena, slot bindings),
// - a ValueArena (a per-evaluation arena for set and record literals, or the arena an owner builds
//   values into),
// - an OwnedValue, which bundles a Value with a shared ValueArena.
//
// Copying a Value never allocates and never counts references. Values that leave the library
// through the public API are OwnedValues.

class Value;
struct RecordEntry;

/// The kind of a Value. The enumerator order is Cedar's order across kinds (Rust's derived `Ord` on
/// `ValueKind` and `Literal`, then extension values by type name: `datetime`, `decimal`,
/// `duration`, `ipaddr`), so values of different kinds compare by kind.
enum class ValueKind : std::uint8_t
{
    Bool,
    Long,
    String,
    Entity,
    Set,
    Record,
    Datetime,
    Decimal,
    Duration,
    IpAddress,
};

/// A view of a Cedar set: elements sorted by Cedar's total order on values, with no duplicates.
/// That invariant gives linear equality, membership by binary search and the subset and
/// intersection tests by merge. Sets are made by ValueArena::MakeSet, which sorts and deduplicates.
class Set final
{
public:
    /// The empty set.
    constexpr Set() noexcept = default;

    /// Wraps elements that are already sorted by Compare with no two equal. The caller guarantees
    /// the order (a debug build checks it); ValueArena::MakeSet is the usual way to get it.
    [[nodiscard]] static auto FromSortedUnique(std::span<const Value> elements) noexcept -> Set;

    [[nodiscard]] constexpr auto Elements() const noexcept -> std::span<const Value>;
    [[nodiscard]] constexpr auto Size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] constexpr auto IsEmpty() const noexcept -> bool { return size_ == 0; }
    [[nodiscard]] constexpr auto begin() const noexcept -> const Value* { return data_; }
    [[nodiscard]] constexpr auto end() const noexcept -> const Value*;

    /// Cedar's `contains`: whether `element` is in the set. O(log n).
    [[nodiscard]] auto Contains(const Value& element) const noexcept -> bool;

    /// Cedar's `containsAll`: whether every element of `other` is in this set. O(n + m).
    [[nodiscard]] auto ContainsAll(Set other) const noexcept -> bool;

    /// Cedar's `containsAny`: whether the two sets share an element. O(n + m).
    [[nodiscard]] auto ContainsAny(Set other) const noexcept -> bool;

private:
    friend class Value;
    constexpr Set(const Value* data, const std::size_t size) noexcept : data_{data}, size_{size} {}

    const Value* data_ = nullptr;
    std::size_t size_ = 0;
};

/// A view of a Cedar record: entries sorted by key, byte-wise (Rust's `BTreeMap<SmolStr, Value>`
/// order), with no duplicate keys. Records are made by ValueArena::MakeRecord.
class Record final
{
public:
    /// The empty record.
    constexpr Record() noexcept = default;

    /// Wraps entries that are already sorted by key with no duplicate key. The caller guarantees
    /// the order (a debug build checks it).
    [[nodiscard]] static auto FromSortedUnique(std::span<const RecordEntry> entries) noexcept -> Record;

    [[nodiscard]] constexpr auto Entries() const noexcept -> std::span<const RecordEntry>;
    [[nodiscard]] constexpr auto Size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] constexpr auto IsEmpty() const noexcept -> bool { return size_ == 0; }
    [[nodiscard]] constexpr auto begin() const noexcept -> const RecordEntry* { return data_; }
    [[nodiscard]] constexpr auto end() const noexcept -> const RecordEntry*;

    /// The value stored under `key`, or null when the record has no such key. O(log n).
    [[nodiscard]] auto Find(std::string_view key) const noexcept -> const Value*;

    /// Cedar's `has` on a record.
    [[nodiscard]] auto Contains(const std::string_view key) const noexcept -> bool { return Find(key) != nullptr; }

private:
    friend class Value;
    constexpr Record(const RecordEntry* data, const std::size_t size) noexcept : data_{data}, size_{size} {}

    const RecordEntry* data_ = nullptr;
    std::size_t size_ = 0;
};

/// A Cedar value: a closed tagged union over bool, long (i64), string, entity uid, set, record and
/// the four extension types. See the ownership model above: a Value is a view, valid while the
/// storage it points into lives.
///
/// Equality is structural and total: values of different kinds are unequal, sets compare as sets,
/// records by key and value, extension values by value. The order (`<=>`) is Cedar's total order
/// (Rust's `Ord` on `Value`), which fixes the element order of every set and so the printed order.
/// Within a kind: false < true; longs numerically; strings byte-wise (code point order for UTF-8);
/// entity uids by type (EntityType order) then id; sets and records lexicographically over their
/// sorted elements or (key, value) entries; extension values as their structs order.
///
/// The As accessors require the matching kind.
class Value final
{
public:
    [[nodiscard]] static constexpr auto FromBool(const bool value) noexcept -> Value
    {
        Value result{ValueKind::Bool};
        result.payload_.boolean = value;
        return result;
    }

    [[nodiscard]] static constexpr auto FromLong(const std::int64_t value) noexcept -> Value
    {
        Value result{ValueKind::Long};
        result.payload_.integer = value;
        return result;
    }

    /// A view of `text`; the characters must outlive the value. `text` must be valid UTF-8 (a
    /// precondition, not checked here): the readers that make strings from input validate it.
    [[nodiscard]] static constexpr auto FromString(const std::string_view text) noexcept -> Value
    {
        Value result{ValueKind::String};
        result.payload_.string = {text.data(), text.size()};
        return result;
    }

    /// A view of `uid`; the uid must outlive the value.
    [[nodiscard]] static constexpr auto FromEntity(const EntityUid& uid) noexcept -> Value
    {
        Value result{ValueKind::Entity};
        result.payload_.entity = &uid;
        return result;
    }

    [[nodiscard]] static constexpr auto FromSet(const Set set) noexcept -> Value
    {
        Value result{ValueKind::Set};
        result.payload_.set = {set.data_, set.size_};
        return result;
    }

    [[nodiscard]] static constexpr auto FromRecord(const Record record) noexcept -> Value
    {
        Value result{ValueKind::Record};
        result.payload_.record = {record.data_, record.size_};
        return result;
    }

    [[nodiscard]] static constexpr auto FromDecimal(const Decimal value) noexcept -> Value
    {
        Value result{ValueKind::Decimal};
        result.payload_.scalar = value.scaledValue;
        return result;
    }

    [[nodiscard]] static constexpr auto FromDatetime(const Datetime value) noexcept -> Value
    {
        Value result{ValueKind::Datetime};
        result.payload_.scalar = value.millisecondsSinceEpoch;
        return result;
    }

    [[nodiscard]] static constexpr auto FromDuration(const Duration value) noexcept -> Value
    {
        Value result{ValueKind::Duration};
        result.payload_.scalar = value.milliseconds;
        return result;
    }

    [[nodiscard]] static constexpr auto FromIpAddress(const IpAddress& value) noexcept -> Value
    {
        Value result{ValueKind::IpAddress};
        result.payload_.address = {value.high, value.low};
        result.ipVersion_ = value.version;
        result.ipPrefixLength_ = value.prefixLength;
        return result;
    }

    [[nodiscard]] constexpr auto Kind() const noexcept -> ValueKind { return kind_; }

    [[nodiscard]] constexpr auto AsBool() const noexcept -> bool { return payload_.boolean; }
    [[nodiscard]] constexpr auto AsLong() const noexcept -> std::int64_t { return payload_.integer; }
    [[nodiscard]] constexpr auto AsString() const noexcept -> std::string_view
    {
        return {payload_.string.data, payload_.string.size};
    }
    [[nodiscard]] constexpr auto AsEntity() const noexcept -> const EntityUid& { return *payload_.entity; }
    [[nodiscard]] constexpr auto AsSet() const noexcept -> Set { return Set{payload_.set.data, payload_.set.size}; }
    [[nodiscard]] constexpr auto AsRecord() const noexcept -> Record
    {
        return Record{payload_.record.data, payload_.record.size};
    }
    [[nodiscard]] constexpr auto AsDecimal() const noexcept -> Decimal { return Decimal{payload_.scalar}; }
    [[nodiscard]] constexpr auto AsDatetime() const noexcept -> Datetime { return Datetime{payload_.scalar}; }
    [[nodiscard]] constexpr auto AsDuration() const noexcept -> Duration { return Duration{payload_.scalar}; }
    [[nodiscard]] constexpr auto AsIpAddress() const noexcept -> IpAddress
    {
        return IpAddress{ipVersion_, payload_.address.high, payload_.address.low, ipPrefixLength_};
    }

    /// Cedar's total order on values (see the class comment).
    [[nodiscard]] static auto Compare(const Value& left, const Value& right) noexcept -> std::strong_ordering;

    /// Structural equality; cheaper than `Compare(...) == 0` because it stops at the first size or
    /// kind mismatch.
    [[nodiscard]] static auto Equal(const Value& left, const Value& right) noexcept -> bool;

    friend auto operator==(const Value& left, const Value& right) noexcept -> bool { return Equal(left, right); }
    friend auto operator<=>(const Value& left, const Value& right) noexcept -> std::strong_ordering
    {
        return Compare(left, right);
    }

private:
    explicit constexpr Value(const ValueKind kind) noexcept : kind_{kind} {}

    struct StringPayload
    {
        const char* data;
        std::size_t size;
    };
    struct SetPayload
    {
        const Value* data;
        std::size_t size;
    };
    struct RecordPayload
    {
        const RecordEntry* data;
        std::size_t size;
    };
    struct AddressPayload
    {
        std::uint64_t high;
        std::uint64_t low;
    };
    union Payload
    {
        bool boolean;
        std::int64_t integer;
        StringPayload string;
        const EntityUid* entity;
        SetPayload set;
        RecordPayload record;
        /// Decimal, Datetime and Duration.
        std::int64_t scalar;
        AddressPayload address;
    };

    Payload payload_{.integer = 0};
    IpVersion ipVersion_ = IpVersion::V4;
    std::uint8_t ipPrefixLength_ = 0;
    ValueKind kind_;
};

static_assert(sizeof(Value) == 24);
static_assert(std::is_trivially_copyable_v<Value>);
static_assert(std::is_trivially_destructible_v<Value>);

/// One record field: the key (a view, like the value) and its value.
struct RecordEntry
{
    std::string_view key;
    Value value;
};

static_assert(sizeof(RecordEntry) == 40);

constexpr auto Set::Elements() const noexcept -> std::span<const Value> { return {data_, size_}; }
constexpr auto Set::end() const noexcept -> const Value* { return data_ + size_; }
constexpr auto Record::Entries() const noexcept -> std::span<const RecordEntry> { return {data_, size_}; }
constexpr auto Record::end() const noexcept -> const RecordEntry* { return data_ + size_; }

/// A record whose construction met the same key twice. Cedar rejects duplicate keys in record
/// literals (a parse error) and in JSON (an entities or context error); the caller reports it.
struct DuplicateRecordKey
{
    std::string_view key;
};

/// Storage for the parts of values that a Value only points to: string characters, entity uids,
/// set elements and record entries. Allocation is monotonic (nothing is freed until the arena is
/// destroyed), from an optional caller-supplied initial buffer and then the heap. Destroying the
/// arena invalidates every view into it.
///
/// Typical owners: the evaluator (one arena per evaluation, starting on a stack buffer, for set and
/// record literals), the entity store and the request (values loaded from JSON), and OwnedValue.
/// Not thread-safe while values are being built; the views it backs may be read from any thread.
class ValueArena final
{
public:
    /// An arena that allocates from the heap.
    ValueArena() noexcept;

    /// An arena that allocates from `initialBuffer` first, then from the heap. The buffer must
    /// outlive the arena.
    explicit ValueArena(std::span<std::byte> initialBuffer) noexcept;

    ValueArena(const ValueArena&) = delete;
    auto operator=(const ValueArena&) -> ValueArena& = delete;
    ~ValueArena();

    /// The memory resource behind the arena, for scratch containers that live as long as it does.
    [[nodiscard]] auto Resource() noexcept -> std::pmr::memory_resource& { return resource_; }

    /// Copies `text` into the arena.
    [[nodiscard]] auto CopyText(std::string_view text) -> std::string_view;

    /// A string value whose characters are copied into the arena.
    [[nodiscard]] auto MakeString(const std::string_view text) -> Value { return Value::FromString(CopyText(text)); }

    /// An entity value whose uid is copied into the arena.
    [[nodiscard]] auto MakeEntity(const EntityUid& uid) -> Value;

    /// A set of `elements`, sorted and deduplicated. The element views are copied as they are, not
    /// deep-copied: what they point to must outlive the set.
    [[nodiscard]] auto MakeSet(std::span<const Value> elements) -> Value;

    /// A record of `entries`, sorted by key. Keys and values are copied as views, not deep-copied.
    /// Fails, naming the key, when a key appears twice.
    [[nodiscard]] auto MakeRecord(std::span<const RecordEntry> entries) -> std::expected<Value, DuplicateRecordKey>;

    /// Storage in the arena for `count` set elements, for a reader to fill and pass to AdoptSet.
    /// The elements start as `false`.
    [[nodiscard]] auto AllocateValues(std::size_t count) -> std::span<Value>;

    /// Storage in the arena for `count` record entries, for a reader to fill and pass to
    /// AdoptRecord. The entries start with an empty key and `false`.
    [[nodiscard]] auto AllocateRecordEntries(std::size_t count) -> std::span<RecordEntry>;

    /// MakeSet without the copy: sorts and deduplicates `elements` in place and returns a set over
    /// (a prefix of) them. `elements` must be storage from AllocateValues on this arena.
    [[nodiscard]] auto AdoptSet(std::span<Value> elements) -> Value;

    /// MakeRecord without the copy: sorts `entries` by key in place and returns a record over them.
    /// Fails, naming the key, when a key appears twice. `entries` must be storage from
    /// AllocateRecordEntries on this arena.
    [[nodiscard]] auto AdoptRecord(std::span<RecordEntry> entries) -> std::expected<Value, DuplicateRecordKey>;

    /// Copies `value` and everything it points to into the arena.
    [[nodiscard]] auto DeepCopy(const Value& value) -> Value;

private:
    struct StoredEntityUid;

    std::pmr::monotonic_buffer_resource resource_;
    StoredEntityUid* storedEntityUids_ = nullptr;
};

/// A Value that owns its storage, for values that leave the library (evaluation results, attribute
/// getters). Copies share the storage. The view from Get() is valid while any copy is alive.
class OwnedValue final
{
public:
    /// Deep-copies `value` into new storage. Scalars and empty composites need none.
    [[nodiscard]] static auto Copy(const Value& value) -> OwnedValue;

    /// Takes a value already built in `arena`. Every part of `value` must live in `arena` (or be
    /// inline in the Value).
    OwnedValue(std::shared_ptr<const ValueArena> arena, const Value value) noexcept
        : value_{value}, arena_{std::move(arena)}
    {
    }

    /// The value. The view is valid while this OwnedValue or a copy lives, so it is not available
    /// from a temporary.
    [[nodiscard]] auto Get() const& noexcept -> const Value& { return value_; }
    auto Get() const&& noexcept -> const Value& = delete;

    friend auto operator==(const OwnedValue& left, const OwnedValue& right) noexcept -> bool
    {
        return left.value_ == right.value_;
    }
    friend auto operator<=>(const OwnedValue& left, const OwnedValue& right) noexcept -> std::strong_ordering
    {
        return left.value_ <=> right.value_;
    }

private:
    Value value_;
    std::shared_ptr<const ValueArena> arena_;
};

/// Appends `value` in Cedar syntax, matching Rust's `Display for Value` in `cedar-policy-core`:
/// sets as `[a, b]` in set order, records as `{key: value}` in key order with a key quoted when it is
/// not an identifier, is a reserved keyword or is `__cedar`, strings and entity ids escaped as
/// `str::escape_debug` does, extension values in their canonical call form (AppendCanonicalCall).
/// Unlike Rust, a quoted record key is escaped too, so the output always parses.
void AppendCedar(std::string& output, const Value& value);

/// `value` in Cedar syntax (AppendCedar).
[[nodiscard]] auto ToCedar(const Value& value) -> std::string;

} // namespace cedar
