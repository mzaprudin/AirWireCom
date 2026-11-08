import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from uart_loopback import main, run_loopback


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class EchoPort:
    """Serial double with bounded I/O, optional loss, corruption and duplication."""

    timeout = 0.05
    write_timeout = 1

    def __init__(self, clock, *, write_limit=249, read_limit=249,
                 drop_after=None, corrupt_at=None, duplicate_tail=False,
                 stalled_write=False, io_error=False, delay=0.001):
        self.clock = clock
        self.write_limit = write_limit
        self.read_limit = read_limit
        self.drop_after = drop_after
        self.corrupt_at = corrupt_at
        self.duplicate_tail = duplicate_tail
        self.stalled_write = stalled_write
        self.io_error = io_error
        self.delay = delay
        self.written = bytearray()
        self.pending = bytearray()

    def write(self, data):
        self.clock.sleep(self.delay)
        if self.io_error:
            raise OSError("disconnected")
        if self.stalled_write:
            return 0
        count = min(len(data), self.write_limit)
        for value in data[:count]:
            offset = len(self.written)
            self.written.append(value)
            if self.drop_after is None or offset < self.drop_after:
                self.pending.append(value ^ (1 if offset == self.corrupt_at else 0))
        if self.duplicate_tail:
            self.pending.extend(data[:count])
        return count

    def read(self, count):
        self.clock.sleep(min(self.delay, self.timeout))
        count = min(count, self.read_limit)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result


class LoopbackTests(unittest.TestCase):
    def run_case(self, *, port_options=None, **options):
        clock = Clock()
        port = EchoPort(clock, **(port_options or {}))
        settings = dict(baud=115200, byte_count=777, chunk_size=128,
                        timeout=0.1, seed=42, quiet_time=0.01)
        settings.update(options)
        return run_loopback(port, **settings, clock=clock, sleep=clock.sleep), port

    def test_binary_echo_crosses_chunks_and_includes_short_last_chunk(self):
        report, port = self.run_case()
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["successful_chunks"], 7)
        self.assertEqual(report["successful_bytes"], 777)
        self.assertEqual(report["received_bytes"], 777)
        self.assertEqual(report["transmitted_bytes"], 777)
        self.assertIn(0, port.written)
        self.assertIn(255, port.written)
        self.assertEqual(report["payload_sha256"], hashlib.sha256(port.written).hexdigest())
        self.assertEqual(report["received_sha256"], report["payload_sha256"])
        self.assertAlmostEqual(report["chunk_rtt_ms"]["median"], 2)

    def test_partial_reads_and_writes_complete_exactly(self):
        report, _ = self.run_case(port_options={"write_limit": 17, "read_limit": 13})
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["transmitted_bytes"], 777)
        self.assertEqual(report["received_bytes"], report["successful_bytes"])

    def test_payload_is_deterministic_and_seed_changes_it(self):
        first, port_a = self.run_case()
        second, port_b = self.run_case()
        changed, _ = self.run_case(seed=43)
        self.assertEqual(port_a.written, port_b.written)
        self.assertEqual(first["payload_sha256"], second["payload_sha256"])
        self.assertNotEqual(first["payload_sha256"], changed["payload_sha256"])

    def test_corrupt_chunk_stops_before_next_chunk(self):
        report, port = self.run_case(port_options={"corrupt_at": 134, "read_limit": 11})
        self.assertEqual(report["status"], "failed")
        self.assertIn("mismatch at byte 134", report["error"])
        self.assertEqual(report["successful_bytes"], 128)
        self.assertEqual(report["successful_chunks"], 1)
        self.assertEqual(len(port.written), 256)

    def test_lost_suffix_times_out_and_does_not_resume(self):
        report, port = self.run_case(port_options={"drop_after": 133})
        self.assertEqual(report["status"], "failed")
        self.assertIn("echo timeout at byte 133", report["error"])
        self.assertEqual(report["received_bytes"], 133)
        self.assertEqual(report["successful_bytes"], 128)
        self.assertEqual(len(port.written), 256)

    def test_missing_echo_has_no_successful_bytes_or_rtts(self):
        report, _ = self.run_case(port_options={"drop_after": 0})
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["successful_bytes"], 0)
        self.assertEqual(report["received_bytes"], 0)
        self.assertIsNone(report["chunk_rtt_ms"]["p95"])

    def test_zero_writes_obey_deadline(self):
        report, _ = self.run_case(port_options={"stalled_write": True})
        self.assertEqual(report["status"], "failed")
        self.assertIn("write timeout", report["error"])
        self.assertEqual(report["transmitted_bytes"], 0)

    def test_late_complete_echo_is_failure(self):
        report, _ = self.run_case(port_options={"delay": 0.06}, timeout=0.05)
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["successful_bytes"], 0)

    def test_trailing_duplicates_cannot_pass(self):
        report, _ = self.run_case(port_options={"duplicate_tail": True}, byte_count=128)
        self.assertEqual(report["status"], "failed")
        self.assertIn("trailing echo", report["error"])
        self.assertEqual(report["received_bytes"], 129)

    def test_quiet_check_excluded_from_transfer_time(self):
        short, _ = self.run_case(quiet_time=0.01)
        long, _ = self.run_case(quiet_time=0.2)
        self.assertEqual(short["elapsed_seconds"], long["elapsed_seconds"])
        self.assertEqual(short["useful_echo_bytes_per_second"], long["useful_echo_bytes_per_second"])

    def test_disconnect_is_reported(self):
        report, _ = self.run_case(port_options={"io_error": True})
        self.assertEqual(report["status"], "failed")
        self.assertIn("OSError: disconnected", report["error"])

    def test_invalid_settings_rejected(self):
        for settings in ({"baud": 0}, {"byte_count": 0}, {"chunk_size": 250},
                         {"chunk_size": 0}, {"timeout": 0}, {"quiet_time": -1},
                         {"timeout": float("nan")}, {"quiet_time": float("inf")}):
            with self.subTest(settings=settings), self.assertRaises(ValueError):
                self.run_case(**settings)

    def test_open_error_writes_failure_report_and_nonzero_exit(self):
        def fail_open(*args, **kwargs):
            raise OSError("missing port")

        serial = SimpleNamespace(VERSION="test-double", Serial=fail_open,
                                 EIGHTBITS=8, PARITY_NONE="N", STOPBITS_ONE=1)
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "failure.json"
            with patch.dict("sys.modules", {"serial": serial}), patch("builtins.print"):
                status = main(["--port", "missing", "--baud", "115200", "--output", str(target)])
            report = json.loads(target.read_text())
        self.assertEqual(status, 1)
        self.assertEqual(report["status"], "failed")
        self.assertIn("missing port", report["error"])
        self.assertEqual(report["requested"]["baud"], 115200)
        self.assertNotIn("useful_echo_bytes_per_second", report)


if __name__ == "__main__":
    unittest.main()
