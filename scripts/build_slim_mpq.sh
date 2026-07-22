#!/usr/bin/env bash
# Regenerate slim variants of the SC1 MPQ archives, dropping unused
# audio and cinematic assets.
#
# The original archives are NEVER modified or deleted. Output lands
# alongside them as *.slim.mpq. The server / observer will prefer the
# slim variants when present (see data_files_directory in
# data_loading.h) and fall back to the originals otherwise.
#
# Usage:
#   ./scripts/build_slim_mpq.sh [<input_dir>] [<output_dir>]
# defaults:
#   input_dir  = original_resources
#   output_dir = <input_dir>
#
# The slim_mpq binary must be built first:
#   cmake -S . -B build_srv -DOPENBW_BUILD_TOOLS=ON
#   cmake --build build_srv --target slim_mpq

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IN_DIR="${1:-$REPO_ROOT/original_resources}"
OUT_DIR="${2:-$IN_DIR}"

SLIM_BIN="$REPO_ROOT/build_srv/tools/slim_mpq"
if [[ ! -x "$SLIM_BIN" ]]; then
    echo "slim_mpq binary not found at $SLIM_BIN" >&2
    echo "build it first:" >&2
    echo "  cmake -S . -B build_srv -DOPENBW_BUILD_TOOLS=ON" >&2
    echo "  cmake --build build_srv --target slim_mpq" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

DROPS=(--drop 'sound/' --drop 'music/' --drop 'smk/')

for name in StarDat BrooDat Patch_rt; do
    src="$IN_DIR/${name}.mpq"
    dst="$OUT_DIR/${name}.slim.mpq"
    if [[ ! -f "$src" ]]; then
        echo "warning: $src not found, skipping" >&2
        continue
    fi
    echo "== ${name}.mpq -> ${name}.slim.mpq =="
    "$SLIM_BIN" --in "$src" --out "$dst" "${DROPS[@]}"
done

echo
echo "== sizes =="
ls -lh "$IN_DIR"/*.mpq "$OUT_DIR"/*.slim.mpq 2>/dev/null | awk '{printf "  %-42s %s\n", $NF, $5}'
