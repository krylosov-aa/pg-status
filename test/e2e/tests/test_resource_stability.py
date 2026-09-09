"""Bounded mixed HTTP/polling load detects runaway resource accumulation."""

import os
import time
from concurrent.futures import ThreadPoolExecutor

from support.compose import ComposeProject
from support.faults import FaultController
from support.monitor import MonitorClient
from support.wal import WalWriter

_RESOURCE_COMMAND = (
    "ls /proc/1/fd | wc -l; "
    "sed -n 's/^VmRSS:[[:space:]]*\\([0-9]*\\).*/\\1/p' /proc/1/status"
)


def _resources(compose: ComposeProject) -> tuple[int, int]:
    output = compose.invoke(
        "exec",
        "-T",
        "pg-status",
        "sh",
        "-c",
        _RESOURCE_COMMAND,
        capture=True,
    ).stdout.splitlines()
    return int(output[0]), int(output[1])


def test_resources_stay_bounded_during_mixed_load(
    compose: ComposeProject,
    monitor: MonitorClient,
    faults: FaultController,
    wal: WalWriter,
) -> None:
    duration = float(os.environ.get("PG_STATUS_E2E_SOAK_SECONDS", "10"))
    if duration < 1:
        raise ValueError("PG_STATUS_E2E_SOAK_SECONDS must be at least 1")
    wal.warm_up()
    paths = ("/hosts", "/status?host=pg-proxy-1", "/ready", "/live") * 4
    # Exercise allocation paths before recording a steady-state baseline.
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(monitor.text, paths * 4))
        initial = _resources(compose)
        samples = [initial]
        deadline = time.monotonic() + duration
        fault_active = False
        while time.monotonic() < deadline:
            list(pool.map(monitor.text, paths))
            faults.route(
                "pg-proxy-3", None if not fault_active else "replica-2"
            )
            fault_active = not fault_active
            wal.warm_up()
            samples.append(_resources(compose))
            time.sleep(0.2)
    compose.save_diagnostics(
        "resource-samples",
        "fd_count,rss_kib\n"
        + "\n".join(f"{descriptors},{rss}" for descriptors, rss in samples),
    )
    # Room for transient sockets and sanitizer quarantine, not an RPS claim.
    assert max(row[0] for row in samples) <= initial[0] + 8
    assert max(row[1] for row in samples) <= initial[1] + 64 * 1024
