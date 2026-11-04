#!/usr/bin/env bash
# Run workload, collect one complete metadata snapshot, then create the plots.
# Ubuntu host: run each workload, pause for Windows laptop UART collection,
# import the resulting blocks.csv, then create the plots.
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 experiment.conf" >&2
    exit 2
fi

CONFIG_PATH="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
CONFIG_DIR="$(dirname "$CONFIG_PATH")"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck disable=SC1090
source "$CONFIG_PATH"

WORKLOAD_MODES=(seq_write seq_read rand_write rand_read)
if [[ "$RUNS_ROOT" != /* ]]; then
    RUNS_ROOT="$CONFIG_DIR/$RUNS_ROOT"
fi

if [[ ! -b "$TARGET_PATH" ]]; then
    echo "TARGET_PATH is not a block device: $TARGET_PATH" >&2
    echo "Set TARGET_PATH to the OpenSSD device on the Ubuntu host." >&2
    exit 2
fi
if ! command -v fio >/dev/null 2>&1; then
    echo "fio is not installed or not on PATH." >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is not installed or not on PATH." >&2
    exit 2
fi

echo "Ubuntu Host PC experiment"
echo "Target device: $TARGET_PATH"
echo "The fio write workloads can overwrite data on this device."
read -r -p "Type the exact device path to continue [$TARGET_PATH]: " TARGET_CONFIRM
if [[ "$TARGET_CONFIRM" != "$TARGET_PATH" ]]; then
    echo "Target confirmation did not match; stopping." >&2
    exit 2
fi

mkdir -p "$RUNS_ROOT"

for WORKLOAD_MODE in "${WORKLOAD_MODES[@]}"; do
    RUN_DIR="$RUNS_ROOT/${WORKLOAD_MODE}_threshold_${AGE_THRESHOLD}"
    if [[ -e "$RUN_DIR" ]]; then
        echo "Run directory already exists: $RUN_DIR" >&2
        echo "Change RUNS_ROOT or remove/archive the previous run first." >&2
        exit 2
    fi

    mkdir -p "$RUN_DIR"
    cp "$CONFIG_PATH" "$RUN_DIR/experiment.conf"
    printf 'workload_mode,age_threshold\n%s,%s\n' "$WORKLOAD_MODE" "$AGE_THRESHOLD" \
        > "$RUN_DIR/run_metadata.csv"

    echo "Starting $WORKLOAD_MODE (threshold=$AGE_THRESHOLD)"
    "$SCRIPT_DIR/workload.sh" "$CONFIG_PATH" "$WORKLOAD_MODE" "$RUN_DIR"
    "$SCRIPT_DIR/collect_metadata.sh" "$CONFIG_PATH" "$RUN_DIR"
    bash "$SCRIPT_DIR/workload.sh" "$CONFIG_PATH" "$WORKLOAD_MODE" "$RUN_DIR"

    CAPTURE_NAME="${WORKLOAD_MODE}_threshold_${AGE_THRESHOLD}_capture"
    echo
    echo "Now collect this workload's UART metadata on the Windows SDK laptop."
    echo "Disconnect/close the SDK UART terminal first so collect.exe can open the COM port."
    echo "From the folder containing collect.exe, run:"
    echo "  collect.exe $UART_PORT $UART_BAUD $CAPTURE_NAME"
    echo "The CSV will be at: $CAPTURE_NAME/blocks.csv"
    echo "Copy it to the Ubuntu host (USB/shared folder), then enter its Ubuntu path here."
    read -r -p "Enter the blocks.csv path visible on Ubuntu: " METADATA_CSV
    METADATA_CSV="${METADATA_CSV/#\~/$HOME}"
    if [[ ! -f "$METADATA_CSV" ]]; then
        echo "CSV file not found: $METADATA_CSV" >&2
        echo "Copy the laptop's blocks.csv to Ubuntu, then rerun this experiment." >&2
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
    python3 "$SCRIPT_DIR/plot_results.py" \
        --blocks "$RUN_DIR/metadata/blocks.csv" \
        --threshold "$AGE_THRESHOLD" \
        --output-dir "$RUN_DIR"

    echo "Complete: $RUN_DIR"
done

echo "All workload runs completed under: $RUNS_ROOT"
