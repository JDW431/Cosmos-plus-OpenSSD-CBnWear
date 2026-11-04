#!/usr/bin/env bash
# Execute one configured sequential or random workload through fio.
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 experiment.conf [workload_mode] [run_dir]" >&2
    exit 2
fi

# shellcheck disable=SC1090
source "$1"

WORKLOAD_MODE="${2:-${WORKLOAD_MODE:-seq_write}}"
RUN_DIR="${3:-$RUN_DIR}"

case "$WORKLOAD_MODE" in
    seq_write) FIO_RW=write ;;
    seq_read)  FIO_RW=read ;;
    rand_write) FIO_RW=randwrite ;;
    rand_read)  FIO_RW=randread ;;
    *) echo "WORKLOAD_MODE must be seq_write, seq_read, rand_write, or rand_read" >&2; exit 2 ;;
esac

mkdir -p "$RUN_DIR"

fio --name="$WORKLOAD_MODE" \
    --filename="$TARGET_PATH" \
    --rw="$FIO_RW" \
    --bs="$IO_SIZE" \
    --size="$WORKING_SET_SIZE" \
    --runtime="$RUNTIME_SECONDS" \
    --time_based=1 \
    --direct="$DIRECT_IO" \
    --iodepth="$IO_DEPTH" \
    --numjobs="$NUM_JOBS" \
    --group_reporting=1 \
    --output="$RUN_DIR/fio.json" \
    --output-format=json
