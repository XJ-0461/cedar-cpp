#!/usr/bin/env bash
# Asks the reference Cedar CLI whether it accepts each entities and context JSON case and writes the
# parity table that tests/unit/EntityJsonTests.cpp runs.
#
# Input: a cases directory with `entities/*.json` (each a whole entities file) and
# `contexts/*.json` (each a whole context file).
# Output: one line per case, sorted, three tab-separated columns:
#   accept|reject   entities|context   the case's path relative to the cases directory
# A case is accepted when `cedar authorize` with the policy `permit(principal, action, resource);`
# answers ALLOW: the entities case given as `--entities`, the context case given as `--context`
# (with an empty entities file).
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar binary> --cases-directory <directory> --output <table>" >&2
  exit 2
}

oracle=""
cases=""
output=""
while [ $# -gt 0 ]; do
  case "$1" in
    --oracle) [ $# -ge 2 ] || usage; oracle="$2"; shift 2 ;;
    --cases-directory) [ $# -ge 2 ] || usage; cases="$2"; shift 2 ;;
    --output) [ $# -ge 2 ] || usage; output="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$oracle" ] && [ -n "$cases" ] && [ -n "$output" ] || usage
[ -x "$oracle" ] || { echo "not executable: $oracle" >&2; exit 1; }
[ -d "$cases/entities" ] && [ -d "$cases/contexts" ] || { echo "missing entities/ or contexts/ in $cases" >&2; exit 1; }

temporary="$(mktemp "${output}.XXXXXX")"
policy="$(mktemp "${output}.policy.XXXXXX")"
empty_entities="$(mktemp "${output}.entities.XXXXXX")"
trap 'rm -f "$temporary" "$policy" "$empty_entities"' EXIT
echo 'permit(principal, action, resource);' > "$policy"
echo '[]' > "$empty_entities"

decide() {
  local answer
  answer="$("$oracle" authorize --policies "$policy" -l 'A::"a"' -a 'Action::"a"' -r 'A::"a"' "$@" 2>/dev/null || true)"
  if [ "$(echo $answer)" = "ALLOW" ]; then echo accept; else echo reject; fi
}

count=0
for file in "$cases"/entities/*.json; do
  printf '%s\tentities\t%s\n' "$(decide --entities "$file")" "entities/$(basename "$file")" >> "$temporary"
  count=$((count + 1))
done
for file in "$cases"/contexts/*.json; do
  printf '%s\tcontext\t%s\n' "$(decide --entities "$empty_entities" --context "$file")" \
    "contexts/$(basename "$file")" >> "$temporary"
  count=$((count + 1))
done
LC_ALL=C sort -k3 "$temporary" -o "$temporary"
mv "$temporary" "$output"
echo "wrote $count cases to $output"
