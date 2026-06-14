# pg-status

An extremely lightweight and fast microservice (sidecar) that helps instantly determine the status of your PostgreSQL
hosts including whether they are alive, which one is the master, which ones are replicas, and how far each
replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.

pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.

To learn more about why this project exists and
what problem it solves, you can read [the article about three PostgreSQL Master/Replica Discovery Problems](docs/why.md)


# Usage

Run the application on the same host next to the main service or actually anywhere you want.
After it starts, the HTTP API will be available.

## API

The service provides several HTTP endpoints for retrieving host information.

All APIs support two response formats: plain text and JSON.

If you include the header `Accept: application/json`, the response will be in JSON format, for example: `{"host": "localhost"}`

If you omit this header, the response will be plain text: `localhost`

If the API cannot find a matching host, it will return a 404 status code.
In this case, the response body will be empty for plain text mode, and `{"host": null}` for json mode.


### Lag query parameters

The `/replica`, `/sync_by_*`, and `/most_sync_by_bytes` endpoints accept
optional query parameters `lag_ms` and `lag_bytes` that override the lag
thresholds for a single request. Values must be non-negative integers;
otherwise the endpoint responds with HTTP 400 and a body like
`{"error_text": "Invalid lag_ms"}`.

How a missing parameter is interpreted depends on the route:

- `/replica` — a missing parameter means **no constraint on that
  dimension**. The global `pg_status__sync_max_lag_*` defaults are not
  applied here.
- `/sync_by_*` and `/most_sync_by_bytes` — a missing parameter falls back
  to the global `pg_status__sync_max_lag_ms` /
  `pg_status__sync_max_lag_bytes`.

A `/sync_by_time` request only consults the time threshold and a
`/sync_by_bytes` request only consults the byte threshold; passing the
other parameter to those endpoints is silently ignored.

### LSN query parameter (read-your-writes)

The `/replica`, `/sync_by_*`, and `/most_sync_by_bytes` endpoints also
accept an optional `min_lsn` query parameter — a strict freshness
filter that guarantees the chosen host has replayed at least up to a
given WAL position. This is the primitive for read-your-writes
consistency: instead of relying on `lag_ms` / `lag_bytes` heuristics,
the caller supplies an exact LSN and pg-status only returns a replica
that has caught up to it.

The value must be a PostgreSQL LSN in canonical `HEX/HEX` form (for
example, `0/3000060`). An invalid format produces HTTP 400 with
`{"error_text": "Invalid min_lsn"}`. A missing parameter means no LSN
constraint and stacks with the lag parameters described above.

If no replica has replayed to `min_lsn`, the master is returned as a
fallback.

**Read-your-writes pattern.** After a write to the master, capture
`pg_current_wal_lsn()` and pass it to the next read request:

```
INSERT INTO ...;
SELECT pg_current_wal_lsn();   -- returns e.g. "0/3000060"

# follow-up read is guaranteed to see the write:
GET /replica?min_lsn=0/3000060
```

Either a replica that has already replayed at least to `0/3000060` is
returned, or the master is returned.

### `GET /master`

Returns the host of the current master, if one exists. If no master is available, it returns null.

### `GET /replica`

Returns the host of a replica, selected using the round-robin algorithm.
Optional `lag_ms`, `lag_bytes`, and `min_lsn` query parameters constrain
the result:

- no parameters — any alive replica.
- `?lag_ms=X` — alive replicas with `lag_ms ≤ X`.
- `?lag_bytes=Y` — alive replicas with `lag_bytes ≤ Y`.
- `?lag_ms=X&lag_bytes=Y` — alive replicas with both `lag_ms ≤ X` AND `lag_bytes ≤ Y`.
- `?min_lsn=X/Y` — alive replicas that have replayed at least up to the given LSN (see "LSN query parameter" above). Composes with the lag filters.

If no replica matches, the master’s host is returned instead.

### `GET /sync_by_time`

Returns the host of a replica (selected using the round-robin algorithm) considered time-synchronous — its time lag is less than or equal to the threshold.
The threshold is the `lag_ms` query parameter if provided, otherwise `pg_status__sync_max_lag_ms`.
If no replica meets this condition, the master’s host is returned.

### `GET /sync_by_bytes`

Returns the host of a replica (selected using the round-robin algorithm) considered byte-synchronous — according to the WAL LSN, its lag is less than or equal to the threshold.
The threshold is the `lag_bytes` query parameter if provided, otherwise `pg_status__sync_max_lag_bytes`.
If no replica meets this condition, the master’s host is returned.

### `GET /sync_by_time_or_bytes`

Returns the host of a replica (selected using the round-robin algorithm) that is considered synchronous either by time or by bytes.
Per-request `lag_ms` / `lag_bytes` query parameters override the corresponding global thresholds for this request only.
If no such replica exists, the master’s host is returned.

### `GET /sync_by_time_and_bytes`

Returns the host of a replica (selected using the round-robin algorithm) that is considered synchronous by both time and bytes.
Per-request `lag_ms` / `lag_bytes` query parameters override the corresponding global thresholds for this request only.
If no such replica exists, the master’s host is returned.

