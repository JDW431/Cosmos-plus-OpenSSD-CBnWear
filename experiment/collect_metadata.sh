#!/usr/bin/env bash
# Ubuntu helper: validate and import blocks.csv collected over UART on the SDK laptop.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 run_dir laptop_blocks.csv" >&2
    exit 2
fi

RUN_DIR="$1"
METADATA_CSV="$2"
METADATA_CSV="${METADATA_CSV/#\~/$HOME}"
if [[ ! -f "$METADATA_CSV" ]]; then
    echo "Metadata CSV not found: $METADATA_CSV" >&2
    exit 2
fi

CSV_HEADER="$(head -n 1 "$METADATA_CSV" | tr -d '\r')"
EXPECTED_HEADER="die,virtual_block,erase_count,age,bad,free"
if [[ "$CSV_HEADER" != "$EXPECTED_HEADER" ]]; then
    echo "Unexpected blocks.csv header: $CSV_HEADER" >&2
    echo "Expected: $EXPECTED_HEADER" >&2
    exit 2
fi

mkdir -p "$RUN_DIR/metadata"
cp "$METADATA_CSV" "$RUN_DIR/metadata/blocks.csv"
echo "Imported metadata: $RUN_DIR/metadata/blocks.csv"
