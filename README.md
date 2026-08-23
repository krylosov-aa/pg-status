# pg-status

An extremely lightweight and fast sidecar service that reports the status of
your PostgreSQL hosts: whether they are alive, which host is the master, which
hosts are replicas, and how far each replica is lagging behind the master.

pg-status is designed to run alongside your main application. It is
resource-efficient and fast enough to query on every request without
noticeable overhead. However, it can also be deployed as a standalone service,
allowing multiple instances of your main application to share a single
pg-status instance.

It polls database hosts in the background at a configurable interval and
exposes an HTTP API for retrieving hosts that meet specific conditions.

All responses are served directly from memory.

To learn why this project exists and what problem it solves, read
[Three PostgreSQL Master/Replica Discovery Problems](docs/why.md).

## Usage

Run pg-status alongside your main service or on any host that can reach the
PostgreSQL servers. The HTTP API becomes available after the initial status
check of every configured host has completed.

### API

The service provides several HTTP endpoints for retrieving host information.

Host-selection endpoints support two response formats: plain text and JSON.
These endpoints are `/master`, `/replica`, `/sync_by_*`, and
`/most_sync_by_bytes`.

Include the `Accept: application/json` header to receive JSON, for example:
`{"host": "localhost"}`.

Without this header, the response is plain text: `localhost`.

The `/hosts` and `/status` endpoints always return JSON, while `/version`
always returns plain text.

If a host-selection endpoint cannot find a matching host, it returns HTTP 404.
The response body is empty in plain-text mode and `{"host": null}` in JSON
mode.

#### Lag query parameters

The `/replica` and `/sync_by_*` endpoints accept optional `lag_ms` and
`lag_bytes` query parameters that override the lag thresholds for a single
request. `/most_sync_by_bytes` accepts the same parameters but considers only
`lag_bytes`; `lag_ms` has no effect. Values must be non-negative integers;
otherwise, the endpoint responds with HTTP 400 and a body such as
`{"error_text": "Invalid lag_ms"}`.

The meaning of an omitted parameter depends on the route:

- `/replica` — a missing parameter means **no constraint on that
  dimension**. The global `pg_status__sync_max_lag_*` defaults are not
  applied here.
- `/sync_by_*` — a missing parameter falls back to the corresponding global
  `pg_status__sync_max_lag_ms` or `pg_status__sync_max_lag_bytes` value.
- `/most_sync_by_bytes` — a missing `lag_bytes` falls back to
  `pg_status__sync_max_lag_bytes`; `lag_ms` is always ignored.

A `/sync_by_time` request considers only the time threshold, while
`/sync_by_bytes` and `/most_sync_by_bytes` requests consider only the byte
threshold. Passing the other parameter to these endpoints has no effect.

#### LSN query parameter (read-your-writes)

The `/replica`, `/sync_by_*`, and `/most_sync_by_bytes` endpoints also accept
an optional `min_lsn` query parameter: a strict freshness filter that
guarantees the chosen replica has replayed through a given WAL position. This
is the basis for read-your-writes consistency. Instead of
relying on `lag_ms` and `lag_bytes` heuristics, the caller supplies an exact
LSN, and pg-status returns only a replica that has caught up to it.

The value must be a PostgreSQL LSN in canonical `HEX/HEX` form (for
example, `0/3000060`). An invalid format produces HTTP 400 with
`{"error_text": "Invalid min_lsn"}`. An omitted parameter means there is no
LSN constraint. When provided, it is combined with the lag parameters
described above.

If no replica has replayed to `min_lsn`, the master is returned as a
fallback.

**Read-your-writes pattern.** After writing to the master, capture
`pg_current_wal_lsn()` and pass it to the next read request:

```text
INSERT INTO ...;
SELECT pg_current_wal_lsn();   -- returns e.g. "0/3000060"

# The following read is guaranteed to see the write:
GET /replica?min_lsn=0/3000060
```