### `GET /most_sync_by_bytes`

Returns the host of the most byte-synchronous replica — the one with the smallest `lag_bytes` among replicas that still satisfy the time threshold. Unlike the `/sync_by_*` endpoints, this endpoint does not use round-robin: selection is deterministic (ties are broken by host order).
Per-request `lag_bytes` query parameter overrides the global threshold for this request only.
If no replica satisfies threshold, the master’s host is returned.

### `GET /hosts`

Returns a list of all hosts with their status information in json format.
The `sync_by_time` / `sync_by_bytes` flags reflect the current lag against
the global `pg_status__sync_max_lag_*` thresholds. For a dead host
(`alive: false`), the lag fields are `null` and the sync flags are omitted.

The `lsn` field is the host's latest known WAL position as of the last
successful poll: `pg_current_wal_lsn()` on the master,
`pg_last_wal_replay_lsn()` on a replica. It is `null` for dead hosts.

For example:
```json
[
  {
    "host": "host-1",
    "master": true,
    "alive": true,
    "lag_ms": 0,
    "sync_by_time": true,
    "lag_bytes": 0,
    "sync_by_bytes": true,
    "lsn": "0/3000060"
  },
  {
    "host": "host-2",
    "master": false,
    "alive": true,
    "lag_ms": 6193,
    "sync_by_time": false,
    "lag_bytes": 456,
    "sync_by_bytes": true,
    "lsn": "0/2FFFE98"
  },
  {
    "host": "host-3",
    "master": false,
    "alive": false,
    "lag_ms": null,
    "sync_by_time": false,
    "lag_bytes": null,
    "sync_by_bytes": false,
    "lsn": null
  }
]
```

### `GET /status`

Returns status of a host that you specified in the `host` query parameter.
If the `host` parameter is missing, the endpoint responds with HTTP 400 and
`{"error_text": "Get parameter 'host' wasn't passed"}`. If the host is not in
the monitored list, a 404 is returned.

You can also use this API to poll pg-status to check if it has started up and is alive.

For example: `http://127.0.0.1:8000/status?host=host-1`
```json
{
  "master": false,
  "alive": true,
  "lag_ms": 0,
  "sync_by_time": true,
  "lag_bytes": 0,
  "sync_by_bytes": true,
  "lsn": "0/3000060"
}
```

### `GET /version`

Returns the pg-status semver


### Parameters

You can configure various parameters using environment variables:

- `pg_status__pg_user` — The user under which SQL queries to PostgreSQL will be executed. Default: `postgres`
- `pg_status__pg_password` — The password for the PostgreSQL user. Default: `postgres`
- `pg_status__pg_database` — The name of the database to connect to. Default: `postgres`
- `pg_status__hosts` — A list of PostgreSQL hosts, separated by the `,`.
- `pg_status__pg_port` — The connection port. You can specify separate ports for individual hosts using the same delimiter. Default: `5432`
- `pg_status__connect_timeout` — The time limit (in seconds) for establishing a connection to PostgreSQL. Default: `2`
- `pg_status__max_fails` — The number of consecutive errors allowed when checking a host’s status before it is considered dead. Default: `3`
- `pg_status__sleep_ms` — The delay (in milliseconds) between consecutive host status checks. Default: `5000`
- `pg_status__query_timeout_ms` — Hard deadline (in milliseconds) for a single poll iteration of a host (connect + send + read). If the iteration does not complete in time, the connection is closed and the host's failure counter is incremented. Default: `5000`
- `pg_status__conn_max_age_ms` — Maximum age (in milliseconds) of a reused PostgreSQL connection. Connections older than this are closed and reopened on the next iteration so that stale connections do not stick around forever. Default: `300000` (5 minutes)
- `pg_status__sync_max_lag_ms` — The maximum acceptable replication lag (in milliseconds) for a replica to still be considered time-synchronous. Default: `1000`
- `pg_status__sync_max_lag_bytes` — The maximum acceptable lag (in bytes) for a replica to still be considered byte-synchronous. Default: `1000000` (1 MB)
- `pg_status__http_port` — the port on which the http server will listen. Default: `8000`


# Installation

