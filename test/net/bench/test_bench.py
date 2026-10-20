"""Harness regressions; fake exporters make readiness failures deterministic."""

import csv
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

import report

HERE = Path(__file__).resolve().parent


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)

    def run_command(self, command, **kwargs):
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
            **kwargs,
        )
        try:
            output, _ = process.communicate(timeout=8)
            return process.returncode, output
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGKILL)
                process.communicate()

    def test_tiny_remaining_budget_never_disables_curl_timeout(self):
        source = (HERE / "bench.sh").read_text()
        function = "remaining() {" + source.split("remaining() {", 1)[1].split("\n}", 1)[0] + "\n}"
        status, output = self.run_command(
            ["bash", "-c", function + "\nnow() { echo 42; }; remaining 42.0000001 5"]
        )
        self.assertEqual(status, 1, output)
        self.assertEqual(output, "")

    def test_readiness_budget_includes_initial_scrapes_and_polling(self):
        runner = self.root / "runner"
        runner.write_text("#!/bin/sh\nexec sleep 30\n")
        runner.chmod(0o755)
        curl = self.root / "curl"
        curl.write_text("""#!/bin/sh
count=$(cat "$CURL_CALLS" 2>/dev/null || echo 0)
count=$((count + 1))
echo "$count" > "$CURL_CALLS"
if [ "$count" -lt "$CURL_FAIL_AT" ]; then
  echo 'bench_connections_current 0'
  exit 0
fi
while [ "$1" != --max-time ]; do shift; done
sleep "$2"
exit 1
""")
        curl.chmod(0o755)
        # Process accounting is irrelevant here; no real network or benchmark binary is used.
        ps = self.root / "ps"
        ps.write_text("#!/bin/sh\necho 0\n")
        ps.chmod(0o755)
        for fail_at in (1, 2, 3):
            with self.subTest(failed_scrape=fail_at):
                output_dir = self.root / f"run-{fail_at}"
                env = dict(
                    os.environ,
                    PATH=f"{self.root}:{os.environ['PATH']}",
                    READY_TIMEOUT="2",
                    CURL_FAIL_AT=str(fail_at),
                    CURL_CALLS=str(self.root / f"calls-{fail_at}"),
                )
                started = time.monotonic()
                status, output = self.run_command(
                    [
                        "bash",
                        str(HERE / "bench.sh"),
                        "-I",
                        "quic-go",
                        "-T",
                        "idle",
                        "-c",
                        "1",
                        "-b",
                        str(runner),
                        "-o",
                        str(output_dir),
                    ],
                    env=env,
                )
                self.assertEqual(status, 1, output)
                self.assertLess(time.monotonic() - started, 4, output)
                self.assertIn("metrics scrape failed", output)
                self.assertEqual(
                    self.root.joinpath(f"calls-{fail_at}").read_text().strip(), str(fail_at)
                )
                with (output_dir / "manifest.tsv").open() as source:
                    manifest = dict(csv.reader(source, delimiter="\t"))
                self.assertEqual(manifest["measured_side"], "receiver")

    def test_report_records_measured_side(self):
        start = dict.fromkeys(report.NATIVE_FAMILIES, 0)
        start["bench_connections_current"] = 1
        start["bench_connections_ready"] = 1
        end = dict(start, bench_messages_sent_total=100, bench_messages_received_total=100)
        for prefix in ("metrics", "peer-metrics"):
            for phase, sample in (("start", start), ("end", end)):
                (self.root / f"{prefix}-{phase}.txt").write_text(
                    "".join(f"{key} {value}\n" for key, value in sample.items())
                )
        args = """
            --implementation quic-go --protocol quic --workload message --topology fanin-1
            --side sender --peers 1 --clients 1 --connections 1 --size 240 --response-size 1
            --rate 10 --burst 1 --inflight 1 --threads 1 --tuned 1
            --stream-credit-profile parity-4096 --congestion-control cubic --warmup 1
            --git-revision test --binary-sha256 test --elapsed 10 --metrics-elapsed 10
            --peer-metrics-elapsed 10 --cpu-start 0 --cpu-end 1 --peer-cpu-start 0 --peer-cpu-end 1
            --rss-baseline 0 --rss-start 1 --rss-end 1 --peer-rss-start 1 --peer-rss-end 1
            --driver-errors 0 --host-udp-receive-drops 0 --label test
        """.split()
        status, output = self.run_command(
            [sys.executable, str(HERE / "report.py"), "--dir", str(self.root), *args]
        )
        self.assertEqual(status, 0, output)
        with (self.root / "summary.tsv").open() as source:
            row = next(csv.DictReader(source, delimiter="\t"))
        self.assertEqual(row["measured_side"], "sender")

    def test_summary_keeps_opposite_endpoints_separate(self):
        row = dict(
            label="test",
            implementation="ton",
            protocol="quic",
            workload="message",
            stream_credit_profile="parity-4096",
            status="ok",
            measured_side="sender",
        )
        summary = self.root / "summary.tsv"
        with summary.open("w") as target:
            writer = csv.DictWriter(target, fieldnames=row, delimiter="\t")
            writer.writeheader()
            writer.writerows((row, dict(row, measured_side="receiver")))
        status, output = self.run_command(
            [sys.executable, str(HERE / "summarize.py"), str(summary)]
        )
        self.assertEqual(status, 0, output)
        self.assertEqual(len(output.splitlines()), 3, output)
        self.assertIn("sender", output)
        self.assertIn("receiver", output)

    @unittest.skipUnless(
        os.environ.get("TON_BENCH"), "set TON_BENCH for the localhost C++ regression"
    )
    def test_malformed_response_fails_immediately(self):
        used_ports = set()

        def address():
            while True:
                with (
                    socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp,
                    socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as quic,
                ):
                    udp.bind(("127.0.0.1", 0))
                    port = udp.getsockname()[1]
                    if port > 64535 or {port, port + 1000} & used_ports:
                        continue
                    try:
                        quic.bind(("127.0.0.1", port + 1000))
                    except OSError:
                        continue
                    used_ports.update((port, port + 1000))
                    return f"127.0.0.1:{port}"

        server_addr, client_addr = address(), address()
        binary = str(Path(os.environ["TON_BENCH"]).resolve())
        server = subprocess.Popen(
            [binary, "--server", "--quic", "-a", server_addr, "-r", "1", "-t", "1"],
            cwd=self.root,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            status, output = self.run_command(
                [
                    binary,
                    "--client",
                    "--quic",
                    "-a",
                    client_addr,
                    "-s",
                    server_addr,
                    "-r",
                    "240",
                    "-n",
                    "6",
                    "-c",
                    "1",
                    "-t",
                    "1",
                    "--test-timeout",
                    "5",
                ],
                cwd=self.root,
            )
            self.assertNotEqual(status, 0, output)
            self.assertIn(": bad response", output)
            self.assertNotIn("Benchmark complete", output)
        finally:
            server.terminate()
            server.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
