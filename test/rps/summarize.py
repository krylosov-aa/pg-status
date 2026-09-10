"""Summarize confirmed runs without pooling latency percentiles."""

import argparse
import json
from pathlib import Path


def summarize(root):
    suite = json.loads((root / "suite.json").read_text())
    rows = {}
    for name, case in suite.items():
        if "confirmed" not in case:
            raise ValueError(f"not yet confirmed: {name}")
        rate = case["confirmed"]
        candidates = [
            r for r in case["confirmation"] if r["offered_rps"] == rate
        ]
        assert len(candidates) >= 3 and all(r["pass"] for r in candidates)
        chosen = candidates[-3:]
        reports = [
            json.loads((root / r["directory"] / "report.json").read_text())
            for r in chosen
        ]
        failed = [
            r
            for r in case["search"] + case["confirmation"]
            if not r["pass"] and r["offered_rps"] > rate
        ]
        higher = min(r["offered_rps"] for r in failed)
        samples = [
            s
            for report in reports
            for s in report["resources"]
            if s["seconds"] >= report.get("settle_seconds", 12)
            and "server_cpu_pct" in s
        ]
        rows[name] = {
            "confirmed_rps": rate,
            "actual_rps_min": min(r["validated_rps"] for r in reports),
            "actual_rps_max": max(r["validated_rps"] for r in reports),
            "p99_ms_worst": max(r["latency_ms"]["99"] for r in reports),
            "p999_ms_worst": max(r["latency_ms"]["99.9"] for r in reports),
            "max_latency_ms": max(r["latency_ms"]["max"] for r in reports),
            "server_cpu_pct_mean": sum(s["server_cpu_pct"] for s in samples)
            / len(samples),
            "generator_cpu_pct_mean": sum(
                s["generator_cpu_pct"] for s in samples
            )
            / len(samples),
            "server_rss_mib_max": max(s["server"]["VmRSS"] for s in samples)
            / 1024,
            "server_pss_mib_max": max(s["server"]["Pss"] for s in samples)
            / 1024,
            "host_steal_pct_max": max(s["host_steal_pct"] for s in samples),
            "requests": sum(r["responses"] for r in reports),
            "invalid": sum(r["invalid"] for r in reports),
            "duration_seconds_each": reports[0]["duration_seconds"],
            "runs": [r["directory"] for r in chosen],
            "next_failed_rps": higher,
            "next_failed_runs": [
                r for r in failed if r["offered_rps"] == higher
            ],
        }
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = summarize(args.results)
    args.output.write_text(json.dumps(rows, indent=2) + "\n")
    print(
        "| Scenario | RPS | Worst p99, ms | CPU, % of one vCPU | Peak RSS, MiB |"
    )
    print("|---|---:|---:|---:|---:|")
    for name, row in rows.items():
        print(
            f"| {name} | {row['confirmed_rps']:,} | "
            f"{row['p99_ms_worst']:.3f} | "
            f"{row['server_cpu_pct_mean']:.1f} | "
            f"{row['server_rss_mib_max']:.2f} |"
        )


if __name__ == "__main__":
    main()
