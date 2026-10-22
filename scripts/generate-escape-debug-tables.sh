#!/usr/bin/env bash
# Regenerates lib/source/model/EscapeDebugTables.cpp from the Rust toolchain that built the reference
# Cedar CLI, so cedar-cpp escapes entity ids and strings exactly as that CLI does.
set -euo pipefail

usage() {
  echo "usage: $0 --output-file <path> --work-directory <dir>" >&2
  exit 2
}

output_file=""
work_directory=""
while [ $# -gt 0 ]; do
  case "$1" in
    --output-file) [ $# -ge 2 ] || usage; output_file="$2"; shift 2 ;;
    --work-directory) [ $# -ge 2 ] || usage; work_directory="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$output_file" ] && [ -n "$work_directory" ] || usage

if command -v cargo >/dev/null 2>&1; then
  cargo_command="cargo"
elif [ -x "$HOME/.cargo/bin/cargo" ]; then
  cargo_command="$HOME/.cargo/bin/cargo"
else
  echo "cargo not found on PATH or at ~/.cargo/bin/cargo" >&2
  exit 1
fi
rustc_command="$(dirname "$(command -v "$cargo_command")")/rustc"

script_directory="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$work_directory"
ESCAPE_DEBUG_TABLES_RUSTC_VERSION="$("$rustc_command" --version)" \
CARGO_TARGET_DIR="$(cd "$work_directory" && pwd)" \
  "$cargo_command" run --quiet --release --manifest-path "$script_directory/escape-debug-tables/Cargo.toml" \
  > "$output_file"
echo "wrote $output_file"
