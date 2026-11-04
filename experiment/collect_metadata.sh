#!/usr/bin/env bash
# Ubuntu helper: collect and validate the firmware metadata export over UART.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 experiment.conf run_dir" >&2
    exit 2
fi

CONFIG_PATH="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
RUN_DIR="$2"
if [[ "$RUN_DIR" != /* ]]; then
    RUN_DIR="$(pwd)/$RUN_DIR"
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck disable=SC1090
source "$CONFIG_PATH"

if [[ ! -c "$UART_DEVICE" ]]; then
    echo "UART_DEVICE is not a character device: $UART_DEVICE" >&2
    exit 2
fi

mkdir -p "$RUN_DIR/metadata"
python3 "$SCRIPT_DIR/collect_metadata.py" \
    --device "$UART_DEVICE" \
    --baud "$UART_BAUD" \
    --idle-timeout "$UART_IDLE_TIMEOUT_SECONDS" \
    --output-dir "$RUN_DIR/metadata"
