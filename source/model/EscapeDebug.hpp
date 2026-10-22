#pragma once

#include <string>
#include <string_view>

namespace cedar::detail
{

// Appends `text` escaped exactly as Rust's `str::escape_debug` does, which is how Cedar prints
// entity ids and string literals: `\0 \t \r \n \\ \" \'` by name, non-printable code points and a
// leading grapheme extender as `\u{hex}` in lowercase hex, everything else verbatim.
//
// `text` is expected to be UTF-8. A byte that does not start a well-formed sequence is written as
// U+FFFD, which is what Rust's lossy conversion would have produced before printing.
void AppendEscapeDebug(std::string& output, std::string_view text);

// As AppendEscapeDebug, but escaping each code point as Rust's `char::escape_debug` does, which
// writes a grapheme extender as `\u{hex}` at any position. Cedar prints `like` patterns this way,
// one character at a time (`cedar-policy-core/src/ast/pattern.rs:82`).
void AppendCharacterEscapeDebug(std::string& output, std::string_view text);

} // namespace cedar::detail
