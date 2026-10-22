#!/usr/bin/env bash
# Builds the partial-evaluation oracle (tests/parity/partial-evaluation-oracle) against a read-only
# reference checkout and installs it as <output>/bin/cedar-partial-evaluation-oracle.
#
# The crate is copied to <output>/crate with its Cargo.toml dependency paths written from
# --cedar-source-directory, and the checkout's Cargo.lock is copied beside it, so every crate the
# checkout also uses resolves to the version the checkout pins. Cargo's target directory lives under
# the output directory, so neither the checkout nor this repository is written.
set -euo pipefail

usage() {
  echo "usage: $0 --cedar-source-directory <dir> --output-directory <dir>" >&2
  exit 2
}

source_directory=""
output_directory=""
while [ $# -gt 0 ]; do
  case "$1" in
    --cedar-source-directory) [ $# -ge 2 ] || usage; source_directory="$2"; shift 2 ;;
    --output-directory) [ $# -ge 2 ] || usage; output_directory="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$source_directory" ] && [ -n "$output_directory" ] || usage
[ -d "$source_directory/cedar-policy" ] || { echo "not a cedar checkout: $source_directory" >&2; exit 2; }
[ -f "$source_directory/Cargo.lock" ] || { echo "no Cargo.lock in $source_directory" >&2; exit 2; }

if command -v cargo >/dev/null 2>&1; then
  cargo_command="cargo"
elif [ -x "$HOME/.cargo/bin/cargo" ]; then
  cargo_command="$HOME/.cargo/bin/cargo"
else
  echo "cargo not found on PATH or at ~/.cargo/bin/cargo" >&2
  exit 1
fi

crate_source="$(cd "$(dirname "$0")/../tests/parity/partial-evaluation-oracle" && pwd)"
absolute_source="$(cd "$source_directory" && pwd)"
mkdir -p "$output_directory/bin" "$output_directory/crate/src"
absolute_output="$(cd "$output_directory" && pwd)"
crate_directory="$absolute_output/crate"

sed "s|@CedarSourceDirectory@|$absolute_source|g" "$crate_source/Cargo.toml" > "$crate_directory/Cargo.toml"
cp "$crate_source/src/main.rs" "$crate_directory/src/main.rs"
cp "$absolute_source/Cargo.lock" "$crate_directory/Cargo.lock"

CARGO_TARGET_DIR="$absolute_output/cargo-target" "$cargo_command" build --release \
  --manifest-path "$crate_directory/Cargo.toml"
cp "$absolute_output/cargo-target/release/cedar-partial-evaluation-oracle" \
  "$absolute_output/bin/cedar-partial-evaluation-oracle"
echo "installed $absolute_output/bin/cedar-partial-evaluation-oracle"
