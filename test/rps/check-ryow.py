"""Check real INSERT -> committed LSN -> HTTP selection -> SQL read paths."""

import http.client
import json
import time
from collections import Counter

import psycopg2


def main():
    connections = {
        host: psycopg2.connect(host=host, user="postgres", dbname="postgres")
        for host in ("127.0.0.1", "127.0.0.2", "127.0.0.3")
    }
    for connection in connections.values():
        connection.autocommit = True
    cursors = {host: conn.cursor() for host, conn in connections.items()}
    primary = cursors["127.0.0.1"]
    primary.execute(
        "CREATE TABLE IF NOT EXISTS bench_ryow (id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, payload text)"
    )
    client = http.client.HTTPConnection("127.0.0.1", 8000, timeout=5)

    def get(path):
        client.request("GET", path)
        response = client.getresponse()
        body = response.read().decode()
        assert response.status == 200, (path, response.status, body)
        return body

    def write():
        assert get("/master") == "127.0.0.1"
        primary.execute(
            "INSERT INTO bench_ryow(payload) VALUES ('visible') RETURNING id"
        )
        row = primary.fetchone()[0]
        primary.execute("SELECT pg_current_wal_lsn()")
        return row, primary.fetchone()[0]

    def read(row, lsn):
        host = get("/replica?min_lsn=" + lsn)
        cursors[host].execute(
            "SELECT payload FROM bench_ryow WHERE id=%s", (row,)
        )
        assert cursors[host].fetchone() == ("visible",), (host, row, lsn)
        return host

    result = {}
    selected = Counter()
    for _ in range(1000):
        selected[read(*write())] += 1
    result["immediate_after_commit"] = dict(selected)
    try:
        for host in ("127.0.0.2", "127.0.0.3"):
            cursors[host].execute("SELECT pg_wal_replay_pause()")
        time.sleep(0.1)
        selected = Counter()
        for _ in range(100):
            host = read(*write())
            assert host == "127.0.0.1"
            selected[host] += 1
        result["both_replicas_paused"] = dict(selected)
    finally:
        resume_errors = []
        for host in ("127.0.0.2", "127.0.0.3"):
            try:
                for attempt in range(10):
                    try:
                        cursors[host].execute("SELECT pg_wal_replay_resume()")
                        break
                    except psycopg2.errors.SerializationFailure:
                        # Queued vacuum WAL can cancel the resume SELECT.
                        if attempt == 9:
                            raise
                        time.sleep(0.1)
            except psycopg2.Error as error:
                resume_errors.append(f"{host}: {error}")
        if resume_errors:
            raise RuntimeError(f"failed to resume replicas: {resume_errors}")
    row, lsn = write()
    deadline = time.monotonic() + 10
    while get("/replica?min_lsn=" + lsn) == "127.0.0.1":
        assert time.monotonic() < deadline
        time.sleep(0.05)
    selected = Counter()
    for _ in range(1000):
        host = read(row, lsn)
        assert host != "127.0.0.1"
        selected[host] += 1
    result["replayed_committed_lsn"] = {"lsn": lsn, "selected": dict(selected)}
    result["passed"] = True
    print(json.dumps(result, indent=2))
    client.close()
    for connection in connections.values():
        connection.close()


if __name__ == "__main__":
    main()
