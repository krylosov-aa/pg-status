"""Run and archive one wrk2 measurement, including Linux process resources.

CPU percentages use 100% = one logical CPU. No dependencies beyond Python 3.
The duration includes wrk2's initial ~10-second histogram calibration.
"""

import argparse
import hashlib
import json
import math
import os
import subprocess
import time
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RPS = ROOT / "test/rps"
HZ = os.sysconf("SC_CLK_TCK")


def process(pid):
    root = Path(f"/proc/{pid}")
    stat = (root / "stat").read_text().rsplit(")", 1)[1].split()
    result = {"cpu_seconds": (int(stat[11]) + int(stat[12])) / HZ}
    for line in (root / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in ("VmRSS", "VmHWM", "Threads"):
            result[key] = int(value.split()[0])
    for line in (root / "smaps_rollup").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in ("Pss", "Private_Clean", "Private_Dirty", "Swap"):
            result[key] = int(value.split()[0])
    result["fds"] = len(list((root / "fd").iterdir()))
    return result


def host_cpu():
    return list(
        map(int, Path("/proc/stat").read_text().splitlines()[0].split()[1:9])
    )


def cpu_control(pid):
    relative = (
        Path(f"/proc/{pid}/cgroup").read_text().strip().split("::", 1)[1]
    )
    root = Path("/sys/fs/cgroup")
    group = root / relative.lstrip("/")
    limits = {}
    current = group
    while current != root:
        limit = current / "cpu.max"
        if limit.exists():
            limits[str(current.relative_to(root))] = limit.read_text().strip()
        current = current.parent
    return {
        "cgroup": relative,
        "limits": limits,
        "cpu_stat": dict(
            line.split()
            for line in (group / "cpu.stat").read_text().splitlines()
        ),
    }


def probe(profile, lsn, json_mode):
    results = {}
    for line in profile.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        path = fields[1].replace("{lsn}", lsn)
        expectation = fields[2] if len(fields) == 3 else "http"
        request = urllib.request.Request("http://127.0.0.1:8000" + path)
        if json_mode:
            request.add_header("Accept", "application/json")
        with urllib.request.urlopen(request, timeout=3) as response:
            body = response.read().decode()
            assert response.status == 200
        value = json.loads(body) if json_mode or path == "/hosts" else body
        if expectation == "hosts":
            assert len(value) == 3 and all(h["alive"] for h in value)
            assert sum(h["master"] for h in value) == 1
        elif expectation != "http":
            host = value["host"] if json_mode else value
            allowed = (
                {"127.0.0.1"}
                if expectation == "primary"
                else {"127.0.0.2", "127.0.0.3"}
            )
            if expectation == "route":
                allowed.add("127.0.0.1")
            assert host in allowed, (path, expectation, host)
        results[path] = value
    return results


def validate_mix(profile, result, connections):
    weights = {"primary": 0, "replica": 0, "route": 0, "hosts": 0}
    for line in profile.read_text().splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) != 3 or fields[2] not in weights:
            return True
        weights[fields[2]] += int(fields[0])
    total = sum(weights.values())
    count = result["responses"]
    primary = result["bodies"].get("127.0.0.1", 0)
    hosts = result["bodies"].get("hosts_json", 0)
    # Responses may arrive in a different order from issued requests. Bound
    # the incomplete tail and the two workers' partial profile cycles.
    tolerance = 2 * connections + 2 * total
    return (
        count * weights["primary"] / total - tolerance
        <= primary
        <= count * (weights["primary"] + weights["route"]) / total + tolerance
        and abs(hosts - count * weights["hosts"] / total) <= tolerance
    )


