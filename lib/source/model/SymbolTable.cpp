#include "model/SymbolTable.hpp"

#include <bit>
#include <cassert>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>

namespace cedar::detail
{

namespace
{

constexpr std::uint64_t TagMask = 0xFFFF'FFFF'0000'0000ULL;
constexpr std::uint64_t SymbolMask = 0x0000'0000'FFFF'FFFFULL;

auto HashText(const std::string_view text) noexcept -> std::uint64_t
{
    return static_cast<std::uint64_t>(std::hash<std::string_view>{}(text));
}

// Segment k holds the indices [1024 * (2^k - 1), 1024 * (2^(k+1) - 1)).
struct SegmentPosition
{
    std::size_t segment;
    std::size_t offset;
};

constexpr auto PositionOf(const std::uint32_t symbol, const std::size_t firstSegmentBits) noexcept -> SegmentPosition
{
    const std::uint64_t biased = std::uint64_t{symbol} + (std::uint64_t{1} << firstSegmentBits);
    const auto segment = static_cast<std::size_t>(std::bit_width(biased)) - firstSegmentBits - 1;
    const auto offset = static_cast<std::size_t>(biased - (std::uint64_t{1} << (segment + firstSegmentBits)));
    return {.segment = segment, .offset = offset};
}

static_assert(PositionOf(0, 10).segment == 0 && PositionOf(0, 10).offset == 0);
static_assert(PositionOf(1023, 10).segment == 0 && PositionOf(1023, 10).offset == 1023);
static_assert(PositionOf(1024, 10).segment == 1 && PositionOf(1024, 10).offset == 0);
static_assert(PositionOf(std::numeric_limits<std::uint32_t>::max(), 10).segment == 22);

} // namespace

SymbolTable::Index::Index(const std::size_t capacity)
    : mask(capacity - 1), slots(std::make_unique<std::atomic<std::uint64_t>[]>(capacity))
{
}

SymbolTable::SymbolTable()
{
    auto index = std::make_unique<Index>(InitialIndexCapacity);
    index_.store(index.get(), std::memory_order_release);
    ownedIndices_.push_back(std::move(index));
    const std::scoped_lock lock{insertMutex_};
    [[maybe_unused]] const auto empty = InsertLocked({}, HashText({}));
    assert(empty == 0);
}

auto SymbolTable::Instance() -> SymbolTable&
{
    static SymbolTable* const instance = new SymbolTable{};
    return *instance;
}

auto SymbolTable::EntryAt(const std::uint32_t symbol) const noexcept -> const Entry&
{
    assert(symbol < size_.load(std::memory_order_acquire));
    const auto [segment, offset] = PositionOf(symbol, FirstSegmentBits);
    return segments_[segment].load(std::memory_order_acquire)[offset];
}

auto SymbolTable::Text(const std::uint32_t symbol) const noexcept -> std::string_view
{
    const Entry& entry = EntryAt(symbol);
    return {entry.data, entry.size};
}

auto SymbolTable::Hash(const std::uint32_t symbol) const noexcept -> std::uint64_t
{
    return EntryAt(symbol).hash;
}

auto SymbolTable::Size() const noexcept -> std::uint32_t
{
    return size_.load(std::memory_order_acquire);
}

auto SymbolTable::Lookup(const Index& index, const std::string_view text, const std::uint64_t hash) const noexcept
    -> std::optional<std::uint32_t>
{
    const std::uint64_t tag = hash & TagMask;
    for (std::size_t position = hash & index.mask;; position = (position + 1) & index.mask)
    {
        const std::uint64_t slot = index.slots[position].load(std::memory_order_acquire);
        if (slot == 0) { return std::nullopt; }
        if ((slot & TagMask) != tag) { continue; }
        const auto symbol = static_cast<std::uint32_t>((slot & SymbolMask) - 1);
        const Entry& entry = EntryAt(symbol);
        if (entry.hash == hash && std::string_view{entry.data, entry.size} == text) { return symbol; }
    }
}

auto SymbolTable::Find(const std::string_view text) const -> std::optional<std::uint32_t>
{
    return Lookup(*index_.load(std::memory_order_acquire), text, HashText(text));
}

auto SymbolTable::Intern(const std::string_view text) -> std::uint32_t
{
    const std::uint64_t hash = HashText(text);
    if (const auto found = Lookup(*index_.load(std::memory_order_acquire), text, hash)) { return *found; }

    const std::scoped_lock lock{insertMutex_};
    if (const auto found = Lookup(*index_.load(std::memory_order_relaxed), text, hash)) { return *found; }
    return InsertLocked(text, hash);
}

auto SymbolTable::CopyCharactersLocked(const std::string_view text) -> const char*
{
    if (text.empty()) { return ""; }
    if (text.size() >= DedicatedAllocationThreshold)
    {
        auto storage = std::make_unique_for_overwrite<char[]>(text.size());
        std::memcpy(storage.get(), text.data(), text.size());
        characterChunks_.push_back(std::move(storage));
        return characterChunks_.back().get();
    }
    if (chunkRemaining_ < text.size())
    {
        characterChunks_.push_back(std::make_unique_for_overwrite<char[]>(CharacterChunkSize));
        chunkCursor_ = characterChunks_.back().get();
        chunkRemaining_ = CharacterChunkSize;
    }
    char* destination = chunkCursor_;
    std::memcpy(destination, text.data(), text.size());
    chunkCursor_ += text.size();
    chunkRemaining_ -= text.size();
    return destination;
}

void SymbolTable::PlaceInIndexLocked(Index& index, const std::uint64_t slotValue, const std::uint64_t hash) noexcept
{
    std::size_t position = hash & index.mask;
    while (index.slots[position].load(std::memory_order_relaxed) != 0) { position = (position + 1) & index.mask; }
    index.slots[position].store(slotValue, std::memory_order_release);
}

void SymbolTable::GrowIndexLocked()
{
    const Index& current = *index_.load(std::memory_order_relaxed);
    const std::size_t capacity = current.mask + 1;
    auto grown = std::make_unique<Index>(capacity * 2);
    for (std::size_t position = 0; position < capacity; ++position)
    {
        const std::uint64_t slot = current.slots[position].load(std::memory_order_relaxed);
        if (slot == 0) { continue; }
        const auto symbol = static_cast<std::uint32_t>((slot & SymbolMask) - 1);
        PlaceInIndexLocked(*grown, slot, EntryAt(symbol).hash);
    }
    index_.store(grown.get(), std::memory_order_release);
    ownedIndices_.push_back(std::move(grown));
}

auto SymbolTable::InsertLocked(const std::string_view text, const std::uint64_t hash) -> std::uint32_t
{
    const std::uint32_t symbol = size_.load(std::memory_order_relaxed);
    // The slot encoding stores symbol + 1 in 32 bits. Reaching this needs four billion distinct
    // identifiers, which exhausts memory long before; terminate rather than corrupt the table.
    if (symbol == std::numeric_limits<std::uint32_t>::max()) { std::terminate(); }

    const auto [segment, offset] = PositionOf(symbol, FirstSegmentBits);
    Entry* entries = segments_[segment].load(std::memory_order_relaxed);
    if (entries == nullptr)
    {
        ownedSegments_.push_back(std::make_unique<Entry[]>(std::size_t{1} << (segment + FirstSegmentBits)));
        entries = ownedSegments_.back().get();
        segments_[segment].store(entries, std::memory_order_release);
    }
    entries[offset] = Entry{.data = CopyCharactersLocked(text), .size = text.size(), .hash = hash};
    size_.store(symbol + 1, std::memory_order_release);

    const Index& current = *index_.load(std::memory_order_relaxed);
    if (std::size_t{symbol + 1} * 2 > current.mask + 1) { GrowIndexLocked(); }
    PlaceInIndexLocked(*index_.load(std::memory_order_relaxed), (hash & TagMask) | (std::uint64_t{symbol} + 1), hash);
    return symbol;
}

} // namespace cedar::detail
