#include <cedar/Value.hpp>

#include "model/EscapeDebug.hpp"

#include <algorithm>
#include <cassert>
#include <memory>
#include <new>

namespace cedar
{

namespace
{

auto CompareEntries(const RecordEntry& left, const RecordEntry& right) noexcept -> std::strong_ordering
{
    if (const auto byKey = left.key <=> right.key; byKey != 0) { return byKey; }
    return Value::Compare(left.value, right.value);
}

// A record key prints bare when the parser would read it back as a field name: an identifier that
// is not a reserved keyword and not `__cedar` (Rust's `UnreservedId`).
auto IsBareRecordKey(const std::string_view key) noexcept -> bool
{
    return CheckIdentifier(key).has_value() && key != ReservedNamespaceName;
}

void AppendQuoted(std::string& output, const std::string_view text)
{
    output.push_back('"');
    detail::AppendEscapeDebug(output, text);
    output.push_back('"');
}

} // namespace

// ---- Set ----

auto Set::FromSortedUnique(const std::span<const Value> elements) noexcept -> Set
{
    assert(std::ranges::adjacent_find(elements, [](const Value& left, const Value& right) {
               return Value::Compare(left, right) >= 0;
           }) == elements.end());
    return Set{elements.data(), elements.size()};
}

auto Set::Contains(const Value& element) const noexcept -> bool
{
    const Value* found = std::lower_bound(begin(), end(), element, [](const Value& left, const Value& right) {
        return Value::Compare(left, right) < 0;
    });
    return found != end() && Value::Equal(*found, element);
}

auto Set::ContainsAll(const Set other) const noexcept -> bool
{
    if (other.size_ > size_) { return false; }
    const Value* mine = begin();
    for (const Value& wanted : other)
    {
        while (true)
        {
            if (mine == end()) { return false; }
            const auto order = Value::Compare(*mine, wanted);
            if (order > 0) { return false; }
            ++mine;
            if (order == 0) { break; }
        }
    }
    return true;
}

auto Set::ContainsAny(const Set other) const noexcept -> bool
{
    const Value* mine = begin();
    const Value* theirs = other.begin();
    while (mine != end() && theirs != other.end())
    {
        const auto order = Value::Compare(*mine, *theirs);
        if (order == 0) { return true; }
        if (order < 0) { ++mine; }
        else { ++theirs; }
    }
    return false;
}

// ---- Record ----

auto Record::FromSortedUnique(const std::span<const RecordEntry> entries) noexcept -> Record
{
    assert(std::ranges::adjacent_find(entries, [](const RecordEntry& left, const RecordEntry& right) {
               return left.key >= right.key;
           }) == entries.end());
    return Record{entries.data(), entries.size()};
}

auto Record::Find(const std::string_view key) const noexcept -> const Value*
{
    // Keys are unique, so a small record is scanned, comparing lengths before bytes; a larger one
    // is searched in its key order.
    constexpr std::size_t LinearScanLimit = 8;
    if (static_cast<std::size_t>(end() - begin()) <= LinearScanLimit)
    {
        for (const RecordEntry& entry : *this)
        {
            if (entry.key.size() == key.size() && entry.key == key) { return &entry.value; }
        }
        return nullptr;
    }
    const RecordEntry* found = std::lower_bound(
        begin(), end(), key, [](const RecordEntry& entry, const std::string_view wanted) { return entry.key < wanted; });
    return found != end() && found->key == key ? &found->value : nullptr;
}

// ---- Value ----

auto Value::Compare(const Value& left, const Value& right) noexcept -> std::strong_ordering
{
    if (left.kind_ != right.kind_) { return left.kind_ <=> right.kind_; }
    switch (left.kind_)
    {
    case ValueKind::Bool: return left.payload_.boolean <=> right.payload_.boolean;
    case ValueKind::Long: return left.payload_.integer <=> right.payload_.integer;
    case ValueKind::String: return left.AsString() <=> right.AsString();
    case ValueKind::Entity:
        if (left.payload_.entity == right.payload_.entity) { return std::strong_ordering::equal; }
        return left.AsEntity() <=> right.AsEntity();
    case ValueKind::Set:
    {
        const Set leftSet = left.AsSet();
        const Set rightSet = right.AsSet();
        return std::lexicographical_compare_three_way(leftSet.begin(), leftSet.end(), rightSet.begin(),
                                                      rightSet.end(), &Value::Compare);
    }
    case ValueKind::Record:
    {
        const Record leftRecord = left.AsRecord();
        const Record rightRecord = right.AsRecord();
        return std::lexicographical_compare_three_way(leftRecord.begin(), leftRecord.end(), rightRecord.begin(),
                                                      rightRecord.end(), &CompareEntries);
    }
    case ValueKind::Datetime:
    case ValueKind::Decimal:
    case ValueKind::Duration: return left.payload_.scalar <=> right.payload_.scalar;
    case ValueKind::IpAddress: return left.AsIpAddress() <=> right.AsIpAddress();
    }
    return std::strong_ordering::equal;
}

auto Value::Equal(const Value& left, const Value& right) noexcept -> bool
{
    if (left.kind_ != right.kind_) { return false; }
    switch (left.kind_)
    {
    case ValueKind::Bool: return left.payload_.boolean == right.payload_.boolean;
    case ValueKind::Long: return left.payload_.integer == right.payload_.integer;
    case ValueKind::String: return left.AsString() == right.AsString();
    case ValueKind::Entity:
        return left.payload_.entity == right.payload_.entity || left.AsEntity() == right.AsEntity();
    case ValueKind::Set:
    {
        const Set leftSet = left.AsSet();
        const Set rightSet = right.AsSet();
        return std::ranges::equal(leftSet, rightSet, &Value::Equal);
    }
    case ValueKind::Record:
    {
        const Record leftRecord = left.AsRecord();
        const Record rightRecord = right.AsRecord();
        return std::ranges::equal(leftRecord, rightRecord, [](const RecordEntry& a, const RecordEntry& b) {
            return a.key == b.key && Value::Equal(a.value, b.value);
        });
    }
    case ValueKind::Datetime:
    case ValueKind::Decimal:
    case ValueKind::Duration: return left.payload_.scalar == right.payload_.scalar;
    case ValueKind::IpAddress: return left.AsIpAddress() == right.AsIpAddress();
    }
    return false;
}

// ---- ValueArena ----

struct ValueArena::StoredEntityUid
{
    EntityUid uid;
    StoredEntityUid* next;
};

ValueArena::ValueArena() noexcept : resource_{std::pmr::new_delete_resource()} {}

ValueArena::ValueArena(const std::span<std::byte> initialBuffer) noexcept
    : resource_{initialBuffer.data(), initialBuffer.size(), std::pmr::new_delete_resource()}
{
}

ValueArena::~ValueArena()
{
    // The monotonic resource releases memory without running destructors; the uids own heap
    // strings, so they are destroyed here first.
    for (StoredEntityUid* stored = storedEntityUids_; stored != nullptr;)
    {
        StoredEntityUid* const next = stored->next;
        std::destroy_at(stored);
        stored = next;
    }
}

auto ValueArena::CopyText(const std::string_view text) -> std::string_view
{
    if (text.empty()) { return {}; }
    auto* const characters = static_cast<char*>(resource_.allocate(text.size(), alignof(char)));
    std::ranges::copy(text, characters);
    return {characters, text.size()};
}

auto ValueArena::MakeEntity(const EntityUid& uid) -> Value
{
    void* const memory = resource_.allocate(sizeof(StoredEntityUid), alignof(StoredEntityUid));
    auto* const stored = ::new (memory) StoredEntityUid{uid, storedEntityUids_};
    storedEntityUids_ = stored;
    return Value::FromEntity(stored->uid);
}

auto ValueArena::MakeSet(const std::span<const Value> elements) -> Value
{
    if (elements.empty()) { return Value::FromSet(Set{}); }
    auto* const data = static_cast<Value*>(resource_.allocate(elements.size_bytes(), alignof(Value)));
    std::uninitialized_copy(elements.begin(), elements.end(), data);
    return AdoptSet({data, elements.size()});
}

auto ValueArena::MakeRecord(const std::span<const RecordEntry> entries) -> std::expected<Value, DuplicateRecordKey>
{
    if (entries.empty()) { return Value::FromRecord(Record{}); }
    auto* const data = static_cast<RecordEntry*>(resource_.allocate(entries.size_bytes(), alignof(RecordEntry)));
    std::uninitialized_copy(entries.begin(), entries.end(), data);
    return AdoptRecord({data, entries.size()});
}

auto ValueArena::AllocateValues(const std::size_t count) -> std::span<Value>
{
    if (count == 0) { return {}; }
    auto* const data = static_cast<Value*>(resource_.allocate(count * sizeof(Value), alignof(Value)));
    std::uninitialized_fill_n(data, count, Value::FromBool(false));
    return {data, count};
}

auto ValueArena::AllocateRecordEntries(const std::size_t count) -> std::span<RecordEntry>
{
    if (count == 0) { return {}; }
    auto* const data = static_cast<RecordEntry*>(resource_.allocate(count * sizeof(RecordEntry), alignof(RecordEntry)));
    std::uninitialized_fill_n(data, count, RecordEntry{{}, Value::FromBool(false)});
    return {data, count};
}

auto ValueArena::AdoptSet(const std::span<Value> elements) -> Value
{
    if (elements.empty()) { return Value::FromSet(Set{}); }
    std::sort(elements.begin(), elements.end(),
              [](const Value& left, const Value& right) { return Value::Compare(left, right) < 0; });
    const auto last = std::unique(elements.begin(), elements.end(), &Value::Equal);
    return Value::FromSet(Set::FromSortedUnique(elements.first(static_cast<std::size_t>(last - elements.begin()))));
}

auto ValueArena::AdoptRecord(const std::span<RecordEntry> entries) -> std::expected<Value, DuplicateRecordKey>
{
    if (entries.empty()) { return Value::FromRecord(Record{}); }
    std::sort(entries.begin(), entries.end(),
              [](const RecordEntry& left, const RecordEntry& right) { return left.key < right.key; });
    const auto duplicate = std::adjacent_find(entries.begin(), entries.end(), [](const RecordEntry& left, const RecordEntry& right) {
        return left.key == right.key;
    });
    if (duplicate != entries.end()) { return std::unexpected{DuplicateRecordKey{duplicate->key}}; }
    return Value::FromRecord(Record::FromSortedUnique(entries));
}

auto ValueArena::DeepCopy(const Value& value) -> Value
{
    switch (value.Kind())
    {
    case ValueKind::String: return MakeString(value.AsString());
    case ValueKind::Entity: return MakeEntity(value.AsEntity());
    case ValueKind::Set:
    {
        const Set set = value.AsSet();
        if (set.IsEmpty()) { return Value::FromSet(Set{}); }
        auto* const data = static_cast<Value*>(resource_.allocate(set.Size() * sizeof(Value), alignof(Value)));
        for (std::size_t index = 0; index < set.Size(); ++index)
        {
            std::construct_at(data + index, DeepCopy(set.Elements()[index]));
        }
        return Value::FromSet(Set::FromSortedUnique({data, set.Size()}));
    }
    case ValueKind::Record:
    {
        const Record record = value.AsRecord();
        if (record.IsEmpty()) { return Value::FromRecord(Record{}); }
        auto* const data =
            static_cast<RecordEntry*>(resource_.allocate(record.Size() * sizeof(RecordEntry), alignof(RecordEntry)));
        for (std::size_t index = 0; index < record.Size(); ++index)
        {
            const RecordEntry& entry = record.Entries()[index];
            std::construct_at(data + index, RecordEntry{CopyText(entry.key), DeepCopy(entry.value)});
        }
        return Value::FromRecord(Record::FromSortedUnique({data, record.Size()}));
    }
    case ValueKind::Bool:
    case ValueKind::Long:
    case ValueKind::Datetime:
    case ValueKind::Decimal:
    case ValueKind::Duration:
    case ValueKind::IpAddress: return value;
    }
    return value;
}

// ---- OwnedValue ----

auto OwnedValue::Copy(const Value& value) -> OwnedValue
{
    const bool needsStorage = [&] {
        switch (value.Kind())
        {
        case ValueKind::String: return !value.AsString().empty();
        case ValueKind::Entity: return true;
        case ValueKind::Set: return !value.AsSet().IsEmpty();
        case ValueKind::Record: return !value.AsRecord().IsEmpty();
        default: return false;
        }
    }();
    if (!needsStorage)
    {
        // An empty string or composite is rebuilt so that it points nowhere.
        ValueArena empty;
        return OwnedValue{nullptr, empty.DeepCopy(value)};
    }
    auto arena = std::make_shared<ValueArena>();
    const Value copy = arena->DeepCopy(value);
    return OwnedValue{std::move(arena), copy};
}

// ---- Printing ----

void AppendCedar(std::string& output, const Value& value)
{
    switch (value.Kind())
    {
    case ValueKind::Bool: output.append(value.AsBool() ? "true" : "false"); return;
    case ValueKind::Long: detail::AppendSignedDecimal(output, value.AsLong()); return;
    case ValueKind::String: AppendQuoted(output, value.AsString()); return;
    case ValueKind::Entity: output.append(value.AsEntity().ToString()); return;
    case ValueKind::Set:
    {
        output.push_back('[');
        bool first = true;
        for (const Value& element : value.AsSet())
        {
            if (!first) { output.append(", "); }
            first = false;
            AppendCedar(output, element);
        }
        output.push_back(']');
        return;
    }
    case ValueKind::Record:
    {
        output.push_back('{');
        bool first = true;
        for (const RecordEntry& entry : value.AsRecord())
        {
            if (!first) { output.append(", "); }
            first = false;
            if (IsBareRecordKey(entry.key)) { output.append(entry.key); }
            else { AppendQuoted(output, entry.key); }
            output.append(": ");
            AppendCedar(output, entry.value);
        }
        output.push_back('}');
        return;
    }
    case ValueKind::Datetime: AppendCanonicalCall(output, value.AsDatetime()); return;
    case ValueKind::Decimal: AppendCanonicalCall(output, value.AsDecimal()); return;
    case ValueKind::Duration: AppendCanonicalCall(output, value.AsDuration()); return;
    case ValueKind::IpAddress: AppendCanonicalCall(output, value.AsIpAddress()); return;
    }
}

auto ToCedar(const Value& value) -> std::string
{
    std::string output;
    AppendCedar(output, value);
    return output;
}

} // namespace cedar
