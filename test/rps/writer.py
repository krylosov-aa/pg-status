"""Generate committed WAL at a fixed, modest rate; publish the latest LSN.

Run on the PostgreSQL vCPU. This is background database traffic, not part of
the measured HTTP RPS. The LSN is read after the UPDATE transaction commits.
"""

import argparse
import json
import os
import signal
import time
from pathlib import Path

import psycopg2


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--rate", type=int, default=100)
    args = parser.parse_args()
    connection = psycopg2.connect(
        host="127.0.0.1", user="postgres", dbname="postgres"
    )
    connection.autocommit = True
    cursor = connection.cursor()
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    started = time.monotonic()
    count = 0
    target = args.state / "latest-lsn.txt"
    temporary = target.with_suffix(".tmp")
    with (args.state / "writer.jsonl").open("a") as log:
        while running:
            cursor.execute(
                "UPDATE bench_wal SET version=version+1 WHERE id=%s",
                (count % 100 + 1,),
            )
            cursor.execute("SELECT pg_current_wal_lsn()")
            lsn = cursor.fetchone()[0]
            temporary.write_text(lsn + "\n")
            os.replace(temporary, target)
            count += 1
            if count % args.rate == 0:
                log.write(
                    json.dumps(
                        {
                            "time": time.time(),
                            "commits": count,
                            "elapsed": time.monotonic() - started,
                            "lsn": lsn,
                        }
                    )
                    + "\n"
                )
                log.flush()
            time.sleep(max(0, started + count / args.rate - time.monotonic()))
        log.write(
            json.dumps(
                {
                    "stopped": time.time(),
                    "commits": count,
                    "elapsed": time.monotonic() - started,
                }
            )
            + "\n"
        )
    connection.close()


if __name__ == "__main__":
    main()
