#!/usr/bin/env python3
"""Measure a physical UART echo through an AirWireCom pair (8N1, no flow control)."""

import argparse
import hashlib
import json
import math
import platform
import random
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


class TransferFailure(Exception):
    pass


def run_loopback(port, *, baud, byte_count, chunk_size, timeout, seed,
                 quiet_time=0.05, clock=time.monotonic, sleep=time.sleep):
    """Stop after the first failed chunk; return JSON-compatible counters and RTTs.

    Transmitted bytes count only bytes acknowledged by write(). Successful bytes
    belong to completely matching chunks. The final quiet check rejects excess
    echo bytes; its duration is excluded from transfer throughput.
    """
    if baud <= 0 or byte_count <= 0 or not 1 <= chunk_size <= 249:
        raise ValueError("baud/bytes must be positive; chunk must be 1..249")
    if not math.isfinite(timeout) or not math.isfinite(quiet_time) or timeout <= 0 or quiet_time < 0:
        raise ValueError("timeout must be positive; quiet time must be nonnegative")
    rng = random.Random(seed)
    payload = bytes(rng.randrange(256) for _ in range(byte_count))
    received_hash = hashlib.sha256()
    sent = received = successful = 0
    rtts = []
    error = None
    transfer_end = None
    started = clock()
    try:
        for offset in range(0, byte_count, chunk_size):
            chunk = payload[offset:offset + chunk_size]
            chunk_start = clock()
            deadline = chunk_start + timeout
            written = 0
            while written < len(chunk):
                remaining = deadline - clock()
                if remaining <= 0:
                    raise TransferFailure(f"write timeout at byte {offset + written}")
                port.write_timeout = remaining
                count = port.write(chunk[written:])
                if count is None or not 0 <= count <= len(chunk) - written:
                    raise TransferFailure("serial write returned an invalid byte count")
                sent += count
                written += count
                if count == 0:
                    sleep(min(0.001, max(0, deadline - clock())))
            echo = bytearray()
            while len(echo) < len(chunk):
                remaining = deadline - clock()
                if remaining <= 0:
                    raise TransferFailure(
                        f"echo timeout at byte {offset + len(echo)} "
                        f"({len(echo)}/{len(chunk)} bytes in chunk)")
                port.timeout = min(0.05, remaining)
                data = port.read(len(chunk) - len(echo))
                received += len(data)
                received_hash.update(data)
                previous = len(echo)
                echo.extend(data)
                if echo != chunk[:len(echo)]:
                    first = next(i for i in range(previous, len(echo))
                                 if echo[i] != chunk[i])
                    raise TransferFailure(
                        f"echo mismatch at byte {offset + first}: "
                        f"expected 0x{chunk[first]:02x}, got 0x{echo[first]:02x}")
                if not data:
                    sleep(min(0.001, max(0, deadline - clock())))
            if clock() > deadline:
                raise TransferFailure(f"chunk deadline exceeded at byte {offset}")
            successful += len(chunk)
            rtts.append((clock() - chunk_start) * 1000)
        transfer_end = clock()
        quiet_deadline = transfer_end + quiet_time
        while clock() < quiet_deadline:
            port.timeout = min(0.05, quiet_deadline - clock())
            extra = port.read(1)
            if extra:
                received += len(extra)
                received_hash.update(extra)
                raise TransferFailure("unexpected trailing echo bytes")
            sleep(min(0.001, max(0, quiet_deadline - clock())))
    except (TransferFailure, OSError) as exc:
        error = f"{type(exc).__name__}: {exc}"
    elapsed = (transfer_end if transfer_end is not None else clock()) - started
    ordered = sorted(rtts)
    return {
        "status": "passed" if error is None else "failed",
        "error": error,
        "settings": {"baud": baud, "bytes": byte_count, "chunk": chunk_size,
                     "timeout_seconds": timeout, "seed": seed,
                     "quiet_time_seconds": quiet_time, "serial_format": "8N1",
                     "flow_control": "none"},
        "transmitted_bytes": sent,
        "received_bytes": received,
        "successful_bytes": successful,
        "successful_chunks": len(rtts),
        "elapsed_seconds": elapsed,
        "useful_echo_bytes_per_second": successful / elapsed if elapsed > 0 else None,
        "chunk_rtt_ms": {
            "min": min(rtts) if rtts else None,
            "median": statistics.median(rtts) if rtts else None,
            "p95": ordered[math.ceil(0.95 * len(ordered)) - 1] if ordered else None,
        },
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "received_sha256": received_hash.hexdigest(),
    }


def positive_int(value):
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def nonnegative_float(value):
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0:
        raise argparse.ArgumentTypeError("must be a finite nonnegative number")
    return parsed


def positive_float(value):
    parsed = nonnegative_float(value)
    if parsed == 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--port", required=True, help="host port, e.g. /dev/ttyACM0 or COM8")
    parser.add_argument("--baud", required=True, type=positive_int, help="UART baud rate")
    parser.add_argument("--bytes", type=positive_int, default=65536, dest="byte_count",
                        help="total payload bytes")
    parser.add_argument("--chunk", type=positive_int, default=128, help="stop-and-wait chunk, 1..249")
    parser.add_argument("--timeout", type=positive_float, default=2.0,
                        help="whole-chunk write + echo deadline in seconds")
    parser.add_argument("--seed", type=int, default=1, help="deterministic binary payload seed")
    parser.add_argument("--startup-delay", type=nonnegative_float, default=2.0,
                        help="seconds after opening port before discarding startup input")
    parser.add_argument("--quiet-time", type=nonnegative_float, default=0.05,
                        help="final check for unexpected echo bytes in seconds")
    parser.add_argument("--master", default="unspecified", help="master board and firmware revision")
    parser.add_argument("--slave", default="unspecified", help="slave board and firmware revision")
    parser.add_argument("--notes", default="", help="distance, power, wiring, radio conditions")
    parser.add_argument("--output", type=Path, required=True, help="JSON report path")
    args = parser.parse_args(argv)
    if args.chunk > 249:
        parser.error("--chunk must be 1..249")
    # Keep --help and the unit tests independent of pyserial.
    try:
        import serial
    except ImportError:
        parser.error("install dependencies: python -m pip install -r scripts/requirements-loopback.txt")
    metadata = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "port": args.port, "master": args.master, "slave": args.slave, "notes": args.notes,
        "python": platform.python_version(), "system": platform.platform(),
        "pyserial": serial.VERSION, "startup_delay_seconds": args.startup_delay,
    }
    transfer = {"baud": args.baud, "byte_count": args.byte_count,
                "chunk_size": args.chunk, "timeout": args.timeout,
                "seed": args.seed, "quiet_time": args.quiet_time}
    try:
        with serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=args.timeout,
                           bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                           stopbits=serial.STOPBITS_ONE, xonxoff=False, rtscts=False,
                           dsrdtr=False) as port:
            time.sleep(args.startup_delay)
            port.reset_input_buffer()
            report = run_loopback(port, **transfer)
    except OSError as exc:
        report = {"status": "failed", "error": f"{type(exc).__name__}: {exc}",
                  "requested": transfer}
    report["setup"] = metadata
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{report['status']}: {args.output}")
    if report["error"]:
        print(report["error"], file=sys.stderr)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
