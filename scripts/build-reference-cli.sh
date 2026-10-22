#!/usr/bin/env bash
# Builds the Rust Cedar command line from a read-only reference checkout and
# installs it as <output>/bin/cedar: default features only, plus the cargo
# features `--features` lists (comma-separated, for example `permissive-validate`
# for the oracle of permissive validation). Cargo's target directory lives under
# the output directory so the checkout is never written.
set -euo pipefail

usage() {
  echo "usage: $0 --cedar-source-directory <dir> --output-directory <dir> [--features <list>]" >&2
  exit 2
}

source_directory=""
output_directory=""
features=""
while [ $# -gt 0 ]; do
  case "$1" in
    --cedar-source-directory) [ $# -ge 2 ] || usage; source_directory="$2"; shift 2 ;;
    --output-directory) [ $# -ge 2 ] || usage; output_directory="$2"; shift 2 ;;
    --features) [ $# -ge 2 ] || usage; features="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$source_directory" ] && [ -n "$output_directory" ] || usage
[ -d "$source_directory" ] || { echo "not a directory: $source_directory" >&2; exit 2; }

if command -v cargo >/dev/null 2>&1; then
  cargo_command="cargo"
elif [ -x "$HOME/.cargo/bin/cargo" ]; then
  cargo_command="$HOME/.cargo/bin/cargo"
else
  echo "cargo not found on PATH or at ~/.cargo/bin/cargo" >&2
  exit 1
fi

mkdir -p "$output_directory/bin"
target_directory="$(cd "$output_directory" && pwd)/cargo-target"

cd "$source_directory"
feature_arguments=()
[ -n "$features" ] && feature_arguments=(--features "$features")
CARGO_TARGET_DIR="$target_directory" "$cargo_command" build --release -p cedar-policy-cli "${feature_arguments[@]}"
cp "$target_directory/release/cedar" "$output_directory/bin/cedar"
echo "installed $output_directory/bin/cedar"