Either a replica whose replay LSN is at or beyond `0/3000060` is returned, or
the master is returned.

#### `GET /master`

Returns the current master's host name. If no master is available, the endpoint
returns HTTP 404 as described above.

#### `GET /replica`

Returns the host name of a replica, selected using round-robin.
Optional `lag_ms`, `lag_bytes`, and `min_lsn` query parameters constrain
the result:

- No parameters — any live replica.
- `?lag_ms=X` — live replicas with `lag_ms ≤ X`.
- `?lag_bytes=Y` — live replicas with `lag_bytes ≤ Y`.
- `?lag_ms=X&lag_bytes=Y` — live replicas with both `lag_ms ≤ X` and
  `lag_bytes ≤ Y`.
- `?min_lsn=X/Y` — live replicas whose replay LSN is at or beyond the given
  value (see "LSN query parameter" above). This constraint can be combined
  with the lag filters.

If no replica matches, the master's host name is returned instead.

#### `GET /sync_by_time`

Returns the host name of a replica, selected using round-robin, whose time lag
is less than or equal to the threshold. The threshold is taken from the
`lag_ms` query parameter when provided; otherwise,
`pg_status__sync_max_lag_ms` is used. If no replica meets this condition, the
master's host name is returned.

#### `GET /sync_by_bytes`

Returns the host name of a replica, selected using round-robin, whose WAL lag
in bytes is less than or equal to the threshold. The threshold is taken from
the `lag_bytes` query parameter when provided; otherwise,
`pg_status__sync_max_lag_bytes` is used. If no replica meets this condition,
the master's host name is returned.

#### `GET /sync_by_time_or_bytes`

Returns the host name of a replica, selected using round-robin, that is
synchronous either by time or by bytes. The `lag_ms` and `lag_bytes` query
parameters override the corresponding global thresholds for the current
request. If no such replica exists, the master's host name is returned.

#### `GET /sync_by_time_and_bytes`

Returns the host name of a replica, selected using round-robin, that is
synchronous by both time and bytes. The `lag_ms` and `lag_bytes` query
parameters override the corresponding global thresholds for the current
request. If no such replica exists, the master's host name is returned.

#### `GET /most_sync_by_bytes`

Returns the host name of the replica with the smallest `lag_bytes` among those
that satisfy the byte threshold and the optional `min_lsn` constraint. Unlike
the `/sync_by_*` endpoints, this endpoint does not use round-robin: selection
is deterministic, and ties are resolved by host order.

The `lag_bytes` query parameter overrides `pg_status__sync_max_lag_bytes` for
the current request. Neither `lag_ms` nor `pg_status__sync_max_lag_ms` is
considered.

If no replica satisfies the byte and LSN constraints, the master's host name
is returned.

#### `GET /hosts`

Returns a JSON list containing status information for every configured host.
The `sync_by_time` and `sync_by_bytes` flags indicate whether the current lag
is within the global `pg_status__sync_max_lag_*` thresholds. For a dead host
(`alive: false`), the lag fields and `lsn` are `null`, and the sync flags are
`false`.

The `lsn` field is the host's latest known WAL position as of the last
successful poll: `pg_current_wal_lsn()` on the master,
`pg_last_wal_replay_lsn()` on a replica. It is `null` for dead hosts.

Example:

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

#### `GET /status`

Returns the status of the host specified by the `host` query parameter.
If the `host` parameter is missing, the endpoint responds with HTTP 400 and
`{"error_text": "Get parameter 'host' wasn't passed"}`. If the host is not in
the monitored list, the endpoint returns HTTP 404.

You can also use this endpoint to check whether a configured host is currently
alive.

Example: `http://127.0.0.1:8000/status?host=host-1`

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

#### `GET /version`

Returns the pg-status semantic version as plain text.

### Parameters

Configure pg-status using the following environment variables:

