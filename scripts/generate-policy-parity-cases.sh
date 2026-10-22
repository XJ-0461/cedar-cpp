#!/usr/bin/env bash
# Asks the reference Cedar CLI about every policy set in a case directory and writes, next to each
# `<name>.cedar`, the `<name>.expected` that tests/unit/PolicyParserTests.cpp checks.
#
# `<name>.expected` is `reject` alone when `cedar check-parse --policies` refuses the file;
# otherwise `accept`, a line feed, and Rust's canonical text of the policy set. The canonical text
# comes from translating the file to JSON and back (`translate-policy --direction cedar-to-json`,
# then `json-to-cedar`), which prints Rust's AST form: static policies, then templates, each
# sorted by id. The CLI renames each policy to its `@id` annotation, so the cases use no `@id`.
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar binary> --directory <case directory>" >&2
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

json="$(mktemp)"
trap 'rm -f "$json"' EXIT
count=0
for case_file in "$directory"/*.cedar; do
  expected="${case_file%.cedar}.expected"
  if grep -q '@id' "$case_file"; then
    echo "a case uses @id, which the CLI renames policies by: $case_file" >&2
    exit 1
  fi
  if "$oracle" check-parse --policies "$case_file" >/dev/null 2>&1; then
    "$oracle" translate-policy --direction cedar-to-json -p "$case_file" > "$json"
    {
      printf 'accept\n'
      "$oracle" translate-policy --direction json-to-cedar -p "$json"
    } > "$expected"
  else
    printf 'reject\n' > "$expected"
  fi
  count=$((count + 1))
done
echo "wrote $count expectations in $directory"
