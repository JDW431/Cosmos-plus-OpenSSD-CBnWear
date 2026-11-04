#!/usr/bin/env bash
# Run workload, collect one complete metadata snapshot, then create the plots.
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
    python3 "$SCRIPT_DIR/plot_results.py" \
        --blocks "$RUN_DIR/metadata/blocks.csv" \
        --threshold "$AGE_THRESHOLD" \
        --output-dir "$RUN_DIR"

    echo "Complete: $RUN_DIR"
done

echo "All workload runs completed under: $RUNS_ROOT"
