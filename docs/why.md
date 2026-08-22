# Three PostgreSQL Master/Replica Discovery Problems — and How to Solve Them

When an application starts using multiple PostgreSQL hosts, the headaches begin: you need to dynamically find the master after a failover, pick a replica with an acceptable replication lag, and guarantee that a user won't see stale data immediately after their own write. DNS caches for minutes, libpq knows nothing about lag, HAProxy has never heard of LSN. Let's look at how existing solutions work and how to cover all three problems with a lightweight HTTP service — [pg-status](https://github.com/krylosov-aa/pg-status).

## PostgreSQL Master and Replicas

There comes a point in an application's life when a single PostgreSQL host is no longer enough. I'll highlight the two main reasons:

### Resilience

A single host can go down or become unreachable on the network. If the application has only one PostgreSQL host, this can mean a complete outage. To survive the failure of any individual host, teams set up multiple hosts in a master-replica scheme.

The scheme works like this: there is one master host that accepts write requests from users. There are one or more replica hosts that accept only read requests. Data reaches the replica through *streaming replication*: the replica connects to the master and continuously replays its WAL journal (Write-Ahead Log) — the log of all data changes that PostgreSQL maintains on the master.

This immediately implies an important property: a replica is **always slightly behind** the master. There is a delay between a transaction being committed on the master and it being replayed on the replica — this is called *replication lag*. It is usually milliseconds, but under heavy load or network issues it can grow to seconds or more. This is normal and expected — which is exactly why it matters to know how far behind a replica is before reading from it.

In this scheme, if the master becomes unavailable, one of the replicas can be promoted to the new master, and the application survives the incident. This is called a *failover*. Importantly, failover does not happen automatically on its own — it must be orchestrated. Tools exist for this purpose that monitor cluster state and, when the master goes down, automatically elect a new one from among the replicas and reconfigure replication. Without such a tool, the switch has to be done manually. Even without failover, if the application reads from replicas, read-only functionality can continue to be served while the master is unavailable.

### Throughput

An application can become so loaded that the PostgreSQL host becomes the bottleneck. PostgreSQL is not a distributed database, and it is not easy to scale horizontally. But in most applications, 80–90% of database queries are reads. Routing them to replicas offloads the master in exactly the places that hurt: CPU for query processing and I/O for reading data from disk. The master is left to handle only writes.


## The Problems on the Application Side

Standing up multiple hosts is only half the battle. The headaches start on the application side, which previously made queries to a single static host. Now the right host must be found dynamically, since a failover with a master switch can happen at any moment, and replicas each have different lag.

Three concrete problems arise:

**1. Find the current live master.** Any application needs this — you can't write without a master. You can't simply hardcode the master address in a config: after a failover a different host becomes the master, and the application must learn this without restarting. So the master address needs to be determined dynamically — ideally before every write session.

**2. Find a sufficiently in-sync replica.** This is needed to offload the master. There may be several replicas, each with different lag — you need to pick a suitable one. Lag is measured in two dimensions: by time (how many milliseconds the replica is behind the master) and by WAL bytes (how much unreplayed data has accumulated). What "sufficiently" means depends on context — a 5-second lag is acceptable for an analytics report, but not for a user profile page.

**3. Guarantee read-your-writes.** The user saved data, the page reloaded and... showed old data. This happens because the write went to the master, but the next read went to a replica that hadn't caught up yet. This is a stricter version of problem 2: instead of a threshold lag, you need a point guarantee — the replica must have replayed data up to the exact WAL position that a specific transaction left behind.

In this article I'll explain how to solve all three problems on the application side.


## Existing Solutions and Their Limitations

### libpq Multi-Host

In libpq, you can list hosts directly in the connection string and specify the desired host type via `target_session_attrs`:

```
host=host-1,host-2,host-3 target_session_attrs=read-write
```

libpq tries hosts sequentially and picks a suitable one — all built into the driver.

Values for `target_session_attrs`:
- `read-write` — master only
- `read-only` — replica only
- `prefer-standby` — replica if available, otherwise master
- `any` — any live host

Sounds convenient, but there are significant limitations on closer inspection.

**Finding the right host requires real TCP connections.** libpq determines host type by running `SELECT pg_is_in_recovery()` after establishing a connection. If the first host is a replica but a master is needed, libpq connects to it, runs the query, discovers it's a replica, tears down the connection, and moves on to the next. Each such attempt is a full TCP handshake plus PostgreSQL authentication. With three hosts, in the worst case the right one isn't reached until the third attempt.

**Slow failover detection.** libpq only learns about a master change when the current connection breaks or a query returns an error. While the connection to the old master is alive, libpq keeps writing to it — even if that host has already transitioned to replica mode. There is no background monitoring that signals "this host changed roles."

**No lag control.** `prefer-standby` returns any live replica, even one that's 30 minutes behind. There's no way to say "return a replica with lag_ms ≤ 100."

**No read-your-writes.** No built-in mechanism.

### DNS

A simple approach: a DNS record always points to the current master, and when a failover happens the DNS record is updated. Minimal infrastructure. But updating DNS is not an instantaneous operation, and in practice the delay can be significantly longer than it seems.

**Existing connections will not switch at all.** This is the most important point. DNS is resolved once when a connection is established. If you have a pool of 20 connections to the old master — they will stay on it until the connections are recreated. No DNS update will switch them. Switching only happens when the pool is forced to create a new connection — on a reuse timeout, on an error, or manually.

**Multi-layer caching.** Even for new connections, a DNS record passes through several independent caches: the OS resolver (30–60 s), JVM (forever by default — `networkaddress.cache.ttl = -1`), Kubernetes CoreDNS (30 s), and the connection pool which recreates connections on its own schedule. The delays add up, and after a failover the new address reaches the application after tens of seconds to minutes.

So during a failover, a master switch through DNS can take anywhere from tens of seconds to several minutes — depending on the stack. And even after the DNS change, some traffic will still hit the old host through existing connections in the pool.

**No metadata.** DNS returns only an address, nothing about replica lag.

### HAProxy

HAProxy is a TCP/HTTP load balancer that can health-check backends and route connections. For PostgreSQL the typical setup has two listeners — one for writes (master only), one for reads (replicas). The application itself decides which port to connect to.

**The built-in health check doesn't detect host role.** HAProxy can verify backend reachability at the network level, but it can't distinguish master from replica with built-in tools. Role detection requires an external HTTP endpoint on each PostgreSQL host that returns 200 or 503 depending on role. You have to implement and maintain this endpoint yourself — either via an auxiliary process or via an existing cluster orchestration tool.

**Lag control is entirely manual work.** HAProxy knows nothing about PostgreSQL replication. To exclude lagging replicas, you have to implement the logic in the health-check endpoint yourself: query the WAL position or timestamp of the last replayed event, calculate the lag, and return 503 if a threshold is exceeded. This is extra code to write, test, and maintain. The lag threshold is yet another configuration constant with no way to override it per request.

**HAProxy sits in the path of SQL queries.** Every connection to the database passes through it — an extra network hop. For HA you need a second HAProxy with keepalived and a VIP, otherwise HAProxy itself becomes a single point of failure.

**Read-your-writes is impossible in principle.** HAProxy operates at the TCP connection level and has no access to the SQL query contents — it doesn't know what LSN a client's transaction left behind.

### pgpool-II

A full SQL proxy: parses every query, decides where to route it (SELECT → replica, INSERT/UPDATE → master), pools client connections, manages failover. Transparent to the application — no code changes needed.

Compared to simpler solutions, pgpool-II has real lag control support:

- `delay_threshold` — maximum replica lag in WAL bytes. If a replica exceeds the threshold, pgpool stops sending SELECTs to it until it catches up.
- `delay_threshold_by_time` (pgpool-II 4.4+) — same, but in time units (milliseconds by default), using `pg_stat_replication.replay_lag`.
- `prefer_lower_delay_standby` (pgpool-II 4.3+) — if the selected replica exceeds the threshold, pgpool picks the least-lagging replica instead of falling back to master.

This is noticeably better than DNS or libpq. But there are important limitations:

- **Sits in the path of SQL queries.** Every query to the database goes through pgpool — an extra network hop. pgpool itself becomes a single point of failure; HA mode requires a watchdog setup with a VIP.
- **Load balancing is chosen at session level, not query level.** The replica host is assigned when the connection is established and doesn't change until it's closed — unless `statement_level_load_balance` is enabled. And inside an explicit transaction, after the first write query, all subsequent SELECTs go to the master until the transaction ends.
- **Lag thresholds are global.** `delay_threshold` and `delay_threshold_by_time` are configuration parameters, not query parameters. You can't say "apply a 50 ms threshold for this specific query." One threshold for the entire application.
- **No read-your-writes via LSN.** pgpool-II provides no mechanism for passing a specific WAL position from the client.

### Patroni REST API

If you're already using Patroni for PostgreSQL cluster management, it has an HTTP API: `/leader`, `/primary`, `/replica`, `/health`, `/cluster`, and others.

Patroni can do more than it might seem. The `/replica?lag=10MB` endpoint returns HTTP 200 only if the replica is no more than 10 MB behind — you can specify in bytes or human-readable format (`16kB`, `64MB`, `1GB`). The `/cluster` endpoint returns complete information about each cluster member, including `replay_lsn`, `replay_lag`, `receive_lsn`, `receive_lag`.

- **This is a health-check API, not a discovery API.** `/replica?lag=X` returns HTTP 200 or 503 — "this node is suitable or not." It doesn't return a hostname to connect to. This format is designed for use with a load balancer (like HAProxy) that is already pointed at a specific node and verifies its suitability. To independently select a suitable replica, you have to poll each node separately or parse the `/cluster` response.
- **Patroni is a full-featured HA cluster manager.** It requires an external DCS (etcd, Consul, or ZooKeeper). If Patroni is already in place — use its API. If not — deploying the whole stack just for discovery is overkill.
- **No read-your-writes via LSN.** `/replica?lag=X` filters by threshold lag in bytes, but there's no way to pass a specific WAL position and get a replica that has already replayed data up to that position.

### pg-status — an HTTP Discovery Service

I built a microservice [pg-status](https://github.com/krylosov-aa/pg-status) — a lightweight HTTP service that polls PostgreSQL hosts in the background and answers questions like "who is the current master?", "which replica is within 100 ms of lag?", "which replica has already replayed a specific WAL LSN?" — all from memory, in fractions of a millisecond.

**Does not sit in the path of SQL queries.** This is the fundamental architectural difference from HAProxy and pgpool-II. The application connects to PostgreSQL directly — pg-status only advises *which host* to connect to.

**Active background monitoring, not reaction to errors.** Unlike libpq and DNS, pg-status doesn't wait for a connection to fail. It continuously polls all hosts in the background and always has an up-to-date picture. A master change is detected within one polling interval (5 seconds by default, configurable) — not after tens of seconds of TTL, not after a connection drops.

**Lag control per request or global config.** A lag threshold can be set globally, or passed directly as a query parameter. One application endpoint can require lag ≤ 50 ms while another is fine with 5 seconds — with no configuration changes and no service restart.

**Read-your-writes via LSN.** The only solution reviewed here that provides a built-in tool for this pattern. After a write to the master, the application passes the transaction's WAL position in a request to pg-status — and gets back only the replica that has already replayed data up to that position (or the master).

**Automatic load balancing across replicas.** When multiple replicas fall within the acceptable lag, pg-status automatically distributes load among them round-robin. No need to implement balancing yourself.

**Stack and platform agnostic.** Plain HTTP, response in plain text or JSON. Can be called from any programming language with a single HTTP request, no special client libraries needed. Written in C, supports Linux and macOS.

**Minimal infrastructure requirements.** No etcd, Consul, or ZooKeeper like Patroni. No watchdog cluster like pgpool-II in HA mode. One binary, a few environment variables — and it works. Consumes 9 MB of RAM and responds fast enough to call before every database query. Can be deployed as a sidecar so each application instance has an independent discovery helper, or deployed as one or more global instances for the whole application.

Let's walk through how to use pg-status in practice — starting with each of the three problems individually, then a full Python integration.

## Solving Problem 1: Finding the Master

pg-status provides the `GET /master` endpoint for this, which returns the hostname of the current master:

```bash
$ curl http://localhost:8000/master
host-1
```

The application calls this before every write session. pg-status continuously polls hosts in the background and tracks master changes itself — your code simply asks "who is the master right now?" and gets a current answer.

Adding the `Accept: application/json` header returns the response as JSON:

```bash
$ curl -H "Accept: application/json" http://localhost:8000/master
{"host": "host-1"}
```

If there is no master — a 404 is returned.


## Solving Problem 2: Finding a Synchronized Replica

Here pg-status provides several endpoints depending on the required guarantee. The selection logic is straightforward:

- Just any live replica with no freshness requirements — `/replica`
- Time lag matters (milliseconds) — `/sync_by_time`
- Volume of unapplied WAL matters (bytes) — `/sync_by_bytes`
- Either condition is acceptable — `/sync_by_time_or_bytes`
- Both conditions must hold simultaneously — `/sync_by_time_and_bytes`
- The most up-to-date replica, deterministically — `/most_sync_by_bytes`

**Just a live replica** — no lag constraints, round-robin load balancing:

```bash
$ curl http://localhost:8000/replica
host-2
```

**Replica with a time constraint** — lag no greater than the threshold (default 1 second, configurable):

```bash
$ curl http://localhost:8000/sync_by_time
host-2
```

**Replica with a WAL bytes constraint** — byte lag does not exceed the threshold (default 1 MB, configurable):

```bash
$ curl http://localhost:8000/sync_by_bytes
host-2
```

**Replica synchronized by at least one dimension** — either by time or by bytes:

```bash
$ curl http://localhost:8000/sync_by_time_or_bytes
host-2
```

**Replica synchronized by both dimensions simultaneously** — both time and bytes:

```bash
$ curl http://localhost:8000/sync_by_time_and_bytes
host-2
```

**The most synchronized replica** — the one with the smallest byte lag:

```bash
$ curl http://localhost:8000/most_sync_by_bytes
host-2
```

Thresholds can be overridden directly in the request via query parameters:

```bash
# Replica with no more than 50 ms lag for this specific request
$ curl 'http://localhost:8000/sync_by_time?lag_ms=50'

# Replica with no more than 10 KB lag
$ curl 'http://localhost:8000/sync_by_bytes?lag_bytes=10000'
```

This lets a single application use strict guarantees for critical queries and relaxed ones for analytics.

**Important note:** if no suitable replica exists, any of these endpoints will return the master as a fallback. Application code always receives a host — it just may sometimes be the master instead of a replica.


## Solving Problem 3: Read-Your-Writes via `min_lsn`

This is the most interesting problem. Let's first understand what's happening.

### The Problem

After a write to the master, the replica doesn't receive the data instantaneously — there is a delay between a transaction being committed on the master and being replayed on the replica. If you read from a replica immediately after a write, you may see stale data.


### The Solution: WAL LSN

PostgreSQL knows the exact position in the transaction log (WAL LSN) up to which each replica has replayed data. pg-status polls this position and keeps it in memory.

The pattern:
1. After writing to the master, obtain the LSN of that transaction via `pg_current_wal_lsn()`
2. Pass this LSN to pg-status in the next read request
3. pg-status returns only the replica that has already replayed data up to that position

```bash
# After writing to the master, get the LSN:
# SELECT pg_current_wal_lsn(); → '0/3000060'

# Next read — only from a replica that has already replayed '0/3000060'
$ curl 'http://localhost:8000/replica?min_lsn=0/3000060'
host-2
```

If no replica has caught up to the required position yet — the master is returned as a fallback. This is the correct behavior: it's better to read from the master than to show stale data.


## Python Integration

Let me show what this looks like in a real Python application on FastAPI with async SQLAlchemy.

For managing session lifecycle I'm using [context-async-sqlalchemy](https://github.com/krylosov-aa/context-async-sqlalchemy) — a small library I wrote. It solves the following problem: storing a SQLAlchemy session in the current request context, automatically committing on successful response and rolling back on error. The key class is `DBConnect`: an object that knows which host to connect to and lazily creates the engine on first access. It can be switched to a different host via `change_host()` without restarting the application, or you can keep multiple `DBConnect` instances, one per host.

### pg-status Client

A simple wrapper around HTTP — call the needed endpoint and return the hostname:

```python
# pg_status.py
import aiohttp

PG_STATUS_URL = "http://localhost:8000"


async def get_master_host() -> str:
    async with aiohttp.ClientSession() as client:
        async with client.get(f"{PG_STATUS_URL}/master") as response:
            response.raise_for_status()
            return await response.text()


async def get_replica_host() -> str:
    async with aiohttp.ClientSession() as client:
        async with client.get(
            f"{PG_STATUS_URL}/most_sync_by_bytes"
        ) as response:
            response.raise_for_status()
            return await response.text()


async def get_all_hosts() -> list[str]:
    """Returns the names of all known hosts"""
    async with aiohttp.ClientSession() as client:
        async with client.get(f"{PG_STATUS_URL}/hosts") as response:
            response.raise_for_status()
            data = await response.json()
            return [h["host"] for h in data]
```

### Connection Management

`DBConnect` from `context-async-sqlalchemy` is an object that holds a SQLAlchemy engine for one host and lazily creates sessions for it. At application startup, we create one `DBConnect` per host and store them in a dictionary. When a session is needed, we ask pg-status which host is current and retrieve the right `DBConnect` from the dictionary:

```python
# database.py
from sqlalchemy.ext.asyncio import (
    AsyncSession,
    create_async_engine,
    async_sessionmaker,
)
from context_async_sqlalchemy import DBConnect, db_session


def create_engine(host: str):
    return create_async_engine(
        f"postgresql+asyncpg://user:password@{host}:5432/mydb",
        pool_pre_ping=True,
    )


def create_session_maker(engine):
    return async_sessionmaker(
        engine, class_=AsyncSession, expire_on_commit=False
    )


# Dictionary of connections to all hosts — both master and replicas
_connections: dict[str, DBConnect] = {}


async def prepare_connections() -> None:
    """Called at application startup — create a DBConnect for each host"""
    hosts = await get_all_hosts()  # GET /hosts
    for host in hosts:
        _connections[host] = DBConnect(
            engine_creator=create_engine,
            session_maker_creator=create_session_maker,
            host=host,
        )


async def master_session() -> AsyncSession:
    master_host = await get_master_host()  # GET /master
    return await db_session(_connections[master_host])


async def replica_session() -> AsyncSession:
    replica_host = await get_replica_host()  # GET /most_sync_by_bytes
    return await db_session(_connections[replica_host])
```

Here's what happens during a failover: pg-status detects the master change within one polling interval (5 seconds by default, configurable). The very next call to `master_session()` gets the new host from pg-status and returns the already-existing `DBConnect` for it from `_connections`. No separate master object is needed — all connections live in one dictionary, and pg-status tells you which one to use.

### Using in Handlers

In the end, each handler simply picks the right session type:

```python
# handlers.py
async def create_order(data: OrderData) -> Order:
    session = await master_session()  # write → always to the master
    order = Order(**data.model_dump())
    session.add(order)
    return order


async def get_order_list(user_id: int) -> list[Order]:
    session = await replica_session()  # read → to a replica
    result = await session.execute(
        select(Order).where(Order.user_id == user_id)
    )
    return result.scalars().all()
```

### Read-Your-Writes

For RYOW you need to pass the LSN to the client after a write, and at the next read use it to select a replica. It's convenient to organize this through two middlewares.

The main advantage of this approach is that it works automatically for the entire application at once. Wire up the middleware once and every request that actually changed data automatically gets an LSN in the response cookie. No need to think about this when writing each new handler, no need to manually call `pg_current_wal_lsn()` in business logic. The key word is "actually changed": `pg_current_xact_id_if_assigned()` returns NULL for read-only transactions, so the cookie is only updated where there were actual writes — SELECT queries are not affected.

One non-obvious detail is `_LsnHolder`. Since `save_current_lsn_if_there_writes` is called from the `before_commit` hook and can't return a value to `lsn_cookie_middleware` directly, shared mutable state is needed: `lsn_cookie_middleware` puts an `_LsnHolder` into a `ContextVar`, and `save_current_lsn_if_there_writes` retrieves the same object and writes the LSN into it.

```python
# read_own_writes.py
from contextvars import ContextVar
from sqlalchemy import text
from sqlalchemy.ext.asyncio import AsyncSession
from starlette.middleware.base import RequestResponseEndpoint
from starlette.requests import Request
from starlette.responses import Response


class _LsnHolder:
    value: str | None = None


_request_lsn: ContextVar[_LsnHolder] = ContextVar("_request_lsn")


def get_request_lsn() -> _LsnHolder:
    return _request_lsn.get()


async def save_current_lsn_if_there_writes(session: AsyncSession) -> None:
    """Called before commit — saves the LSN if there was a write"""
    result = await session.execute(
        text(
            "SELECT pg_current_wal_lsn()::text "
            "WHERE pg_current_xact_id_if_assigned() IS NOT NULL"
        )
    )
    lsn = result.scalar()
    if lsn:
        get_request_lsn().value = lsn


async def lsn_cookie_middleware(
    request: Request, call_next: RequestResponseEndpoint
) -> Response:
    # Initialize the holder at the start of each request
    _request_lsn.set(_LsnHolder())

    response = await call_next(request)

    # If this request had a write — send the LSN to the client via cookie
    lsn_holder = get_request_lsn()
    if lsn_holder.value:
        response.headers["Set-Cookie"] = (
            f"X-WAL-LSN={lsn_holder.value}; Path=/; SameSite=Lax; Secure"
        )
    return response
```

Wire up both middlewares in the application:

```python
# setup_app.py
from context_async_sqlalchemy.fastapi_utils import (
    add_fastapi_http_db_session_middleware,
)
from starlette.middleware.base import BaseHTTPMiddleware
from read_own_writes import (
    lsn_cookie_middleware,
    save_current_lsn_if_there_writes,
)

add_fastapi_http_db_session_middleware(
    app,
    before_commit=save_current_lsn_if_there_writes,  # captures LSN before commit
)
app.add_middleware(
    BaseHTTPMiddleware, dispatch=lsn_cookie_middleware
)  # sends LSN in cookie
```

On the read side — extract the LSN from the cookie and pass it to pg-status:

```python
async def replica_session_ryow(request: Request) -> AsyncSession:
    min_lsn = request.cookies.get("X-WAL-LSN")
    replica_host = await get_replica_host(min_lsn=min_lsn)
    return await db_session(_connections[replica_host])
```

```python
async def get_replica_host(min_lsn: str | None = None) -> str:
    params = {"min_lsn": min_lsn} if min_lsn else {}
    async with aiohttp.ClientSession() as client:
        async with client.get(
            f"{PG_STATUS_URL}/most_sync_by_bytes", params=params
        ) as response:
            response.raise_for_status()
            return await response.text()
```

The full flow: write request → LSN captured before commit → `X-WAL-LSN` cookie set in response → client sends it in the next request → server reads the cookie and passes `min_lsn` to pg-status → only the replica that has already replayed the required WAL position is returned, or the master if no replica has caught up yet.

#### Where to Store the LSN

A cookie is a convenient option for browser clients: the browser sends them automatically with every request, no changes to client code are needed, the LSN is stored on the client side and doesn't require Redis or any shared storage. Scope is limited by path and expiry. But this is not the only option:

- **Response header + request header** — the server returns the LSN in `X-WAL-LSN`, the client explicitly passes it back in the next request. Suitable for API clients and mobile applications.
- **Redis / shared storage** — the LSN is stored server-side under a `user_id` or `session_id` key. Needed if writes and reads happen on different application nodes — a cookie won't help in that case, the LSN must be placed in shared storage.
- **JWT / session token** — the LSN is included as a claim in the token. Convenient if the application already uses JWT sessions.

#### Protecting the LSN in Production

An LSN is simply a WAL position like `0/3000060`. It's not secret in itself, but accepting it from the client without verification is dangerous: an attacker could pass an arbitrarily large LSN and force the server to always read from the master — no replica will ever reach such a position. In production you should sign the value with HMAC before sending it to the client and verify the signature on receipt — and only then pass the LSN to pg-status.


## Running and Configuration

### Docker

```bash
docker run -d \
  -e pg_status__hosts=host-1,host-2,host-3 \
  -e pg_status__pg_user=postgres \
  -e pg_status__pg_password=postgres \
  -p 8000:8000 \
  krylosovaa/pg-status:latest
```

After startup you can immediately verify:

```bash
$ curl http://localhost:8000/master
host-1

$ curl http://localhost:8000/hosts | jq .
[
  {"host": "host-1", "master": true, "alive": true, "lag_ms": 0, ...},
  {"host": "host-2", "master": false, "alive": true, "lag_ms": 45, ...},
  {"host": "host-3", "master": false, "alive": true, "lag_ms": 120, ...}
]
```

### Key Parameters

| Parameter | Default | What it does |
|---|---|---|
| `pg_status__hosts` | — | Comma-separated list of hosts (required) |
| `pg_status__sleep_ms` | 5000 | Host polling interval in ms |
| `pg_status__sync_max_lag_ms` | 1000 | Lag threshold for `sync_by_time` |
| `pg_status__sync_max_lag_bytes` | 1000000 | Lag threshold for `sync_by_bytes` |
| `pg_status__max_fails` | 3 | Consecutive failures before a host is declared dead |
| `pg_status__query_timeout_ms` | 5000 | Timeout for a single host poll in ms |

### Where to Get It

- [Docker Hub](https://hub.docker.com/r/krylosovaa/pg-status)
- [deb package](https://github.com/krylosov-aa/pg-status/releases/latest) for Debian/Ubuntu
- [Static binary](https://github.com/krylosov-aa/pg-status/releases/latest) for direct execution
- [GitHub](https://github.com/krylosov-aa/pg-status) — can be compiled via CMake for any platform

## Conclusion

Working with PostgreSQL master/replica on the application side comes down to three problems: find the master, find a suitable replica, ensure read-your-writes. pg-status solves all three through a simple HTTP API that responds from memory in fractions of a millisecond.

Happy to answer questions in the comments. If the article was useful — a star on [pg-status](https://github.com/krylosov-aa/pg-status) or [context-async-sqlalchemy](https://github.com/krylosov-aa/context-async-sqlalchemy) is a great motivator to keep developing both projects.
