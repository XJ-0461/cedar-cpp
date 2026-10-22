#!/usr/bin/env bash
# Asks a Cedar CLI whether it accepts each entities, context and request case read with a schema,
# and writes the parity table that tests/unit/SchemaJsonConformanceTests.cpp runs.
#
# Input: a cases directory with `Schema.cedarschema`, `entities/*.json` (each a whole entities
# file), `contexts/*.json` (each a context of the action App::Action::"view") and `requests/*.json`
# (each a `--request-json` file).
# Output: one line per case, sorted, three tab-separated columns:
#   accept|reject   entities|context|request   the case's path relative to the cases directory
# An entities case is accepted when `check-parse --schema --entities` succeeds. A context case is
# accepted when `authorize --request-validation false` with the policy `permit(principal, action,
# resource);` answers ALLOW, so only reading the context by its declared type counts; a request
# case when `authorize --request-json` with request validation answers ALLOW.
#
# The CLI is the reference `cedar` to make the table, or cedar-cpp-cli to compare against it.
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
schema="$cases/Schema.cedarschema"
[ -f "$schema" ] || { echo "missing $schema" >&2; exit 1; }
for kind in entities contexts requests; do
  [ -d "$cases/$kind" ] || { echo "missing $kind/ in $cases" >&2; exit 1; }
done

temporary="$(mktemp "${output}.XXXXXX")"
policy="$(mktemp "${output}.policy.XXXXXX")"
empty_entities="$(mktemp "${output}.entities.XXXXXX")"
trap 'rm -f "$temporary" "$policy" "$empty_entities"' EXIT
echo 'permit(principal, action, resource);' > "$policy"
echo '[]' > "$empty_entities"

allowed() {
  local answer
  answer="$("$oracle" authorize --policies "$policy" --entities "$empty_entities" --schema "$schema" "$@" 2>/dev/null || true)"
  if [ "$(echo $answer)" = "ALLOW" ]; then echo accept; else echo reject; fi
}

count=0
for file in "$cases"/entities/*.json; do
  if "$oracle" check-parse --schema "$schema" --entities "$file" >/dev/null 2>&1; then verdict=accept; else verdict=reject; fi
  printf '%s\tentities\t%s\n' "$verdict" "entities/$(basename "$file")" >> "$temporary"
  count=$((count + 1))
done
for file in "$cases"/contexts/*.json; do
  printf '%s\tcontext\t%s\n' "$(allowed --request-validation false -l 'App::User::"alice"' -a 'App::Action::"view"' \
    -r 'App::Photo::"p"' --context "$file")" "contexts/$(basename "$file")" >> "$temporary"
  count=$((count + 1))
done
for file in "$cases"/requests/*.json; do
  printf '%s\trequest\t%s\n' "$(allowed --request-json "$file")" "requests/$(basename "$file")" >> "$temporary"
  count=$((count + 1))
done
LC_ALL=C sort -k3 "$temporary" -o "$temporary"
mv "$temporary" "$output"
echo "wrote $count cases to $output"
