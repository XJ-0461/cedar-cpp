#!/usr/bin/env bash
# Writes the expected outputs of the partial-evaluation case families from the Rust oracle.
#
# For every <Family>/cases.jsonl directly beneath --cases-directory, runs
# `<oracle> <Family>/cases.jsonl` and writes its standard output to <Family>/expected.jsonl.
# The oracle writes one line per non-blank case line; a run that exits non-zero, writes to standard
# error, or writes a different number of lines stops the script, and that family's expected.jsonl
# is left as it was.
set -euo pipefail

usage() {
  echo "usage: $0 --oracle <cedar-partial-evaluation-oracle> --cases-directory <dir>" >&2
  exit 2
}

oracle=""
cases_directory=""
while [ $# -gt 0 ]; do
  case "$1" in
    --oracle) [ $# -ge 2 ] || usage; oracle="$2"; shift 2 ;;
    --cases-directory) [ $# -ge 2 ] || usage; cases_directory="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$oracle" ] && [ -n "$cases_directory" ] || usage
[ -x "$oracle" ] || { echo "oracle is not an executable file: $oracle" >&2; exit 2; }
[ -d "$cases_directory" ] || { echo "cases directory does not exist: $cases_directory" >&2; exit 2; }

shopt -s nullglob
manifests=("$cases_directory"/*/cases.jsonl)
if [ ${#manifests[@]} -eq 0 ]; then
  echo "no <Family>/cases.jsonl beneath $cases_directory" >&2
  exit 1
fi

for manifest in "${manifests[@]}"; do
  family_directory="$(dirname "$manifest")"
  family="$(basename "$family_directory")"
  output="$(mktemp "$family_directory/.expected.jsonl.XXXXXX")"
  errors="$(mktemp "$family_directory/.oracle-errors.XXXXXX")"
  status=0
  "$oracle" "$manifest" >"$output" 2>"$errors" || status=$?
  if [ "$status" -ne 0 ] || [ -s "$errors" ]; then
    echo "oracle failed on $family (exit status $status):" >&2
    cat "$errors" >&2
    rm -f "$output" "$errors"
    exit 1
  fi
  rm -f "$errors"
  case_count="$(LC_ALL=C grep -c '[^[:space:]]' "$manifest" || true)"
  line_count="$(wc -l <"$output")"
  if [ "$case_count" -ne "$line_count" ]; then
    echo "oracle wrote $line_count lines for $case_count cases in $family" >&2
    rm -f "$output"
    exit 1
  fi
  mv -f "$output" "$family_directory/expected.jsonl"
  echo "$family: $line_count cases"
done
