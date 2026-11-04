#!/usr/bin/env python3
"""Collect the OpenSSD META_* UART export using Ubuntu's serial device."""

import argparse
import csv
import os
import select
import stat
import subprocess
import sys
import time
from pathlib import Path


CSV_FIELDS = ["die", "virtual_block", "erase_count", "age", "bad", "free"]


def uint(value, label, maximum=0xFFFFFFFF):
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"invalid {label}: {value!r}") from exc
    if not 0 <= number <= maximum:
        raise ValueError(f"{label} out of range: {value!r}")
    return number


def parse_line(line, state):
    fields = line.split(",")
    if not fields[0].startswith("META_"):
        return None

    if fields[0] == "META_BEGIN":
        if len(fields) != 4 or state["dies"] is not None:
            raise ValueError("invalid or duplicate META_BEGIN")
        if fields[1] != "1":
            raise ValueError(f"unsupported metadata protocol version: {fields[1]}")
        state["dies"] = uint(fields[2], "die count")
        state["blocks"] = uint(fields[3], "block count")
        if not state["dies"] or not state["blocks"]:
            raise ValueError("die and block counts must be nonzero")
        return None

    if state["dies"] is None:
        raise ValueError("metadata record received before META_BEGIN")

    if fields[0] == "META_BLOCK":
        if len(fields) != 7:
            raise ValueError("invalid META_BLOCK field count")
        die = uint(fields[1], "die index")
        block = uint(fields[2], "block index")
        erase = uint(fields[3], "erase count", 65535)
        age = fields[4]
        if age != "NA":
            uint(age, "age")
        bad = uint(fields[5], "bad flag", 1)
        free = uint(fields[6], "free flag", 1)
        if die >= state["dies"] or block >= state["blocks"]:
            raise ValueError("META_BLOCK index out of range")
        expected_index = state["rows"]
        if die * state["blocks"] + block != expected_index:
            raise ValueError("META_BLOCK records are missing, duplicated, or out of order")
        state["rows"] += 1
        return [die, block, erase, age, bad, free]

    if fields[0] == "META_END":
        if len(fields) != 2:
            raise ValueError("invalid META_END field count")
        declared = uint(fields[1], "META_END row count")
        expected = state["dies"] * state["blocks"]
        if declared != state["rows"] or state["rows"] != expected:
            raise ValueError(f"incomplete export: received {state['rows']} of {expected} blocks")
        state["complete"] = True
        return None

    raise ValueError(f"unknown metadata record: {fields[0]}")


def collect(device, baud, idle_timeout, output_dir):
    if not os.path.exists(device) or not stat.S_ISCHR(os.stat(device).st_mode):
        raise ValueError(f"UART device is not a character device: {device}")
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "blocks.csv"
    partial_path = output_dir / "blocks.partial"
    log_path = output_dir / "uart.log"
    if csv_path.exists() or partial_path.exists() or log_path.exists():
        raise ValueError(f"output files already exist in {output_dir}; use a fresh run directory")

    # Use the system `stty` utility to configure the UART. This avoids importing
    # Python's termios extension, which may be unavailable or damaged on Ubuntu.
    old_settings = subprocess.run(
        ["stty", "-F", device, "-g"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    subprocess.run(
        ["stty", "-F", device, str(baud), "raw", "-echo", "-ixon", "-ixoff", "-crtscts", "-flush"],
        check=True,
    )
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    state = {"dies": None, "blocks": None, "rows": 0, "complete": False}
    pending = bytearray()
    try:
        with open(log_path, "wb") as raw_log, open(partial_path, "w", newline="", encoding="ascii") as csv_file:
            writer = csv.writer(csv_file, lineterminator="\n")
            writer.writerow(CSV_FIELDS)

            request = b"export\r\n"
            sent = 0
            while sent < len(request):
                try:
                    sent += os.write(fd, request[sent:])
                except BlockingIOError:
                    select.select([], [fd], [], 1.0)

            last_data = time.monotonic()
            while not state["complete"]:
                remaining = idle_timeout - (time.monotonic() - last_data)
                if remaining <= 0:
                    raise TimeoutError(
                        f"no UART data for {idle_timeout:g}s; check UART connection, baud, and firmware export support"
                    )
                readable, _, _ = select.select([fd], [], [], min(remaining, 1.0))
                if not readable:
                    continue
                try:
                    chunk = os.read(fd, 4096)
                except BlockingIOError:
                    continue
                if not chunk:
                    continue
                last_data = time.monotonic()
                raw_log.write(chunk)
                raw_log.flush()
                pending.extend(chunk)
                while b"\n" in pending and not state["complete"]:
                    raw_line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    try:
                        line = raw_line.rstrip(b"\r").decode("ascii")
                    except UnicodeDecodeError as exc:
                        raise ValueError("UART stream contains non-ASCII data") from exc
                    row = parse_line(line, state)
                    if row is not None:
                        writer.writerow(row)
                if len(pending) > 1023:
                    raise ValueError("UART line exceeded 1023 bytes")

            csv_file.flush()
            os.fsync(csv_file.fileno())

        os.replace(partial_path, csv_path)
        print(f"Collected {state['rows']} blocks -> {csv_path}")
    finally:
        os.close(fd)
        subprocess.run(["stty", "-F", device, old_settings], check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Linux UART path, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", required=True, type=int)
    parser.add_argument("--idle-timeout", type=float, default=15.0)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    if args.idle_timeout <= 0:
        parser.error("--idle-timeout must be positive")
    try:
        collect(args.device, args.baud, args.idle_timeout, args.output_dir)
    except (OSError, ValueError, TimeoutError, subprocess.CalledProcessError) as exc:
        print(f"Metadata collection failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
