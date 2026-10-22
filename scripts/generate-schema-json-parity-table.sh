#!/usr/bin/env bash
# Asks the reference Cedar CLI whether it accepts each JSON schema fragment and writes the parity
# table that tests/unit/SchemaJsonTests.cpp runs.
#
# Input: one or more directories searched recursively for `*.json` files, each a whole JSON schema.
# Output: one line per file, sorted by path, two tab-separated columns:
#   accept|reject   the file's path, relative to its input directory when --relative is given,
#                   absolute otherwise
# A fragment is accepted when `cedar translate-schema --direction json-to-cedar` succeeds, or fails
# only in its Cedar-syntax printer (name collisions, an entity shape that is not a record): the
# command reads the fragment with `SchemaFragment::from_json_str`, which does not link, then prints.
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar binary> --input-directory <directory> [--input-directory ...] [--relative] --output <table>" >&2
  exit 2
}

oracle=""
output=""
relative=0
inputs=()
while [ $# -gt 0 ]; do
  case "$1" in
    --oracle) [ $# -ge 2 ] || usage; oracle="$2"; shift 2 ;;
    --input-directory) [ $# -ge 2 ] || usage; inputs+=("$2"); shift 2 ;;
    --relative) relative=1; shift ;;
    --output) [ $# -ge 2 ] || usage; output="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$oracle" ] && [ -n "$output" ] && [ ${#inputs[@]} -gt 0 ] || usage
[ -x "$oracle" ] || { echo "not executable: $oracle" >&2; exit 1; }
if [ $relative -eq 1 ] && [ ${#inputs[@]} -ne 1 ]; then
  echo "--relative needs exactly one --input-directory" >&2
  exit 2
fi

temporary="$(mktemp "${output}.XXXXXX")"
errors="$(mktemp "${output}.errors.XXXXXX")"
trap 'rm -f "$temporary" "$errors"' EXIT

decide() {
  if "$oracle" translate-schema --direction json-to-cedar --schema "$1" > /dev/null 2> "$errors"; then
    echo accept
  elif grep -q -e 'There are name collisions' -e 'have shapes that cannot be converted to Cedar' "$errors"; then
    echo accept
  else
    echo reject
  fi
}

count=0
for directory in "${inputs[@]}"; do
  [ -d "$directory" ] || { echo "not a directory: $directory" >&2; exit 1; }
  absolute="$(cd "$directory" && pwd)"
  while IFS= read -r -d '' file; do
    if [ $relative -eq 1 ]; then name="${file#"$absolute"/}"; else name="$file"; fi
    printf '%s\t%s\n' "$(decide "$file")" "$name" >> "$temporary"
    count=$((count + 1))
  done < <(find "$absolute" -name '*.json' -type f -print0)
done
LC_ALL=C sort -t "$(printf '\t')" -k2 "$temporary" -o "$temporary"
mv "$temporary" "$output"
echo "wrote $count cases to $output"