- `pg_status__hosts` — Comma-separated list of PostgreSQL hosts. Required.
- `pg_status__pg_user` — PostgreSQL user. Default: `postgres`.
- `pg_status__pg_password` — PostgreSQL password. Default: `postgres`.
- `pg_status__pg_database` — PostgreSQL database name. Default: `postgres`.
- `pg_status__pg_port` — PostgreSQL port. To use a different port for each
  host, provide a comma-separated list in the same order as
  `pg_status__hosts`. A single value applies to every host. Default: `5432`.
- `pg_status__connect_timeout` — Time limit, in seconds, for establishing a
  PostgreSQL connection. Default: `2`.
- `pg_status__max_fails` — Number of consecutive failed checks before a host
  is considered dead. Default: `3`.
- `pg_status__sleep_ms` — Delay, in milliseconds, between consecutive checks
  of a host. Default: `5000`.
- `pg_status__query_timeout_ms` — Hard deadline, in milliseconds, for one poll
  iteration (connect, send, and read). When an iteration times out, its
  connection is closed and the host's failure counter is incremented.
  Default: `5000`.
- `pg_status__conn_max_age_ms` — Maximum age, in milliseconds, of a reused
  PostgreSQL connection. Older connections are closed after the current
  iteration and reopened for the next one. Default: `300000` (5 minutes).
- `pg_status__sync_max_lag_ms` — Maximum time lag, in milliseconds, for a
  replica to be considered time-synchronous. Default: `1000`.
- `pg_status__sync_max_lag_bytes` — Maximum WAL lag, in bytes, for a replica
  to be considered byte-synchronous. Default: `1000000` (1 MB).
- `pg_status__http_listen_address` — IP address on which the HTTP server
  listens. Accepts an IPv4 address, an IPv6 address, or `*` for best-effort
  IPv4/IPv6 wildcard listeners. Default: `0.0.0.0`.
- `pg_status__http_port` — HTTP server port. Default: `8000`.
- `pg_status__log_level` — Minimum logging level. Accepts `debug`, `info`,
  `warning` (or `warn`), `error`, or `fatal`. Default: `info`.

## Installation

Available installation options:

