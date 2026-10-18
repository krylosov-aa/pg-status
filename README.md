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
PostgreSQL servers. The HTTP server starts at once, and `/live` and
`/version` answer immediately. `/ready` and all host-information endpoints
return HTTP 503 with `{"error_text": "pg_monitor is not ready"}`, regardless
of the `Accept` header and query parameters, until every configured host has
completed its first check, successfully or not. Initial connections
[take turns](#connection-reuse), so with dozens of hosts readiness can take
several seconds; size readiness probe timeouts accordingly.

### API

The host-selection endpoints are `/master`, `/replica`, `/sync_by_*`, and
`/most_sync_by_bytes`. They return the host name as plain text, such as
`localhost`, or as JSON, such as `{"host": "localhost", ...}`, when the
request has the `Accept: application/json` header. If no host matches, they
return HTTP 404 with an empty body or `{"host": null, ...}`. `/hosts` and
`/status` always return JSON, and `/version` returns plain text.

#### Lag query parameters

`/replica`, `/sync_by_*`, and `/most_sync_by_bytes` accept optional `lag_ms`
and `lag_bytes` query parameters that override the lag thresholds for one
request. Values must be non-negative integers; otherwise, the endpoint
responds with HTTP 400 and a body such as `{"error_text": "Invalid lag_ms"}`.

An omitted parameter means no constraint for `/replica`, and the global
`pg_status__sync_max_lag_ms` or `pg_status__sync_max_lag_bytes` for the
other endpoints. `/sync_by_time` uses only `lag_ms`, while `/sync_by_bytes`
and `/most_sync_by_bytes` use only `lag_bytes`; the other parameter has no
effect there.

A lag that pg-status cannot measure is `null` in [`/hosts`](#get-hosts) and
meets no threshold: only a request without a limit on that lag, such as
`/replica` without parameters, can return such a replica.

#### LSN query parameter (read-your-writes)

The same endpoints accept an optional `min_lsn` parameter, a PostgreSQL LSN
such as `0/3000060`. Only a replica that has replayed WAL up to this position
matches, which gives read-your-writes consistency without relying on lag
thresholds. An invalid value produces HTTP 400 with
`{"error_text": "Invalid min_lsn"}`. `min_lsn` is combined with the lag
parameters; if no replica matches, the master is returned.

WAL positions are comparable only within one WAL history. After a failover,
a replica may remain on the history that the new master abandoned, past
`min_lsn` but without the write. Therefore a replica whose timeline differs
from the master's never meets `min_lsn`, already when `/master` starts
returning the new master. Whether pg-status knows replica timelines depends
on the
[monitoring user's privileges](#postgresql-privileges).

The optional `timeline` parameter is the timeline of the write, a number from
1 to 4294967295; another value produces HTTP 400 with
`{"error_text": "Invalid timeline"}`. With it, only a replica known to be on
that timeline matches. This also protects reads when pg-status does not know
replica timelines, and during a failover before pg-status selects the new
master. Once pg-status selects a master on a later timeline, no replica
matches the earlier one, and such reads go to the master until the client
writes again. `timeline` is accepted wherever `min_lsn` is and can also be
used alone.

**Read-your-writes pattern.** After the write has been committed, read the
master's WAL insert position and timeline, and pass them with the next read:

```text
INSERT INTO ...;               -- in autocommit mode, or after COMMIT returns
SELECT pg_current_wal_insert_lsn(),
       ('x' || substr(pg_walfile_name(pg_current_wal_insert_lsn()), 1, 8))
         ::bit(32)::bigint;    -- returns e.g. "0/3000060" and 2

GET /replica?min_lsn=0/3000060&timeline=2
```

Either a replica on timeline 2 that has replayed `0/3000060` is returned, or
the master.

- Read the position only after COMMIT has returned: inside the transaction,
  it does not cover the commit record. Drivers that open transactions
  implicitly, such as Python DB-API, run the `SELECT` in the same transaction
  unless it follows the commit.
- Use `pg_current_wal_insert_lsn()`: `pg_current_wal_lsn()` may not cover an
  asynchronous commit (`synchronous_commit = off`).
- Take the timeline from the WAL file name as above, not from
  `pg_control_checkpoint()`, which shows the old timeline for a while after a
  promotion.

#### Locality-aware replica selection

When complete locality metadata is configured (see
[Parameters](#parameters)), `/replica` and the `/sync_by_*` endpoints choose
among the replicas that pass their filters in this order:

1. Replicas whose `dc` matches the current `dc`.
2. Otherwise, replicas whose `geo` matches the current `geo`.
3. Otherwise, all of them.

Round-robin applies within the chosen group. Locality never makes an
unsuitable replica eligible. `/most_sync_by_bytes` ignores locality.

#### `GET /master`

Returns the current master's host name. If no master is available, the endpoint
returns HTTP 404 as described above.

#### `GET /replica`

Returns the host name of a replica, selected using locality preference and
round-robin. Optional query parameters constrain the result:

- No parameters — any live replica.
- `?lag_ms=X` — live replicas with `lag_ms ≤ X`.
- `?lag_bytes=Y` — live replicas with `lag_bytes ≤ Y`.
- `?lag_ms=X&lag_bytes=Y` — live replicas with both `lag_ms ≤ X` and
  `lag_bytes ≤ Y`.
- `?min_lsn=X/Y` — live replicas that have replayed WAL up to the given
  position (see [LSN query parameter](#lsn-query-parameter-read-your-writes)).
- `?timeline=N` — live replicas known to be on timeline `N`, usually
  together with `min_lsn`.

The parameters can be combined. If no replica matches, the master's host name
is returned instead.

#### `GET /sync_by_time`

Returns the host name of a replica, selected using locality preference and
round-robin, whose time lag is less than or equal to the threshold:
`lag_ms`, or `pg_status__sync_max_lag_ms` by default. If no replica meets
this condition, the master's host name is returned.

#### `GET /sync_by_bytes`

Returns the host name of a replica, selected using locality preference and
round-robin, whose byte lag is less than or equal to the threshold:
`lag_bytes`, or `pg_status__sync_max_lag_bytes` by default. If no replica
meets this condition, the master's host name is returned.

#### `GET /sync_by_time_or_bytes`

Returns the host name of a replica, selected using locality preference and
round-robin, that is synchronous either by time or by bytes. If no such
replica exists, the master's host name is returned.

#### `GET /sync_by_time_and_bytes`

Returns the host name of a replica, selected using locality preference and
round-robin, that is synchronous by both time and bytes. If no such replica
exists, the master's host name is returned.

#### `GET /most_sync_by_bytes`

Returns the host name of the replica with the smallest `lag_bytes` among those
that meet the byte threshold and the optional `min_lsn` and `timeline`
constraints, or the master's host name if there is none. Locality and
round-robin are not used: ties are resolved by host order.

#### `GET /hosts`

Returns a JSON list with the status of every configured host:

- `host` — the host name from `pg_status__hosts`.
- `dc`, `geo` — the host's configured locality, or `null`.
- `master`, `alive`, `possible_dead` — the host's role and availability (see
  [Reaction speed](#reaction-speed-to-host-unavailability)).
- `lag_ms`, `lag_bytes` — the replica's [time lag](#time-lag) and
  [byte lag](#byte-lag); 0 on the master.
- `sync_by_time`, `sync_by_bytes` — whether these lags are within the global
  `pg_status__sync_max_lag_*` thresholds.
- `lsn` — the host's WAL position: `pg_current_wal_lsn()` on the master,
  `pg_last_wal_replay_lsn()` on a replica.

The values are as of the host's last successful check. For a dead host
(`alive: false`), the lags and `lsn` are `null`, and the sync flags are
`false`. A lag that cannot be measured is also `null`, with a `false` sync
flag.

Example:

```json
[
  {
    "host": "host-1",
    "dc": "frankfurt",
    "geo": "europe",
    "master": true,
    "alive": true,
    "possible_dead": false,
    "lag_ms": 0,
    "sync_by_time": true,
    "lag_bytes": 0,
    "sync_by_bytes": true,
    "lsn": "0/3000060"
  },
  {
    "host": "host-2",
    "dc": "amsterdam",
    "geo": "europe",
    "master": false,
    "alive": true,
    "possible_dead": false,
    "lag_ms": 6193,
    "sync_by_time": false,
    "lag_bytes": 456,
    "sync_by_bytes": true,
    "lsn": "0/2FFFE98"
  },
  {
    "host": "host-3",
    "dc": null,
    "geo": null,
    "master": false,
    "alive": false,
    "possible_dead": true,
    "lag_ms": null,
    "sync_by_time": false,
    "lag_bytes": null,
    "sync_by_bytes": false,
    "lsn": null
  }
]
```

#### `GET /status`

Returns the status of the host given by the `host` query parameter, in the
same format as `/hosts`, without the host name. Without the `host`
parameter, the endpoint responds with HTTP 400 and
`{"error_text": "Get parameter 'host' wasn't passed"}`. A host that is not
in the monitored list gives HTTP 404.

Example: `http://127.0.0.1:8000/status?host=host-2`

```json
{
  "dc": "amsterdam",
  "geo": "europe",
  "master": false,
  "alive": true,
  "possible_dead": false,
  "lag_ms": 6193,
  "sync_by_time": false,
  "lag_bytes": 456,
  "sync_by_bytes": true,
  "lsn": "0/2FFFE98"
}
```

#### `GET /version`

Returns the pg-status semantic version as plain text, including during startup.

#### `GET /live`

Returns HTTP 200 with the plain-text body `OK` as soon as the HTTP server is
running, regardless of monitor readiness or PostgreSQL availability.

#### `GET /ready`

Returns HTTP 200 with the plain-text body `OK` once every configured host has
completed its first check, and HTTP 503 before that. Readiness does not
require a live master or any live PostgreSQL host. Later PostgreSQL outages
do not make pg-status unready; they are reported through host status and
selection.

### Parameters

Configure pg-status using the following environment variables:

- `pg_status__hosts` — Comma-separated list of PostgreSQL hosts. Required.
- `pg_status__pg_user` — PostgreSQL user. Default: `postgres`.
- `pg_status__pg_password` — PostgreSQL password. Default: `postgres`.
- `pg_status__pg_database` — PostgreSQL database name. Default: `postgres`.
- `pg_status__pg_port` — PostgreSQL port. To use a different port for each
  host, provide a comma-separated list in the same order as
  `pg_status__hosts`. A single value applies to every host. Default: `5432`.
- `pg_status__hosts_dc` — Optional comma-separated DC for each host, in the
  same positional order as `pg_status__hosts`.
- `pg_status__current_dc` — Optional DC of the pg-status instance. When set,
  it takes precedence over `pg_status__current_dc_env`.
- `pg_status__current_dc_env` — Optional name of another environment variable
  whose value is the current DC.
- `pg_status__hosts_geo` — Optional comma-separated geo for each host, in the
  same positional order as `pg_status__hosts`.
- `pg_status__current_geo` — Optional geo of the pg-status instance. When set,
  it takes precedence over `pg_status__current_geo_env`.
- `pg_status__current_geo_env` — Optional name of another environment variable
  whose value is the current geo.
- `pg_status__max_fails` — Number of consecutive failed checks before a host
  is considered dead. Default: `3`.
- `pg_status__sleep_ms` — Target period, in milliseconds, between the starts
  of consecutive checks of each host. Checks of a host never overlap: if a
  check takes longer than the period, the next one starts as soon as it
  ends. While a master is selected, some dead hosts are checked less often;
  see [Reaction speed](#reaction-speed-to-host-unavailability). Must be
  greater than `0`. Default: `1000`.
- `pg_status__query_timeout_ms` — Deadline, in milliseconds, for one check:
  connecting, sending the query, and reading the result. A check that times
  out fails, and its connection is closed. DNS resolution and waiting for a
  turn to connect do not count. Must be greater than `0`; may exceed
  `sleep_ms`. Default: `1000`.
- `pg_status__conn_max_age_ms` — Maximum age, in milliseconds, of a reused
  PostgreSQL connection. An older connection is replaced when no other
  connection is being opened or waiting to open; until then it keeps serving
  checks. Host names are resolved only when connecting: if a host entry is a
  proxy, pooler, or DNS alias that can switch servers while existing sessions
  stay open, lower this value. Default: `3600000` (1 hour).
- `pg_status__poll_error_log_interval_ms` — Minimum interval, in
  milliseconds, between `error` records about continuing PostgreSQL poll
  failures of the same host; failures in between are logged at `debug`. The
  first failure after a successful poll is always logged at `error`. `0` logs
  every failure at `error`. Default: `60000` (1 minute).
- `pg_status__sync_max_lag_ms` — Maximum time lag, in milliseconds, for a
  replica to be considered time-synchronous; see [Time lag](#time-lag).
  Default: `1000`.
- `pg_status__sync_max_lag_bytes` — Maximum WAL lag, in bytes, for a replica
  to be considered byte-synchronous; see [Byte lag](#byte-lag). Default:
  `1000000` (1 MB).
- `pg_status__http_listen_address` — IP address on which the HTTP server
  listens. Accepts an IPv4 address, an IPv6 address, or `*` for best-effort
  IPv4/IPv6 wildcard listeners. Default: `0.0.0.0`.
- `pg_status__http_port` — HTTP server port. Default: `8000`.
- `pg_status__log_level` — Minimum logging level. Accepts `debug`, `info`,
  `warning` (or `warn`), `error`, or `fatal`. Default: `info`.
- `pg_status__log_format` — Log format: `text` (default) or `json`.

A locality dimension, DC or geo, is used only when its current value and a
non-empty value for every entry in `pg_status__hosts` are available. If only
part of it is configured, pg-status logs a startup warning and does not use
it; the other dimension, if complete, still applies.

A direct current value, such as `pg_status__current_dc`, takes precedence
over the variable named by `pg_status__current_dc_env`; geo follows the same
rule. Indirect variable names must match `[A-Za-z_][A-Za-z0-9_]*`, and the
lookup is not recursive. In a container, the referenced variable must also be
passed into the container; the bundled Compose example forwards
`PLATFORM_DC` and `PLATFORM_GEO`.

Example using a direct DC and an indirectly supplied geo:

```sh
pg_status__hosts="db-frankfurt.example,db-amsterdam.example"
pg_status__hosts_dc="frankfurt,amsterdam"
pg_status__hosts_geo="europe,europe"
pg_status__current_dc="frankfurt"
pg_status__current_geo_env="PLATFORM_GEO"
PLATFORM_GEO="europe"
```

### PostgreSQL TLS

TLS is configured by libpq's standard environment variables. pg-status does
not add its own certificate store or TLS configuration format:

- `PGSSLMODE` — libpq TLS mode, such as `disable`, `require`, or
  `verify-full`.
- `PGSSLROOTCERT` — path to the trusted CA certificate bundle.
- `PGSSLCRL` — path to a certificate revocation list.
- `PGSSLCERT` — path to the client certificate when the server requires mTLS.
- `PGSSLKEY` — path to the corresponding client private key. Its filesystem
  permissions must satisfy libpq's requirements.

### PostgreSQL privileges

With PostgreSQL's default privileges, any role that can connect can be used
for polling. For complete measurements, grant it `pg_monitor` or
`pg_read_all_stats`. Without them, pg-status does not see the upstream WAL
end of a replica or the timeline of the WAL it receives, and takes the
replica's timeline from its control file, which shows a new timeline only
after the replica's next restartpoint. After a failover, a replica that
already follows the new master then meets no byte threshold and no `min_lsn`
for up to `checkpoint_timeout`, and such requests go to the master.

pg-status also uses `pg_stat_wal_receiver`, `pg_control_checkpoint()`, and
`pg_control_recovery()`, which PostgreSQL allows everyone to use by default.
If the polling query fails with a permission error, pg-status logs a warning
and polls without the receiver statistics, or, if that fails too, also
without the control file functions. In the latter case replica timelines are
unknown, and after a failover a replica left on the abandoned WAL history can
still meet `min_lsn` and byte thresholds; only the
[`timeline` parameter](#lsn-query-parameter-read-your-writes) excludes it.
The full query is tried again after 5 minutes or after a reconnection.

## Installation

Available installation options:

- [Debian package](https://github.com/krylosov-aa/pg-status/releases/download/2.3.2/pg-status_2.3.2_amd64.deb)
- [Docker Hub image](https://hub.docker.com/r/krylosovaa/pg-status)
- [Docker build configurations](docker)
- [Statically linked binary](https://github.com/krylosov-aa/pg-status/releases/download/2.3.2/pg-status_2.3.2_linux_amd64_static.tar.gz)
- [Dynamically linked binary](https://github.com/krylosov-aa/pg-status/releases/download/2.3.2/pg-status_2.3.2_linux_amd64_shared.tar.gz)

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

Measured on an Ubuntu 24.04 VM with 4 Ice Lake vCPUs and 4 GB RAM. pg-status
was pinned to **one vCPU**;  requests used localhost HTTP with keep-alive.

| Workload                           | Confirmed RPS | Worst p99 |
|------------------------------------|--------------:|----------:|
| `/master`                          |    **52,500** |   4.17 ms |
| `/master` + `/most_sync_by_bytes`  |    **55,000** |   4.51 ms |
| `/master` + `/replica`, fresh RYOW |    **52,500** |   4.50 ms |

At these rates, pg-status used approximately **90–94% of one vCPU** and
**10 MiB RSS**. With a **0.1-vCPU quota**, a mixed workload confirmed
**3,000 RPS** at p99 ≤5 ms.

See the [full results](docs/performance.md) for all ten scenarios,
CPU/RAM measurements, rare latency spikes and limitations, and the
[benchmark guide](test/rps/README.md) to reproduce the measurements.

## How it works

### Concurrent polling

A single thread polls all hosts concurrently with libpq's non-blocking API,
so a slow or unresponsive host does not delay updates for the others. Each
host has its own schedule: a check starts `pg_status__sleep_ms` after the
previous one **started**, so with a 1000 ms period, a check that takes 200 ms
is followed by the next one 800 ms later. If a check overruns the period, the
next starts as soon as it ends; missed periods do not accumulate. Two
exceptions limit the CPU spent on connections: a check that needs a new
connection may wait for [its turn](#connection-reuse), and, while a master is
selected, some dead hosts are checked
[less often](#reaction-speed-to-host-unavailability).

Each check has a deadline of `pg_status__query_timeout_ms`. DNS resolution
during connection setup blocks inside libpq and can delay checks of all
hosts, but it does not count toward their deadlines: a slow DNS response
alone does not fail a check.

### Time lag

`lag_ms` estimates how old a replica's data is. It starts from the age of the
last transaction the replica has replayed, as of its last check:

```text
commit_age = now() - pg_last_xact_replay_timestamp()
```

`now()` comes from the replica's clock and the timestamp from the master's.
pg-status corrects the age when the replica's clock is behind the master's
by more than the accuracy of its measurement of both clocks, about half a
round trip; a replica clock that is ahead makes the age larger. Until a
replica replays its first transaction after it starts, the age is unknown.

While the master commits nothing, the age keeps growing even though the
replica has nothing left to replay. Therefore, when a replica has replayed
exactly the WAL position from the selected master's last successful check,
`lag_ms` is also bounded by the time since that check started, which is less
than `pg_status__sleep_ms` while checks run on schedule:

```text
lag_ms = min(commit_age, time since the start of the master's last check)
```

To use the master's latest position, a replica's check may wait for a check
of the master that is in progress or due before the replica's next check.
The wait does not delay the replica's next check and does not fail it.

The bound does not apply, and `lag_ms` is the commit age alone, while:

- no master is selected, or the master's last check failed;
- the replica's position differs from the master's, or its timeline does
  (see [LSN query parameter](#lsn-query-parameter-read-your-writes));
- the master has WAL that it has not written yet: asynchronous commits
  (`synchronous_commit = off`), a transaction in progress that has written
  data, or read-only queries that write WAL, such as hint bits with data
  checksums. Such WAL is usually written within `wal_writer_delay`, but an
  open transaction may keep it unwritten for several seconds.

With frequent commits, the commit age is usually the smaller value. On a
master without writes, a caught-up replica's `lag_ms` can be anywhere below
`pg_status__sleep_ms`, so a smaller threshold, such as `lag_ms=50`, excludes
it part of the time, and the master is returned instead.

### Byte lag

`lag_bytes` estimates how many WAL bytes a replica has yet to replay:

```text
lag_bytes = max(0, max(master_lsn, received_lsn, latest_end_lsn) - replay_lsn)
```

- `master_lsn` is `pg_current_wal_lsn()` from the last check of the selected
  master, or, while none is selected, of the master selected last. Another
  master, as in a split brain, does not count.
- `received_lsn` is `pg_last_wal_receive_lsn()`, WAL the replica has
  received, and `latest_end_lsn` is the upstream WAL end reported to its WAL
  receiver, visible with the [privileges](#postgresql-privileges) above.
- `replay_lsn` is `pg_last_wal_replay_lsn()`, WAL the replica has applied.

These positions can be stale, so zero byte lag does not guarantee freshness.
`lag_bytes` is unknown in two cases: when there is nothing to compare with,
as no master has been seen since pg-status started and the replica's WAL
receiver is not running, and when the replica's timeline differs from the
master's, as its position may belong to another WAL history.

### Connection reuse

Each host keeps its PostgreSQL connection open between checks, so a check
does not repeat the TCP and authentication handshake. A connection is
replaced when it is older than `pg_status__conn_max_age_ms`, and after any
query error or socket failure.

Opening a connection is the most CPU-intensive part of polling: TLS and
SCRAM authentication cost far more than a status query, and under a small
CPU quota, several setups at once can use up the quota that the HTTP thread
shares and delay responses. Therefore new connections take turns:

- only one connection negotiates TLS or authenticates at a time, for at most
  1 second. The turn is taken only after TCP connects, so unreachable hosts
  do not hold up the others;
- after a setup that reached a TLS handshake or password authentication, the
  next one waits 100 ms;
- connection starts are spaced by
  `min(pg_status__sleep_ms / number of hosts, 250 ms)`.

With fast setup, each host still gets about one connection attempt per
period; under a small CPU quota, turns take longer. An aged connection keeps
serving checks until no other connection is waiting for a turn, so recycling
never delays a check.

### Consistency

All fields of one host in a response come from the same check. Hosts are
polled independently, so at any moment some hosts have newer data than
others; there is no consistent view across hosts. The polling thread and the
HTTP handlers never block each other.

### Reaction speed to host unavailability

So that a temporary issue does not mark a host as dead, pg-status waits for
`pg_status__max_fails` consecutive failed checks. An unanswered check fails
after `pg_status__query_timeout_ms`, so counting from an outage, detection
takes up to approximately:

```text
sleep_ms + query_timeout_ms
  + (max_fails − 1) × max(sleep_ms, query_timeout_ms)
```

The first `sleep_ms` allows for an outage just after a successful check.
With the defaults, detection takes up to roughly 4 seconds. Immediate
connection errors are detected sooner; DNS stalls,
[connection turns](#connection-reuse) when several hosts fail at once, or
delays in the monitoring thread can extend this time. For example, if four
servers accept TCP connections but never complete session setup, each
attempt holds the turn for the full second, and detection with the defaults
takes about 9 seconds.

After the first failed check, and until the host is dead, it is possibly
dead (`possible_dead: true`). This state affects host selection:

- Master selection prefers fully responsive masters, then possibly dead
  masters. If neither exists, no master is selected.
- Replica selection prefers fully responsive replicas. If none meets the
  search criteria, a possibly dead replica is returned. Round-robin applies
  within the best available health and locality group.

Lag and LSN filters use the last successfully measured `lag_ms`,
`lag_bytes`, and `lsn` of a possibly dead host, so a failed check does not
make a lagging replica appear synchronous. A dead host is excluded from
selection, and the HTTP API returns `null` for its lag and LSN fields. A
successful check clears the failure counter and the possibly dead state.

Every check of a dead host opens a new connection. If the attempt fails after
a TLS handshake or password authentication, for example at a connection
pooler whose PostgreSQL is down, it costs as much CPU as a successful
connection. While a master is selected, such hosts are checked less often:
the period doubles with each such failure, up to 8 seconds (2, 4, then
8 seconds with the defaults), or `pg_status__sleep_ms` if that is longer, so
a recovered host may be noticed up to 8 seconds later. Hosts that fail
earlier, for example with a refused TCP connection or a DNS error, are still
checked every `pg_status__sleep_ms`. While no master is selected, every dead
host is checked every `pg_status__sleep_ms`, because any of them may bring the
master back; under a small CPU quota, these attempts can delay HTTP responses
until a master is found.

### Split-brain

With client-side master detection, pg-status cannot determine which host
*should* be the master during a split-brain scenario. Within the best available
health group, the first master in `pg_status__hosts` wins.

## Logging

The service writes thread-safe, single-line logs to stderr. The default
`text` format contains an RFC 3339 UTC timestamp, severity, component, and
message:

```text
2026-08-21T12:34:56.123Z INFO http: server started address=0.0.0.0 port=8000
2026-08-21T12:35:01.245Z WARNING monitor: host state changed host=pg-2 state=possible_dead
```

Set `pg_status__log_format=json` for one JSON object per line, with the same
RFC 3339 UTC timestamps and Deploy-compatible fields:

```json
{"@timestamp":"2026-08-21T12:35:01.245Z","levelStr":"WARNING","component":"monitor","message":"host state changed host=pg-2 state=possible_dead"}
```

`pg_status__log_level` applies independently of the format. System errors add
a numeric `errno`. Fatal records use `levelStr=ERROR`.
Long values are shortened with `...[truncated]`
while preserving valid JSON. Host names, states, and lag values remain in
`message`.

The default `info` level reports startup, shutdown, role changes, availability
changes, and transitions across the global `pg_status__sync_max_lag_*`
thresholds. For each host, the first PostgreSQL poll failure is logged at
`error`; continuing failures are logged at `error` at most once per
`pg_status__poll_error_log_interval_ms` (1 minute by default), with the
current operation and error. This limit also covers timeouts and
invalid results, even when the failure reason changes. Intermediate failures
are available at `debug`. A fully successful poll resets the limit and logs
recovery at `info` through the host's availability transition. Polling frequency
is unaffected. Detailed polling progress and replica-to-master fallback
decisions are also available at `debug`.

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