In short there is:
- [deb package](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_amd64.deb)
- [published container to Docker Hub](https://hub.docker.com/r/krylosovaa/pg-status)
- various [docker containers](docker)
- [static binary](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_static.tar.gz)
- [shared binary](https://github.com/krylosov-aa/pg-status/releases/download/2.1.1/pg-status_2.1.1_linux_amd64_shared.tar.gz)


For more information, go to the [docs/installation.md](docs/installation.md) section.

# Performance

Memory - 9Mib

Depending on the API being called and the format selected
(plain `/master` is the fastest, json `/hosts` is the slowest):

- 0.1 CPU — Requests/sec: ~1600-2000
- 1 CPU — Requests/sec: ~8600-9000

[Detailed performance reports](docs/performance.md)

# Implementation Details

## Concurrent polling

A single writer thread polls **all hosts concurrently** through libpq's
non-blocking API and one `poll()` system call over their sockets. Each
host runs its own independent iteration: a new poll is started every
`pg_status__sleep_ms` after the previous one completed, and each
iteration has a hard deadline of `pg_status__query_timeout_ms` (after
which the connection is torn down and the host's failure counter is
incremented).

This means a slow or hung host doesn't block updates for the other
hosts — the rest of the cluster continues to refresh on its own cadence
while the slow host waits for its own deadline.

## Connection reuse

PostgreSQL connections are kept alive between polling iterations:
opening a fresh `PGconn` on every iteration would mean a full
TCP + auth handshake every `pg_status__sleep_ms`, which is wasteful and
adds needless load on the server. Instead, each host keeps its
connection open and reuses it.

To prevent a stale connection from sticking around forever
(e.g. intermediate NAT state expiring, or server-side cleanup), each
connection is recycled when its age exceeds
`pg_status__conn_max_age_ms`. Connections are also closed and reopened
on any query error or socket-level failure.

## Consistency

Cross-host consistency is intentionally not provided.

There is one writer (the poll thread) and many readers (HTTP handlers).
The design goal is that the writer never blocks the readers and the
readers never block the writer.

Per-host data is published as a
consistent snapshot via a seqlock on each host, so readers always see
all fields from the same poll iteration of a single host.

But cross-host inconsistency is still permitted by design: at any given
moment some hosts may already have data from their N-th iteration while
others still have data from their (N−1)-th iteration. The size of this
window is bounded by `pg_status__query_timeout_ms`. For this project,
the fastest response time and the most up-to-date per-host data
mattered more, so cross-host consistency was intentionally sacrificed.


## Reaction speed to host unavailability

If a host doesn't respond to a request, it could mean either a temporary issue or that the host is actually down.
To avoid marking a host as dead prematurely, a host is only considered dead after `pg_status__max_fails` attempts.
A failing iteration aborts after at most `pg_status__query_timeout_ms`, so in the worst case a host transitions
from healthy to dead in roughly `pg_status__max_fails × pg_status__query_timeout_ms`.

To speed up our reaction to possible host unavailability, if a host doesn't respond and the number of attempts
hasn't yet exceeded `pg_status__max_fails`, we mark it as possibly dead, and this affects which hosts get returned:

- If the current master is marked as possibly dead and there’s already a new master, we immediately switch to the new master.
- When selecting a replica, preference is given to live hosts. However, if no live replicas meet a search criteria, a
potentially dead replica will be returned. This means that for up to `pg_status__max_fails` attempts, the
fairness of load balancing between replicas can be disrupted.

## Split-brain

With the client-side master detection approach, there’s a problem: in the case of a split-brain scenario,
we can’t reliably determine who "should" be master. In our case, the first alive master wins.
The order is defined by `pg_status__hosts`.


# Logging

The service writes to stdout and stderr. All errors, such as connection errors to pg hosts,
are written to stderr.

Informational messages about service startup and shutdown are written to stdout.

More importantly, information about host status changes is written to stdout:

If a host fails a status check but has not yet exceeded `pg_status__max_fails`, the message will be: `<host-name>: possible dead`

If a host is confirmed dead (after `pg_status__max_fails` consecutive failures), the message will be: `<host-name>: dead`

If a host is revived or becomes a master after failover, the message will be: `<host-name>: master`

If a host is revived or becomes a replica after failover, the message will be: `<host-name>: replica`

For replicas, there are also messages about replica synchronicity against the global `pg_status__sync_max_lag_*` thresholds:

```
<host-name>: synchronous in time
<host-name>: out of sync in time
<host-name>: synchronous in bytes
<host-name>: out of sync in bytes
```


# Testing the service

You can start the containers and test the application however you like.

### make build_up

Builds [the lightweight container]((docker/alpine/Dockerfile_shared)) using parameters defined in
[docker-compose.yml](docker-compose.yml).

You can create a `.env` file using [the provided example](.env_example), or specify the required
parameters directly in [docker-compose.yml](docker-compose.yml).
This allows you to test the application with your own database setup.

### make build_up_test

Builds [the lightweight container](docker/alpine/Dockerfile_shared)
with parameters defined in [test/docker-compose.yml](test/docker-compose.yml).

In addition to the main service, this setup launches two PostgreSQL instances: one acting as the master and the other as a replica.
To simulate host failover or disconnection, proxy services are used.
This approach allows you to test master-switch scenarios without actually stopping PostgreSQL — you can simply switch the proxy’s target instead.

Helper shell scripts are provided for this purpose:
- [test/pg-proxy-1_is_master.sh](test/pg-proxy-1_is_master.sh)
- [test/pg-proxy-2_is_master.sh](test/pg-proxy-2_is_master.sh)


# Third‑party components

It uses the following third‑party components:

- libmicrohttpd — licensed under [the GNU Lesser General Public License v2.1 or later](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- cJSON — licensed under [the MIT License](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)
- libpq — licensed under [the PostgreSQL License](https://www.postgresql.org/about/licence/)
