#!/usr/bin/env bash
# Asks the reference Cedar CLI about every JSON policy file in a case directory and writes, next to
# each `<name>.cedar.json` or `<name>.linked.json`, the `<name>.expected` that
# tests/unit/PolicyJsonTests.cpp checks.
#
# `<name>.expected` is `reject` alone when `cedar check-parse --policy-format json --policies`
# refuses the file. Otherwise it is `accept`, and for a `.cedar.json` file also a line feed and
# Rust's Cedar text of the policy set (`translate-policy --direction json-to-cedar`). A
# `.linked.json` file has template links, which Rust prints as `Template Instance of ...` rather
# than as Cedar text, so its expectation is `accept` alone.
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

count=0
for case_file in "$directory"/*.cedar.json "$directory"/*.linked.json; do
  [ -e "$case_file" ] || continue
  case "$case_file" in
    *.cedar.json) expected="${case_file%.cedar.json}.expected" ;;
    *) expected="${case_file%.linked.json}.expected" ;;
  esac
  if "$oracle" check-parse --policy-format json --policies "$case_file" >/dev/null 2>&1; then
    {
      printf 'accept\n'
      case "$case_file" in
        *.cedar.json) "$oracle" translate-policy --direction json-to-cedar -p "$case_file" ;;
      esac
    } > "$expected"
  else
    printf 'reject\n' > "$expected"
  fi
  count=$((count + 1))
done
echo "wrote $count expectations in $directory"
