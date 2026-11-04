#!/usr/bin/env bash
# Receive the firmware's UART metadata export and publish blocks.csv in RUN_DIR.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: $0 experiment.conf [run_dir]" >&2
    exit 2
fi

# shellcheck disable=SC1090
source "$1"
RUN_DIR="${2:-$RUN_DIR}"

mkdir -p "$RUN_DIR"
if [[ ! -x "$COLLECT_EXE" ]]; then
    echo "COLLECT_EXE is not executable: $COLLECT_EXE" >&2
    exit 2
fi

COLLECT_PATH="$(cd "$(dirname "$COLLECT_EXE")" && pwd)/$(basename "$COLLECT_EXE")"
METADATA_DIR=metadata
if [[ -e "$RUN_DIR/$METADATA_DIR" ]]; then
    echo "Metadata directory already exists: $RUN_DIR/$METADATA_DIR" >&2
    exit 2
fi

# collect.exe itself creates a new directory, so run it from RUN_DIR and give it
# a fresh child directory name.
(cd "$RUN_DIR" && "$COLLECT_PATH" "$UART_PORT" "$UART_BAUD" "$METADATA_DIR")
