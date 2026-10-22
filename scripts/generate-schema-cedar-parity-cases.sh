#!/usr/bin/env bash
# Asks the reference Cedar CLI about each Cedar-syntax schema in a directory and writes the
# expectations tests/unit/SchemaParserTests.cpp checks.
#
# For every `<name>.cedarschema` in --directory:
#   <name>.expected.json           `translate-schema --direction cedar-to-json` output, when it accepts
#   <name>.expected-resolved.json  `--direction cedar-to-json-with-resolved-types` output, when it accepts
#   <name>.expected-printed.txt    `--direction json-to-cedar` run on the cedar-to-json output, when it
#                                  accepts (the Cedar printer applied to the parsed fragment)
# Each output is stored without the newline the CLI's `println!` adds.
# And Expected.tsv, one line per case sorted by name, five tab-separated columns:
#   parse      accept|reject
#   resolved   accept|reject, or - when parsing rejects
#   printed    accept|collision|shape (the printer's two refusals), or - when parsing rejects
#   warnings   <shadows-builtin count>/<shadows-entity count>, or - when parsing rejects
#   name       the case's file name
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar binary> --directory <directory>" >&2
  exit 2
}

oracle=""
directory=""
while [ $# -gt 0 ]; do
  case "$1" in
    --oracle) [ $# -ge 2 ] || usage; oracle="$2"; shift 2 ;;
    --directory) [ $# -ge 2 ] || usage; directory="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$oracle" ] && [ -n "$directory" ] || usage
[ -x "$oracle" ] || { echo "not executable: $oracle" >&2; exit 1; }
[ -d "$directory" ] || { echo "not a directory: $directory" >&2; exit 1; }

work="$(mktemp -d "${directory}/.generate.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# Runs the oracle; on success writes its stdout without the final newline to $1.
run() {
  local output="$1"
  shift
  if "$oracle" translate-schema "$@" > "$work/stdout" 2> "$work/stderr"; then
    head -c -1 "$work/stdout" > "$output"
    return 0
  fi
  return 1
}

table="$work/Expected.tsv"
: > "$table"
count=0
for input in "$directory"/*.cedarschema; do
  [ -e "$input" ] || continue
  name="$(basename "$input")"
  stem="${input%.cedarschema}"
  rm -f "$stem.expected.json" "$stem.expected-resolved.json" "$stem.expected-printed.txt"
  if ! run "$stem.expected.json" --direction cedar-to-json --schema "$input"; then
    printf 'reject\t-\t-\t-\t%s\n' "$name" >> "$table"
    count=$((count + 1))
    continue
  fi
  builtin="$(grep -c 'shadows a builtin' "$work/stderr" || true)"
  entity="$(grep -c 'shadows an entity' "$work/stderr" || true)"
  resolved=reject
  if run "$stem.expected-resolved.json" --direction cedar-to-json-with-resolved-types --schema "$input"; then
    resolved=accept
  fi
  printed=accept
  if ! run "$stem.expected-printed.txt" --direction json-to-cedar --schema "$stem.expected.json"; then
    if grep -q 'There are name collisions' "$work/stderr"; then printed=collision
    elif grep -q 'cannot be converted to Cedar' "$work/stderr"; then printed=shape
    else
      echo "json-to-cedar refused its own cedar-to-json output for $name" >&2
      exit 1
    fi
  fi
  printf 'accept\t%s\t%s\t%s/%s\t%s\n' "$resolved" "$printed" "$builtin" "$entity" "$name" >> "$table"
  count=$((count + 1))
done
LC_ALL=C sort -t "$(printf '\t')" -k5 "$table" -o "$table"
mv "$table" "$directory/Expected.tsv"
echo "wrote $count cases to $directory/Expected.tsv"
