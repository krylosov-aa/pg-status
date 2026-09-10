"""Black-box regression checks of the measurement tool (Linux, built driver)."""

import json
import os
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def do_GET(self):
        if self.path == "/slow":
            time.sleep(0.02)
        body = b"wrong-host" if self.path == "/wrong" else b"127.0.0.1"
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    root = Path(__file__).resolve().parent
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = {}
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for route in ("slow", "wrong", "ok"):
            profile = temporary / "profile.txt"
            profile.write_text(f"1 /{route} primary\n")
            out = temporary / "report.json"
            run = subprocess.run(
                [
                    str(root / "pg-status-load"),
                    "-url",
                    f"http://127.0.0.1:{server.server_port}",
                    "-profile",
                    str(profile),
                    "-pid",
                    str(os.getpid()),
                    "-rate",
                    "100",
                    "-duration",
                    "2s",
                    "-warmup",
                    "0s",
                    "-workers",
                    "1",
                    "-connections",
                    "1",
                    "-max-p99-ms",
                    "100",
                    "-min-throughput-ratio",
                    "0.98",
                    "-out",
                    str(out),
                ],
                check=False,
            )
            report = json.loads(out.read_text())
            if route == "slow":
                assert run.returncode == 1 and report["invalid"] == 0
                assert report["validated_rps"] < 60
                assert report["scheduled_latency_ms"]["99"] > 500
                assert report["http_latency_ms"]["99"] < 150
            elif route == "wrong":
                assert (
                    run.returncode == 1
                    and report["invalid"] == report["requests"]
                )
                assert report["status_codes"] == {"200": report["requests"]}
            else:
                assert run.returncode == 0 and report["invalid"] == 0
            results[route] = report
            wrk = Path(
                os.environ.get(
                    "WRK", str(Path.home() / "bench-tools/wrk2/wrk")
                )
            )
            if wrk.exists():
                epoch = time.time_ns() // 1000
                environment = dict(
                    os.environ,
                    BENCH_PROFILE=str(profile),
                    BENCH_MEASURE_FROM_US=str(epoch + 1_000_000),
                    BENCH_MEASURE_TO_US=str(epoch + 3_000_000),
                )
                check = subprocess.run(
                    [
                        str(wrk),
                        "-t1",
                        "-c1",
                        "-R100",
                        "-d3s",
                        "--latency",
                        "-s",
                        str(root / "workload.lua"),
                        f"http://127.0.0.1:{server.server_port}",
                    ],
                    capture_output=True,
                    text=True,
                    env=environment,
                    check=True,
                )
                record = json.loads(
                    next(
                        line.removeprefix("BENCH_JSON ")
                        for line in check.stdout.splitlines()
                        if line.startswith("BENCH_JSON ")
                    )
                )
                if route == "slow":
                    assert 40 < record["steady_responses"] / 2 < 60
                    assert record["latency_ms"]["99"] > 500
                elif route == "wrong":
                    assert record["invalid"] == record["responses"] > 0
                    assert (
                        record["steady_invalid"]
                        == record["steady_responses"]
                        > 0
                    )
                else:
                    assert 95 < record["steady_responses"] / 2 < 105
                    assert record["invalid"] == 0
                results["wrk-" + route] = record
    server.shutdown()
    print(json.dumps({"passed": True, "cases": results}, indent=2))


if __name__ == "__main__":
    main()
