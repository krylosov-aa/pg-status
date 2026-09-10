"""Sequential search and repeated confirmations on the dedicated VM.

All attempted runs, including failures, are retained. SLO failures lower the
candidate; they are never discarded or averaged away.
"""

import argparse
import json
import subprocess
import time
from contextlib import contextmanager

import psycopg2
from campaign import ROOT, RPS

CASES = {
    "master": ("master", [], 310),
    "master-most-sync": ("master-most-sync", [], 310),
    "ryow-fresh": ("ryow-fresh", ["--fresh"], 310),
    "ryow-hit": ("ryow-hit", [], 70),
    "ryow-fallback": ("ryow-fallback", [], 70),
    "read-heavy": ("read-heavy", [], 70),
    "mixed": ("mixed", [], 70),
    "mixed-json": ("mixed", ["--json"], 70),
    "hosts": ("hosts", [], 70),
    "new-connection": ("master", ["--close"], 70),
}
RESULTS = RPS / "results/campaign"


@contextmanager
def topology(case):
    conns = []
    try:
        if case == "ryow-fallback":
            for host in ("127.0.0.2", "127.0.0.3"):
                conn = psycopg2.connect(
                    host=host, user="postgres", dbname="postgres"
                )
                conn.autocommit = True
                conns.append(conn)
                with conn.cursor() as cursor:
                    cursor.execute("SELECT pg_wal_replay_pause()")
            time.sleep(0.2)
            lsn = (ROOT / "out/rps-state/latest-lsn.txt").read_text().strip()
        else:
            # A real committed LSN previously verified by check-ryow.py.
            check = json.loads(
                (RESULTS / "environment/ryow-validation.json").read_text()
            )
            lsn = check["replayed_committed_lsn"]["lsn"]
        yield lsn
    finally:
        resume_errors = []
        for conn in conns:
            try:
                for attempt in range(10):
                    try:
                        with conn.cursor() as cursor:
                            cursor.execute("SELECT pg_wal_replay_resume()")
                        break
                    except psycopg2.errors.SerializationFailure:
                        # Resuming replay can cancel this very SELECT when
                        # queued vacuum WAL conflicts with its snapshot.
                        if attempt == 9:
                            raise
                        time.sleep(0.1)
            except psycopg2.Error as error:
                resume_errors.append(str(error))
            finally:
                conn.close()
        if conns:
            time.sleep(3)
        if resume_errors:
            raise RuntimeError(f"failed to resume replicas: {resume_errors}")


def measure(case, rate, duration, phase):
    scenario, flags, _ = CASES[case]
    tag = f"{phase}-{case}"
    with topology(case) as lsn:
        command = [
            "python3",
            str(RPS / "campaign.py"),
            "--scenario",
            scenario,
            "--tag",
            tag,
            "--rate",
            str(rate),
            "--duration",
            str(duration),
            "--lsn",
            lsn,
            *flags,
        ]
        (RESULTS / "current.json").write_text(
            json.dumps(
                {
                    "case": case,
                    "phase": phase,
                    "rate": rate,
                    "duration": duration,
                    "started": time.time(),
                },
                indent=2,
            )
            + "\n"
        )
        print(f"START {tag} {rate} RPS {duration}s", flush=True)
        before = (
            (RESULTS / "index.jsonl").stat().st_size
            if (RESULTS / "index.jsonl").exists()
            else 0
        )
        completed = subprocess.run(command, check=False)
        if (
            completed.returncode not in (0, 1)
            or (RESULTS / "index.jsonl").stat().st_size <= before
        ):
            raise RuntimeError(
                f"measurement failed before producing a report: {command}"
            )
        latest = json.loads(
            (RESULTS / "index.jsonl").read_text().splitlines()[-1]
        )
        assert latest["tag"] == tag and latest["offered_rps"] == rate
        return latest


def search(case):
    if case == "new-connection":
        grid, step = [2000, 5000, 10000, 15000, 20000, 30000], 1000
    elif case == "hosts":
        grid, step = [10000, 20000, 30000, 40000, 50000], 2500
    else:
        grid, step = [30000, 45000, 55000, 65000], 2500
    passed, failed = 0, None
    attempts = []
    for rate in grid:
        result = measure(case, rate, 30, "search")
        attempts.append(result)
        if result["pass"]:
            passed = rate
        else:
            failed = rate
            break
    if failed is None:
        raise RuntimeError(f"extend search above {passed} for {case}")
    while failed - passed > step:
        rate = ((passed + failed) // 2 // step) * step
        if rate <= passed:
            break
        result = measure(case, rate, 30, "search")
        attempts.append(result)
        if result["pass"]:
            passed = rate
        else:
            failed = rate
    if not passed:
        raise RuntimeError(f"no passing rate for {case}")
    return {
        "candidate": passed,
        "failed_above": failed,
        "step": step,
        "search": attempts,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("search", "confirm"))
    parser.add_argument(
        "--cases", nargs="+", choices=CASES, default=list(CASES)
    )
    args = parser.parse_args()
    path = RESULTS / "suite.json"
    data = json.loads(path.read_text()) if path.exists() else {}
    if args.phase == "search":
        for case in args.cases:
            data[case] = search(case)
            path.write_text(json.dumps(data, indent=2) + "\n")
    else:
        # Interleave scenarios so a monotonic change in the VM cannot favor one.
        for repeat in range(1, 4):
            for case in args.cases:
                entry = data[case]
                entry.setdefault("confirmation", []).append(
                    measure(
                        case,
                        entry["candidate"],
                        CASES[case][2],
                        f"confirm{repeat}",
                    )
                )
                path.write_text(json.dumps(data, indent=2) + "\n")
        for case in args.cases:
            entry = data[case]
            while not all(
                r["pass"]
                for r in entry["confirmation"]
                if r["offered_rps"] == entry["candidate"]
            ):
                entry["candidate"] -= entry["step"]
                if entry["candidate"] <= 0:
                    raise RuntimeError(f"no confirmed candidate: {case}")
                for repeat in range(1, 4):
                    entry["confirmation"].append(
                        measure(
                            case,
                            entry["candidate"],
                            CASES[case][2],
                            f"reconfirm{repeat}",
                        )
                    )
                    path.write_text(json.dumps(data, indent=2) + "\n")
            entry["confirmed"] = entry["candidate"]
            path.write_text(json.dumps(data, indent=2) + "\n")
    (RESULTS / "current.json").write_text(
        json.dumps({"phase": args.phase, "finished": time.time()}) + "\n"
    )


if __name__ == "__main__":
    main()
