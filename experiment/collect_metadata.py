#!/usr/bin/env python3
"""Read and validate the OpenSSD META_* UART export on Linux."""

import argparse
import csv
import os
import select
import stat
import sys
import termios
import time
import tty
from pathlib import Path


CSV_FIELDS = ["die", "virtual_block", "erase_count", "age", "bad", "free"]


def parse_positive_int(value, label):
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"invalid {label}: {value}") from exc
    if number <= 0:
        raise ValueError(f"{label} must be positive")
    return number


def parse_nonnegative_int(value, label):
    try:
        number = int(value, 10)
    except ValueError as exc:
        raise ValueError(f"invalid {label}: {value}") from exc
    if number < 0:
        raise ValueError(f"{label} must be nonnegative")
    return number


def parse_meta_line(line, state):
    fields = line.split(",")
    if not fields[0].startswith("META_"):
        return None

    state["last_record"] = time.monotonic()
    if fields[0] == "META_BEGIN":
        if len(fields) != 4 or state["dies"] is not None:
            raise ValueError("invalid or duplicate META_BEGIN")
        if fields[1] != "1":
            raise ValueError(f"unsupported metadata protocol version: {fields[1]}")
        dies = parse_positive_int(fields[2], "die count")
        blocks = parse_positive_int(fields[3], "block count")
        state["dies"], state["blocks"] = dies, blocks
        return None

    if state["dies"] is None or state["blocks"] is None:
        raise ValueError("metadata record received before META_BEGIN")

    if fields[0] == "META_BLOCK":
        if len(fields) != 7:
            raise ValueError("invalid META_BLOCK field count")
        die = parse_nonnegative_int(fields[1], "die index")
        block = parse_nonnegative_int(fields[2], "block index")
        erase = int(fields[3], 10)
        age = fields[4]
        bad = int(fields[5], 10)
        free = int(fields[6], 10)
        if not 0 <= die < state["dies"] or not 0 <= block < state["blocks"]:
            raise ValueError("META_BLOCK index is out of range")
        expected = state["rows"]
        if die * state["blocks"] + block != expected:
            raise ValueError("META_BLOCK records are missing, duplicated, or out of order")
        if not 0 <= erase <= 65535 or (age != "NA" and not 0 <= int(age, 10) <= 0xFFFFFFFF):
            raise ValueError("META_BLOCK erase count or age is out of range")
        if bad not in (0, 1) or free not in (0, 1):
            raise ValueError("META_BLOCK bad/free values must be 0 or 1")
        state["rows"] += 1
        return [die, block, erase, age, bad, free]

    if fields[0] == "META_END":
        if len(fields) != 2:
            raise ValueError("invalid META_END field count")
        declared_rows = int(fields[1], 10)
        expected_rows = state["dies"] * state["blocks"]
        if declared_rows != state["rows"] or state["rows"] != expected_rows:
            raise ValueError(
                f"incomplete metadata export: received {state['rows']} of {expected_rows} blocks"
            )
        state["complete"] = True
        return None

    raise ValueError(f"unknown metadata record: {fields[0]}")


def collect(device, baud, idle_timeout, output_dir):
    if not os.path.exists(device) or not stat.S_ISCHR(os.stat(device).st_mode):
        raise ValueError(f"UART device is not a character device: {device}")
    if baud not in (9600, 19200, 38400, 57600, 115200):
        raise ValueError(f"unsupported baud rate {baud}; supported: 9600..115200 standard rates")

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    csv_path = output_dir / "blocks.csv"
    partial_path = output_dir / "blocks.partial"
    log_path = output_dir / "uart.log"
    if csv_path.exists() or partial_path.exists() or log_path.exists():
        raise ValueError(f"metadata output already exists in {output_dir}; use a fresh run directory")

    baud_constant = getattr(termios, f"B{baud}")
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    old_attrs = termios.tcgetattr(fd)
    state = {"dies": None, "blocks": None, "rows": 0, "complete": False, "last_record": time.monotonic()}
    buffer = bytearray()

    try:
        tty.setraw(fd, termios.TCSANOW)
        attrs = termios.tcgetattr(fd)
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = baud_constant
        attrs[5] = baud_constant
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIFLUSH)

        with open(log_path, "wb") as uart_log, open(partial_path, "w", newline="", encoding="ascii") as csv_file:
            writer = csv.writer(csv_file, lineterminator="\n")
            writer.writerow(CSV_FIELDS)
            request = b"export\r\n"
            written = 0
            while written < len(request):
                try:
                    written += os.write(fd, request[written:])
                except BlockingIOError:
                    select.select([], [fd], [], 1.0)

            last_data = time.monotonic()
            while not state["complete"]:
                remaining = idle_timeout - (time.monotonic() - last_data)
                if remaining <= 0:
                    raise TimeoutError(
                        f"UART export timed out after {idle_timeout:g}s without data; "
                        "check UART connection, baud rate, and firmware metadata export support"
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
                uart_log.write(chunk)
                uart_log.flush()
                buffer.extend(chunk)
                while b"\n" in buffer and not state["complete"]:
                    raw_line, _, rest = buffer.partition(b"\n")
                    buffer = bytearray(rest)
                    raw_line = raw_line.rstrip(b"\r")
                    try:
                        line = raw_line.decode("ascii")
                    except UnicodeDecodeError as exc:
                        raise ValueError("UART export contained non-ASCII bytes") from exc
                    result = parse_meta_line(line, state)
                    if result is not None:
                        writer.writerow(result)
                if len(buffer) > 1023:
                    raise ValueError("UART line exceeded 1023 bytes")

            csv_file.flush()
            os.fsync(csv_file.fileno())

        os.replace(partial_path, csv_path)
        print(f"Collected {state['rows']} blocks -> {csv_path}")
    except Exception:
        try:
            partial_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, old_attrs)
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Linux serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", required=True, type=int)
    parser.add_argument("--idle-timeout", type=float, default=15.0)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    if args.idle_timeout <= 0:
        parser.error("--idle-timeout must be positive")
    try:
        collect(args.device, args.baud, args.idle_timeout, args.output_dir)
    except (OSError, ValueError, TimeoutError, termios.error) as exc:
        print(f"Metadata collection failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