- [Debian package](https://github.com/krylosov-aa/pg-status/releases/download/2.2.0/pg-status_2.2.0_amd64.deb)
- [Docker Hub image](https://hub.docker.com/r/krylosovaa/pg-status)
- [Docker build configurations](docker)
- [Statically linked binary](https://github.com/krylosov-aa/pg-status/releases/download/2.2.0/pg-status_2.2.0_linux_amd64_static.tar.gz)
- [Dynamically linked binary](https://github.com/krylosov-aa/pg-status/releases/download/2.2.0/pg-status_2.2.0_linux_amd64_shared.tar.gz)

For more information, see the [installation guide](docs/installation.md).

## Quick demo

The demo requires Docker with Docker Compose.

To build pg-status and start a ready-to-use PostgreSQL topology, run:

```sh
make build_up_test
```

This starts pg-status, one PostgreSQL primary, two physical streaming
replicas, and proxy services used to simulate role changes. After the
containers become healthy and the initial host checks complete, query the API
at `http://127.0.0.1:8000`:

```sh
curl http://127.0.0.1:8000/master
curl http://127.0.0.1:8000/replica
curl http://127.0.0.1:8000/hosts
```

Switch the simulated master and query pg-status again after the next polling
cycle:

```sh
make 2-master
curl http://127.0.0.1:8000/master
```

Restore the original topology or stop the demo with:

```sh
make 1-master
make down_test
```

See [test/README.md](test/README.md) for details about the topology and project
testing.

## Performance

Approximate memory usage: 9 MiB.

Performance depends on the endpoint and response format. A plain-text
`/master` response is the fastest, while the JSON returned by `/hosts` is the
slowest:

- 0.1 CPU core — approximately 1,600–2,000 requests/s
- 1 CPU core — approximately 8,600–9,000 requests/s

See the [detailed performance reports](docs/performance.md).

## Implementation details

### Concurrent polling

A single writer thread polls **all hosts concurrently** using libpq's
non-blocking API and one `poll()` system call across their sockets. Each host
has an independent polling cycle: a new check starts `pg_status__sleep_ms`
after the previous one finishes. Every iteration has a hard deadline of
`pg_status__query_timeout_ms`; when that deadline expires, the connection is
closed and the host's failure counter is incremented.

A slow or unresponsive host therefore does not block updates for the other
hosts. The rest of the cluster continues to refresh independently while the
slow host waits for its deadline.

### Connection reuse

PostgreSQL connections are kept alive between polling iterations. Opening a
new `PGconn` for every iteration would require a full TCP and authentication
handshake every `pg_status__sleep_ms`, adding unnecessary load to the server.
Instead, each host keeps its connection open and reuses it.

To prevent stale connections from persisting indefinitely, for example after
intermediate NAT state expires or server-side cleanup occurs, each connection
is recycled when its age exceeds `pg_status__conn_max_age_ms`. Connections are
also closed and reopened after any query error or socket-level failure.

### Consistency

Cross-host consistency is intentionally not provided.

There is one writer (the polling thread) and many readers (HTTP handlers). The
writer never blocks the readers, and the readers never block the writer.

Each host's data is published as a consistent snapshot through a seqlock, so
readers always see fields from the same poll iteration for that host.

Cross-host inconsistency is permitted by design: at any moment, some hosts may
contain newer snapshots than others. There is no global polling barrier; the
freshness of each snapshot depends on that host's independent polling schedule
and query deadline. Fast responses and up-to-date per-host data are more
important for this project than a consistent view across all hosts.

### Reaction speed to host unavailability

If a host does not respond to a status check, the cause may be either a
temporary issue or an actual outage. To avoid marking a host as dead
prematurely, pg-status waits for `pg_status__max_fails` consecutive failed
checks. A failed iteration is aborted after at most
`pg_status__query_timeout_ms`. The worst-case detection time is approximately:

```text
max_fails × query_timeout_ms + (max_fails − 1) × sleep_ms
```

After the first failed check, but before the failure count reaches
`pg_status__max_fails`, the host is marked as possibly dead. This state affects
host selection:

- If the current master is marked as possibly dead and a new master has
  already been detected, pg-status immediately switches to the new master.
- When selecting a replica, pg-status prefers fully responsive hosts. If no
  such replica meets the search criteria, it returns a possibly dead replica.
  Consequently, load-balancing fairness may be temporarily reduced while a
  host remains in this state.

### Split-brain

With client-side master detection, pg-status cannot determine which host
*should* be the master during a split-brain scenario. The first live master in
`pg_status__hosts` wins.

## Logging

The service writes thread-safe, single-line logs to stderr. Calls from the HTTP
and monitor threads enqueue records in a bounded in-memory queue; a dedicated
logging thread writes them in queue order. Every line contains an RFC 3339 UTC
timestamp, severity, component, and message:

```text
2026-08-21T12:34:56.123Z INFO http: server started address=0.0.0.0 port=8000
2026-08-21T12:35:01.245Z WARNING monitor: host state changed host=pg-2 state=possible_dead
```

The default `info` level reports startup, shutdown, role changes, availability
changes, and transitions across the global `pg_status__sync_max_lag_*`
thresholds. PostgreSQL operation failures are logged at `error`. Detailed polling
progress and replica-to-master fallback decisions are available at `debug`.

pg-status never logs PostgreSQL passwords or complete connection strings. Log
collection, storage, and rotation are delegated to the process supervisor,
Docker, or the operating system.

## Third-party components

pg-status uses the following third-party components:

- libevent —
  [BSD 3-Clause License](https://github.com/libevent/libevent/blob/master/LICENSE)
- cJSON —
  [MIT License](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)
- libpq —
  [PostgreSQL License](https://www.postgresql.org/about/licence/)
