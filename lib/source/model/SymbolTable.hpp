#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace cedar::detail
{

// The process-wide, append-only table behind cedar::Symbol. A symbol is a dense 32-bit index into
// it; index 0 is the empty string. Interned text lives until the process exits, so the
// std::string_view a symbol hands out never dangles. Nothing is ever removed: the table's size is
// bounded by the distinct identifiers and names a process sees, entity type names read from data
// included (schema-based readers use Find, so undeclared names do not add to it).
//
// Concurrency. Inserts take one mutex. Every read is lock-free:
//
// - Symbol to text. Entries live in segments that never move: segment k holds 1024 << k entries, so
//   23 segments cover every 32-bit index without a capacity chosen up front. A segment pointer is
//   published with a release store after the segment is allocated; an entry is written before the
//   symbol that names it is published, so any thread that obtained the symbol through a
//   synchronizing channel (the table itself, a mutex, an atomic) sees a complete entry.
// - Text to symbol. An open-addressing index of atomic 64-bit slots, each holding the high half of
//   the text hash and the symbol index plus one (zero marks an empty slot). A reader probes the
//   index it loaded with an acquire load and never blocks. Growth builds a larger index under the
//   mutex, copies every slot, and publishes it with a release store. The old index is retired but
//   never freed, so a reader still probing it reads valid memory, and it still holds every symbol
//   inserted before the swap. Retired indices sum to less than the live one (each is half the size
//   of its successor), so keeping them costs at most one more index's worth of memory for the
//   process. Readers never need hazard pointers or epochs, which is the reason for this choice over
//   a shared_mutex: lookup sits on the parser and JSON hot paths, and a shared_mutex makes every
//   reader write to a shared cache line.
//
// A Find that races an Intern of the same text may miss it; it never returns a wrong symbol. Intern
// re-checks under the mutex before inserting, so one string always gets exactly one symbol.
class SymbolTable final
{
public:
    SymbolTable();
    SymbolTable(const SymbolTable&) = delete;
    SymbolTable(SymbolTable&&) = delete;
    auto operator=(const SymbolTable&) -> SymbolTable& = delete;
    auto operator=(SymbolTable&&) -> SymbolTable& = delete;
    ~SymbolTable() = default;

    // The one instance. It is allocated on first use and never destroyed, so symbols stay valid in
    // static destructors.
    [[nodiscard]] static auto Instance() -> SymbolTable&;

    [[nodiscard]] auto Intern(std::string_view text) -> std::uint32_t;
    [[nodiscard]] auto Find(std::string_view text) const -> std::optional<std::uint32_t>;

    // Precondition: `symbol` was returned by Intern or Find on this table.
    [[nodiscard]] auto Text(std::uint32_t symbol) const noexcept -> std::string_view;
    [[nodiscard]] auto Hash(std::uint32_t symbol) const noexcept -> std::uint64_t;

    [[nodiscard]] auto Size() const noexcept -> std::uint32_t;

private:
    struct Entry
    {
        const char* data = nullptr;
        std::size_t size = 0;
        std::uint64_t hash = 0;
    };

    struct Index
    {
        explicit Index(std::size_t capacity);

        std::size_t mask;
        std::unique_ptr<std::atomic<std::uint64_t>[]> slots;
    };

    static constexpr std::size_t FirstSegmentBits = 10;
    static constexpr std::size_t SegmentCount = 23;
    static constexpr std::size_t InitialIndexCapacity = 1024;
    static constexpr std::size_t CharacterChunkSize = 64 * 1024;
    static constexpr std::size_t DedicatedAllocationThreshold = 4 * 1024;

    [[nodiscard]] auto EntryAt(std::uint32_t symbol) const noexcept -> const Entry&;
    [[nodiscard]] auto Lookup(const Index& index, std::string_view text, std::uint64_t hash) const noexcept
        -> std::optional<std::uint32_t>;
    [[nodiscard]] auto InsertLocked(std::string_view text, std::uint64_t hash) -> std::uint32_t;
    [[nodiscard]] auto CopyCharactersLocked(std::string_view text) -> const char*;
    void PlaceInIndexLocked(Index& index, std::uint64_t slotValue, std::uint64_t hash) noexcept;
    void GrowIndexLocked();

    std::array<std::atomic<Entry*>, SegmentCount> segments_{};
    std::atomic<Index*> index_{nullptr};
    std::atomic<std::uint32_t> size_{0};

    std::mutex insertMutex_;
    std::vector<std::unique_ptr<Entry[]>> ownedSegments_;
    std::vector<std::unique_ptr<Index>> ownedIndices_;
    std::vector<std::unique_ptr<char[]>> characterChunks_;
    char* chunkCursor_ = nullptr;
    std::size_t chunkRemaining_ = 0;
};

} // namespace cedar::detail
