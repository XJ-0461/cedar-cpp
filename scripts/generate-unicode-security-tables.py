#!/usr/bin/env python3
"""Writes lib/source/validation/UnicodeSecurityTables.cpp from the Rust crates the validator uses.

The string checks of Cedar's validator (`cedar-policy-core/src/validator/str_checks.rs`) call
`unicode-security` for the General Security Profile (`identifier_allowed`) and for mixed-script
detection, which reads the script extensions of `unicode-script`. This script reads both crates'
own generated tables (`src/tables.rs`), so cedar-cpp uses the Unicode data of exactly the crate
versions in the reference checkout's Cargo.lock, and writes two compact range tables:

- the code points `identifier_allowed` accepts, as merged inclusive ranges;
- each code point's effective script extension (`char::script_extension`: the Script_Extensions
  entry, else the Script value), as ranges into a table of distinct script sets. Code points
  in no range have the Unknown script, the empty set. Each set also carries the Han with
  Bopomofo, Japanese and Korean flags of `AugmentedScriptSet::from(ScriptExtension)`.
"""

import argparse
import os
import re
import sys

CHARACTER = r"'\\u\{([0-9a-f]+)\}'"


def read_tables(source_directory):
    path = os.path.join(source_directory, "src", "tables.rs")
    with open(path, encoding="utf-8") as tables:
        return tables.read()


def crate_version(source_directory):
    with open(os.path.join(source_directory, "Cargo.toml"), encoding="utf-8") as manifest:
        match = re.search(r'^version = "([^"]+)"', manifest.read(), re.MULTILINE)
    if not match:
        raise SystemExit(f"no version in {source_directory}/Cargo.toml")
    return match.group(1)


def unicode_version(text):
    match = re.search(r"pub const UNICODE_VERSION: \(u64, u64, u64\) = \((\d+), (\d+), (\d+)\);", text)
    if not match:
        raise SystemExit("no UNICODE_VERSION")
    return ".".join(match.groups())


def table_body(text, name):
    match = re.search(r"const " + name + r": &'static \[[^\]]*\] = &\[(.*?)\n    \];", text, re.DOTALL)
    if not match:
        raise SystemExit(f"table {name} not found")
    return match.group(1)


def check_sorted(ranges, name):
    for (first, last), (next_first, _) in zip(ranges, ranges[1:]):
        if not first <= last < next_first:
            raise SystemExit(f"{name} is not sorted and disjoint at {first:#x}..{last:#x}")
    for first, last in ranges[-1:]:
        if first > last:
            raise SystemExit(f"{name} has an empty range at {first:#x}")


def merge(ranges):
    merged = []
    for first, last, value in ranges:
        if merged and merged[-1][2] == value and merged[-1][1] + 1 == first:
            merged[-1] = (merged[-1][0], last, value)
        else:
            merged.append((first, last, value))
    return merged


def identifier_allowed_ranges(text):
    body = table_body(text, "IDENTIFIER_STATUS")
    ranges = [(int(first, 16), int(last, 16)) for first, last in re.findall(
        r"\(" + CHARACTER + r",\s*" + CHARACTER + r"\)", body)]
    check_sorted(ranges, "IDENTIFIER_STATUS")
    return [(first, last) for first, last, _ in merge([(first, last, True) for first, last in ranges])]


def script_extensions(text):
    """The script set of every Script value and script_extensions constant, as an integer bitmask
    of NEXT_SCRIPT bits. Common and Inherited are every bit; Unknown is none."""
    next_script = int(re.search(r"pub const NEXT_SCRIPT: u8 = (\d+);", text).group(1))
    every = (1 << next_script) - 1
    enumeration = re.search(r"pub enum Script \{(.*?)\n\}", text, re.DOTALL).group(1)
    scripts = {"Common": every, "Inherited": every, "Unknown": 0}
    for name, value in re.findall(r"^\s*([A-Za-z_]+) = (\d+),", enumeration, re.MULTILINE):
        scripts[name] = 1 << int(value)
    constants = {"COMMON": every, "INHERITED": every, "UNKNOWN": 0}
    module = re.search(r"pub mod script_extensions \{(.*?)\n\}", text, re.DOTALL).group(1)
    for name, expression in re.findall(r"pub const ([A-Z_0-9]+): ScriptExtension = (.*?);", module):
        if name in ("COMMON", "INHERITED", "UNKNOWN"):
            continue
        new = re.fullmatch(r"ScriptExtension::new\((0x[0-9a-f]+|0), (0x[0-9a-f]+|0), (0x[0-9a-f]+|0)\)",
                           expression)
        if new:
            first, second, third = (int(word, 16) for word in new.groups())
            constants[name] = first | (second << 64) | (third << 128)
            continue
        value = 0
        for part in expression.split(".union("):
            part = part.rstrip(")")
            if part not in constants:
                raise SystemExit(f"script extension {name} uses {part} before it is defined")
            value |= constants[part]
        constants[name] = value
    return next_script, every, scripts, constants


