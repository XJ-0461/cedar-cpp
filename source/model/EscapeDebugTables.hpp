#pragma once

#include <cstdint>
#include <span>

namespace cedar::detail
{

// An inclusive range of Unicode scalar values.
struct CodePointRange
{
    std::uint32_t first;
    std::uint32_t last;
};

// The tables in EscapeDebugTables.cpp, generated from the Rust toolchain that built the reference
// CLI (scripts/generate-escape-debug-tables.sh). Both are sorted and disjoint.
//
// Always: code points Rust's `str::escape_debug` writes as `\u{...}` at any position.
// Leading: grapheme extenders, which it writes as `\u{...}` only as the first character.
[[nodiscard]] auto EscapeDebugAlwaysEscapedRanges() -> std::span<const CodePointRange>;
[[nodiscard]] auto EscapeDebugLeadingEscapedRanges() -> std::span<const CodePointRange>;

} // namespace cedar::detail
