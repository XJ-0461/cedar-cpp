#pragma once

#include "model/EscapeDebugTables.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cedar::detail
{

// A set of Unicode scripts, one bit per `unicode-script` Script value (Common and Inherited are
// every bit), and the Han with Bopomofo (bit 0), Japanese (bit 1) and Korean (bit 2) writing
// systems of `unicode-security`'s AugmentedScriptSet.
struct ScriptSet
{
    std::array<std::uint64_t, 3> scripts;
    std::uint8_t writingSystems;
};

// An inclusive range of code points that share one effective script extension, an index into
// ScriptSets().
struct ScriptSetRange
{
    std::uint32_t first;
    std::uint32_t last;
    std::uint16_t set;
};

// The tables in UnicodeSecurityTables.cpp, generated from the `unicode-security` and
// `unicode-script` crates the reference CLI links (scripts/generate-unicode-security-tables.py).
// The range tables are sorted and disjoint.
//
// IdentifierAllowedRanges: the code points `GeneralSecurityProfile::identifier_allowed` accepts.
// ScriptSetRanges: each code point's `char::script_extension`; a code point in no range has the
// Unknown script, the empty set.
[[nodiscard]] auto IdentifierAllowedRanges() -> std::span<const CodePointRange>;
[[nodiscard]] auto ScriptSets() -> std::span<const ScriptSet>;
[[nodiscard]] auto ScriptSetRanges() -> std::span<const ScriptSetRange>;

} // namespace cedar::detail