def effective_script_ranges(text):
    next_script, every, scripts, constants = script_extensions(text)
    script_ranges = [(int(first, 16), int(last, 16), scripts[name]) for first, last, name in re.findall(
        r"\(" + CHARACTER + r",\s*" + CHARACTER + r",\s*Script::([A-Za-z_]+)\)", table_body(text, "SCRIPTS"))]
    extension_ranges = [(int(first, 16), int(last, 16), constants[name]) for first, last, name in re.findall(
        r"\(" + CHARACTER + r",\s*" + CHARACTER + r",\s*script_extensions::([A-Z_0-9]+)\)",
        table_body(text, "SCRIPT_EXTENSIONS"))]
    check_sorted([(first, last) for first, last, _ in script_ranges], "SCRIPTS")
    check_sorted([(first, last) for first, last, _ in extension_ranges], "SCRIPT_EXTENSIONS")

    # `char::script_extension`: the Script_Extensions range that holds the code point, else its
    # Script range, else Unknown. Walk both sorted tables at once and cut at every boundary.
    boundaries = sorted({point for first, last, _ in script_ranges + extension_ranges for point in (first, last + 1)})

    def lookup(ranges, point):
        low, high = 0, len(ranges)
        while low < high:
            middle = (low + high) // 2
            first, last, value = ranges[middle]
            if point < first:
                high = middle
            elif point > last:
                low = middle + 1
            else:
                return value
        return None

    pieces = []
    for first, following in zip(boundaries, boundaries[1:]):
        value = lookup(extension_ranges, first)
        if value is None:
            value = lookup(script_ranges, first)
        if value is None or value == 0:
            continue
        pieces.append((first, following - 1, value))
    return next_script, every, merge(pieces)


def augmented_flags(value, every, scripts):
    """`AugmentedScriptSet::from(ScriptExtension)`: bit 0 Han with Bopomofo, bit 1 Japanese,
    bit 2 Korean."""
    if value == every or value & scripts["Han"]:
        return 0b111
    flags = 0
    if value & (scripts["Hiragana"] | scripts["Katakana"]):
        flags |= 0b010
    if value & scripts["Hangul"]:
        flags |= 0b100
    if value & scripts["Bopomofo"]:
        flags |= 0b001
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--unicode-security-source", required=True, help="the unicode-security crate source")
    parser.add_argument("--unicode-script-source", required=True, help="the unicode-script crate source")
    parser.add_argument("--output", required=True, help="the C++ file to write")
    arguments = parser.parse_args()

    security_text = read_tables(arguments.unicode_security_source)
    script_text = read_tables(arguments.unicode_script_source)
    security_version = crate_version(arguments.unicode_security_source)
    script_version = crate_version(arguments.unicode_script_source)

    allowed = identifier_allowed_ranges(security_text)
    next_script, every, ranges = effective_script_ranges(script_text)
    _, _, scripts, _ = script_extensions(script_text)
    if next_script > 192:
        raise SystemExit(f"NEXT_SCRIPT {next_script} does not fit three 64-bit words")

    sets = []
    set_index = {}
    for _, _, value in ranges:
        if value not in set_index:
            set_index[value] = len(sets)
            sets.append(value)
    if len(sets) > 0xFFFF:
        raise SystemExit("too many distinct script sets")

    mask = (1 << 64) - 1
    lines = [
        f"// Generated by scripts/generate-unicode-security-tables.py from unicode-security {security_version}"
        f" (Unicode {unicode_version(security_text)})",
        f"// and unicode-script {script_version} (Unicode {unicode_version(script_text)}). Do not edit.",
        '#include "validation/UnicodeSecurityTables.hpp"',
        "",
        "#include <array>",
        "",
        "namespace cedar::detail",
        "{",
        "",
        "namespace",
        "{",
        "",
        f"constexpr std::array<CodePointRange, {len(allowed)}> IdentifierAllowed{{{{",
    ]
    lines += [f"    {{0x{first:x}, 0x{last:x}}}," for first, last in allowed]
    lines += [
        "}};",
        "",
        f"constexpr std::array<ScriptSet, {len(sets)}> Sets{{{{",
    ]
    for value in sets:
        words = ", ".join(f"0x{(value >> (64 * word)) & mask:x}" for word in range(3))
        lines.append(f"    {{{{{words}}}, 0x{augmented_flags(value, every, scripts):x}}},")
    lines += [
        "}};",
        "",
        f"constexpr std::array<ScriptSetRange, {len(ranges)}> Ranges{{{{",
    ]
    lines += [f"    {{0x{first:x}, 0x{last:x}, {set_index[value]}}}," for first, last, value in ranges]
    lines += [
        "}};",
        "",
        "} // namespace",
        "",
        "auto IdentifierAllowedRanges() -> std::span<const CodePointRange> { return IdentifierAllowed; }",
        "",
        "auto ScriptSets() -> std::span<const ScriptSet> { return Sets; }",
        "",
        "auto ScriptSetRanges() -> std::span<const ScriptSetRange> { return Ranges; }",
        "",
        "} // namespace cedar::detail",
    ]
    with open(arguments.output, "w", encoding="utf-8") as output:
        output.write("\n".join(lines) + "\n")
    print(f"wrote {len(allowed)} allowed ranges, {len(sets)} script sets and {len(ranges)} script ranges"
          f" to {arguments.output}")


if __name__ == "__main__":
    sys.exit(main())