def run(args):
    if (
        args.threads <= 0
        or args.connections < args.threads
        or args.connections % args.threads
    ):
        raise ValueError("connections must be a positive multiple of threads")
    settle = max(
        12, math.ceil(10 + args.connections / args.threads * 0.005) + 1
    )
    if args.duration <= settle:
        raise ValueError(
            "duration must exceed wrk2 calibration and settling time"
        )
    state = ROOT / "out/rps-state"
    pid = int((state / "server.pid").read_text())
    profile = RPS / "profiles" / f"{args.scenario}.txt"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    tag = args.tag or args.scenario
    directory = args.results / f"{stamp}-{tag}-{args.rate}"
    directory.mkdir(parents=True)
    env = dict(
        os.environ,
        BENCH_PROFILE=str(profile),
        BENCH_LSN=args.lsn,
        BENCH_JSON=str(int(args.json)),
        BENCH_CLOSE=str(int(args.close)),
    )
    if args.fresh:
        env["BENCH_LSN_FILE"] = str(state / "latest-lsn.txt")
        env["BENCH_LSN"] = (state / "latest-lsn.txt").read_text().strip()
    before = probe(profile, env["BENCH_LSN"], args.json)
    command = [
        "taskset",
        "-c",
        args.generator_cpus,
        args.wrk,
        "-t",
        str(args.threads),
        "-c",
        str(args.connections),
        "-d",
        f"{args.duration}s",
        "-R",
        str(args.rate),
        "--timeout",
        "2s",
        "--latency",
        "--u_latency",
        "-s",
        str(RPS / "workload.lua"),
        "http://127.0.0.1:8000",
    ]
    metadata = {
        "utc": stamp,
        "tag": tag,
        "scenario": args.scenario,
        "command": command,
        "env": {k: v for k, v in env.items() if k.startswith("BENCH_")},
        "server_pid": pid,
        "cpu_control_before": cpu_control(pid),
        "server_affinity": sorted(os.sched_getaffinity(pid)),
        "server_env": (state / "server-env.txt").read_text(),
        "before": before,
        "offered_rps": args.rate,
        "duration_seconds": args.duration,
        "settle_seconds": settle,
        "harness_sha256": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (Path(__file__), RPS / "workload.lua")
        },
        "wrk_sha256": hashlib.sha256(Path(args.wrk).read_bytes()).hexdigest(),
        "max_p99_ms": args.max_p99_ms,
        "min_throughput_ratio": 0.99,
    }
    (directory / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )
    (directory / "profile.txt").write_text(profile.read_text())
    samples = []
    previous = None
    with (directory / "wrk.txt").open("w") as output:
        started = time.monotonic()
        epoch_us = time.time_ns() // 1000
        env["BENCH_MEASURE_FROM_US"] = str(epoch_us + settle * 1_000_000)
        env["BENCH_MEASURE_TO_US"] = str(epoch_us + args.duration * 1_000_000)
        metadata["measurement_window_us"] = [
            int(env["BENCH_MEASURE_FROM_US"]),
            int(env["BENCH_MEASURE_TO_US"]),
        ]
        (directory / "metadata.json").write_text(
            json.dumps(metadata, indent=2) + "\n"
        )
        child = subprocess.Popen(
            command, stdout=output, stderr=subprocess.STDOUT, env=env
        )
        while child.poll() is None:
            now = time.monotonic()
            try:
                server, generator, system = (
                    process(pid),
                    process(child.pid),
                    host_cpu(),
                )
            except (FileNotFoundError, ProcessLookupError):
                break
            sample = {
                "seconds": now - started,
                "server": server,
                "generator": generator,
            }
            if previous:
                then, old_s, old_g, old_sys = previous
                dt = now - then
                sample["server_cpu_pct"] = (
                    100 * (server["cpu_seconds"] - old_s["cpu_seconds"]) / dt
                )
                sample["generator_cpu_pct"] = (
                    100
                    * (generator["cpu_seconds"] - old_g["cpu_seconds"])
                    / dt
                )
                ticks = [a - b for a, b in zip(system, old_sys)]
                sample["host_busy_pct"] = (
                    100 * (sum(ticks) - ticks[3] - ticks[4]) / sum(ticks)
                )
                sample["host_steal_pct"] = 100 * ticks[7] / sum(ticks)
            samples.append(sample)
            previous = now, server, generator, system
            time.sleep(1)
        code = child.wait(timeout=10)
    (directory / "resources.json").write_text(
        json.dumps(samples, indent=2) + "\n"
    )
    text = (directory / "wrk.txt").read_text()
    records = [
        line.removeprefix("BENCH_JSON ")
        for line in text.splitlines()
        if line.startswith("BENCH_JSON ")
    ]
    if code != 0 or len(records) != 1:
        raise RuntimeError(f"wrk2 failed ({code}): {directory / 'wrk.txt'}")
    result = json.loads(records[0])
    result.update(metadata)
    result["resources"] = samples
    result["cpu_control_after"] = cpu_control(pid)
    elapsed = result["summary"]["duration"] / 1_000_000
    result["full_run_rps"] = (
        result["responses"] - result["invalid"]
    ) / elapsed
    result["validated_rps"] = (
        result["steady_responses"] - result["steady_invalid"]
    ) / (args.duration - settle)
    try:
        result["after"] = probe(
            profile,
            (state / "latest-lsn.txt").read_text().strip()
            if args.fresh
            else args.lsn,
            args.json,
        )
    except (AssertionError, OSError, ValueError, KeyError, TypeError) as error:
        result["after_error"] = repr(error)
    result["mix_valid"] = validate_mix(profile, result, args.connections)
    result["pass"] = (
        result["invalid"] == 0
        and result["mix_valid"]
        and "after_error" not in result
        and sum(result["summary"]["errors"].values()) == 0
        and result["responses"] == result["summary"]["requests"]
        and sum(result["bodies"].values()) == result["responses"]
        and result["validated_rps"] >= args.rate * 0.99
        and result["latency_ms"]["99"] <= args.max_p99_ms
    )
    (directory / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    steady = [
        s for s in samples if s["seconds"] >= settle and "server_cpu_pct" in s
    ]
    compact = {
        "directory": directory.name,
        "tag": tag,
        "offered_rps": args.rate,
        "validated_rps": result["validated_rps"],
        "p99_ms": result["latency_ms"]["99"],
        "pass": result["pass"],
        "invalid": result["invalid"],
        "server_cpu_pct": sum(s["server_cpu_pct"] for s in steady)
        / len(steady),
        "generator_cpu_pct": sum(s["generator_cpu_pct"] for s in steady)
        / len(steady),
        "rss_mib": max(s["server"]["VmRSS"] for s in steady) / 1024,
        "pss_mib": max(s["server"]["Pss"] for s in steady) / 1024,
    }
    with (args.results / "index.jsonl").open("a") as index:
        index.write(json.dumps(compact) + "\n")
    print(json.dumps(compact), flush=True)
    return result["pass"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--tag")
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--duration", type=int, default=310)
    parser.add_argument("--connections", type=int, default=128)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--generator-cpus", default="2,3")
    parser.add_argument("--max-p99-ms", type=float, default=5)
    parser.add_argument("--lsn", default="")
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--close", action="store_true")
    parser.add_argument(
        "--wrk",
        default=os.environ.get(
            "WRK", str(Path.home() / "bench-tools/wrk2/wrk")
        ),
    )
    parser.add_argument(
        "--results", type=Path, default=RPS / "results/campaign"
    )
    args = parser.parse_args()
    if args.duration < 20 or args.rate <= 0:
        parser.error("duration must be >=20 seconds and rate must be positive")
    raise SystemExit(0 if run(args) else 1)


if __name__ == "__main__":
    main()
