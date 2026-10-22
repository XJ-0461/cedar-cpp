#!/usr/bin/env bash
# Asks the reference Cedar CLI about every expression in an input list and writes the parity table
# that tests/unit/ExpressionParserTests.cpp runs.
#
# Input: one expression per line; blank lines and lines starting with `#` are skipped. An
# expression may not contain a tab.
# Output: one line per expression, three tab-separated columns:
#   accept|reject   the expression   Rust's canonical text of it (empty when rejected or unknown)
# accept/reject is `cedar check-parse --expression`. The canonical text comes from wrapping the
# expression as `permit(principal, action, resource) when { <expression> };` and translating it to
# JSON and back, which prints Rust's AST form; it is empty for an expression that cannot stand in a
# `when` clause (one with a slot).
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar binary> --input <expression list> --output <table>" >&2
  exit 2
}

oracle=""
input=""
output=""
while [ $# -gt 0 ]; do
  case "$1" in
    --oracle) [ $# -ge 2 ] || usage; oracle="$2"; shift 2 ;;
    --input) [ $# -ge 2 ] || usage; input="$2"; shift 2 ;;
    --output) [ $# -ge 2 ] || usage; output="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$oracle" ] && [ -n "$input" ] && [ -n "$output" ] || usage
[ -x "$oracle" ] || { echo "not executable: $oracle" >&2; exit 1; }

canonical_of() {
  local expression="$1"
  local json
  if ! json="$(printf 'permit(principal, action, resource) when { %s\n};' "$expression" \
      | "$oracle" translate-policy --direction cedar-to-json 2>/dev/null)"; then
    return 0
  fi
  # The policy prints as `permit(\n  principal,\n  action,\n  resource\n) when {\n  <expr>\n};`.
  printf '%s' "$json" | "$oracle" translate-policy --direction json-to-cedar 2>/dev/null \
    | sed -n '/^) when {$/,/^};$/p' | sed '1d;$d' | sed 's/^  //' | tr -d '\n' || true
}

temporary="$(mktemp "${output}.XXXXXX")"
trap 'rm -f "$temporary"' EXIT
count=0
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    ''|'#'*) continue ;;
  esac
  case "$line" in
    *$'\t'*) echo "an expression contains a tab: $line" >&2; exit 1 ;;
  esac
  if "$oracle" -f plain check-parse --expression="$line" >/dev/null 2>&1; then
    printf 'accept\t%s\t%s\n' "$line" "$(canonical_of "$line")" >> "$temporary"
  else
    printf 'reject\t%s\t\n' "$line" >> "$temporary"
  fi
  count=$((count + 1))
done < "$input"
mv "$temporary" "$output"
trap - EXIT
echo "wrote $count expressions to $output"
