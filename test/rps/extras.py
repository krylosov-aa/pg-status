"""Low-load resource curve and a 100m CPU sidecar limit (dedicated VM only)."""

import json
import signal
import subprocess
import sys
import time
from pathlib import Path

from campaign import process
from suite import RESULTS, ROOT, measure


def write_control(path, value):
    subprocess.run(
        ["sudo", "-n", "tee", str(path)],
        input=value + "\n",
        text=True,
        stdout=subprocess.DEVNULL,
        check=True,
    )


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(130))
    result = {"resource_curve": [], "quota_search": [], "quota_confirm": []}
    out = RESULTS / "extras.json"

    def save():
        out.write_text(json.dumps(result, indent=2) + "\n")

    pid = int((ROOT / "out/rps-state/server.pid").read_text())
    before = process(pid)
    started = time.monotonic()
    time.sleep(30)
    after = process(pid)
    elapsed = time.monotonic() - started
    result["warm_idle"] = {
        "seconds": elapsed,
        "cpu_pct": (after["cpu_seconds"] - before["cpu_seconds"])
        / elapsed
        * 100,
        "rss_mib": after["VmRSS"] / 1024,
        "pss_mib": after["Pss"] / 1024,
    }
    save()
    for rate in (1000, 5000, 10000, 20000):
        result["resource_curve"].append(measure("mixed", rate, 70, "resource"))
        save()
    group = Path("/sys/fs/cgroup/pg-status-benchmark")
    subprocess.run(["sudo", "-n", "mkdir", "-p", str(group)], check=True)
    members = (group / "cgroup.procs").read_text().split()
    assert not members or members == [str(pid)], members
    original = (group / "cpu.max").read_text().strip()
    result["cpu_max"] = "10000 100000"
    try:
        # Move only pg-status; the PostgreSQL processes retain their group.
        write_control(group / "cgroup.procs", str(pid))
        write_control(group / "cpu.max", result["cpu_max"])
        passed = 0
        for rate in (500, 1000, 2000, 3000, 4000, 5000, 6000):
            report = measure("mixed", rate, 30, "quota100m-search")
            result["quota_search"].append(report)
            save()
            if not report["pass"]:
                break
            passed = rate
        assert passed > 0
        while True:
            reports = [
                measure("mixed", passed, 70, f"quota100m-confirm{i}")
                for i in range(1, 4)
            ]
            result["quota_confirm"].extend(reports)
            save()
            if all(r["pass"] for r in reports):
                result["quota_confirmed_rps"] = passed
                save()
                break
            passed -= 500
            assert passed > 0
    finally:
        write_control(group / "cpu.max", original)
        result["restored_cpu_max"] = (group / "cpu.max").read_text().strip()
        save()
    (RESULTS / "current.json").write_text(
        json.dumps({"phase": "extras", "finished": time.time()}) + "\n"
    )


if __name__ == "__main__":
    main()
