#!/usr/bin/env bash
# Fetches exactly one revision of cedar-integration-tests into a scratch
# directory and unpacks corpus-tests.tar.gz inside it. The Rust cedar-testing
# crate expects the corpus unpacked in place, next to the tests/ directory
# (CEDAR_INTEGRATION_TESTS_PATH points at this directory).
set -euo pipefail

usage() {
  echo "usage: $0 --output-directory <dir> --revision <commit>" >&2
  exit 2
}

output_directory=""
revision=""
while [ $# -gt 0 ]; do
  case "$1" in
    --output-directory) [ $# -ge 2 ] || usage; output_directory="$2"; shift 2 ;;
    --revision) [ $# -ge 2 ] || usage; revision="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$output_directory" ] && [ -n "$revision" ] || usage

mkdir -p "$output_directory"
cd "$output_directory"
if [ ! -d .git ]; then
  git init --quiet
  git remote add origin https://github.com/cedar-policy/cedar-integration-tests
fi
git fetch --quiet --depth 1 origin "$revision"
git checkout --quiet FETCH_HEAD
tar xzf corpus-tests.tar.gz
echo "fetched $(git rev-parse HEAD) into $output_directory"
