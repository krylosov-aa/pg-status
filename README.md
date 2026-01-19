# pg-status

A microservice (sidecar) that helps instantly determine the status of your PostgreSQL hosts including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.

pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.

To learn more about why this project exists and
what problem it solves, you can read the article on one of the
platforms that are convenient for you:

- [dev.to](https://dev.to/krylosov-aa/pg-status-a-lightweight-microservice-for-checking-postgresql-host-status-32jd)
- [medium](https://medium.com/@krylosov.andrew/pg-status-a-lightweight-microservice-for-checking-postgresql-host-status-a70f8944302a)


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


### `GET /master`

Returns the host of the current master, if one exists. If no master is available, it returns null.

### `GET /replica`

Returns the host of a replica, selected using the round-robin algorithm.
If no replicas are available, the master’s host is returned instead.

### `GET /sync_by_time`

Returns the host of a replica (selected using the round-robin algorithm) considered time-synchronous — that is, its time lag is less than the value specified in `pg_status__sync_max_lag_ms`.
If no replica meets this condition, the master’s host is returned.

### `GET /sync_by_bytes`

Returns the host of a replica (selected using the round-robin algorithm) considered byte-synchronous — that is, according to the WAL LSN, its lag is less than the value specified in `pg_status__sync_max_lag_bytes`.
If no replica meets this condition, the master’s host is returned.

### `GET /sync_by_time_or_bytes`

Returns the host of a replica (selected using the round-robin algorithm) that is considered synchronous either by time or by bytes.
If no such replica exists, the master’s host is returned.

### `GET /sync_by_time_and_bytes`

Returns the host of a replica (selected using the round-robin algorithm) that is considered synchronous by both time and bytes.
If no such replica exists, the master’s host is returned.

### `GET /hosts`

Returns a list of all hosts with their status information in json format.

For example:
```json
[
  {
    "host": "host-1",
    "master": true,
    "alive": true
  },
  {
    "host": "host-2",
    "master": false,
    "alive": true
  },
  {
    "host": "host-3",
    "master": false,
    "alive": false
  }
]
```

### `GET /status`

Returns status of a host that you specified in the get parameter.

For example: `http://127.0.0.1:8000/status?host=host-1`
```json
{
  "master": false,
  "alive": true,
  "sync_by_time": true,
  "sync_by_bytes": true
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
- `pg_status__sleep` — The delay (in seconds) between consecutive host status checks. Default: `5`
- `pg_status__sync_max_lag_ms` — The maximum acceptable replication lag (in milliseconds) for a replica to still be considered time-synchronous. Default: `1000`
- `pg_status__sync_max_lag_bytes` — The maximum acceptable lag (in bytes) for a replica to still be considered byte-synchronous. Default: `1000000` (1 MB)
- `pg_status__http_port` — the port on which the http server will listen. Default: `8000`


# Installation

In short there is:
- [deb package](https://github.com/krylosov-aa/pg-status/releases/download/1.5.2/pg-status_1.5.2_amd64.deb)
- [published container to Docker Hub](https://hub.docker.com/r/krylosovaa/pg-status)
- various [docker containers](docker)
- [static binary](https://github.com/krylosov-aa/pg-status/releases/download/1.5.2/pg-status_1.5.2_linux_amd64_static.tar.gz)
- [shared binary](https://github.com/krylosov-aa/pg-status/releases/download/1.5.2/pg-status_1.5.2_linux_amd64_shared.tar.gz)


For more information, go to the [docs/installation.md](docs/installation.md) section.

# Performance

Memory - 9Mib

Depending on the API being called and the format selected
(plain `/master` is the fastest, json `/hosts` is the slowest):

- 0.1 CPU — Requests/sec: ~1600-2000
- 1 CPU — Requests/sec: ~8600-9000

[Detailed performance reports](docs/performance.md)

# Implementation Details

There is one writer and many readers in the program. To ensure the fastest possible response for readers and to allow
the writer to record new host statuses without delay, a lock-free approach was chosen.

The writer goes through all the hosts listed in `pg_status__hosts` every `pg_status__sleep` seconds, attempting to
connect to each host and read its status. Upon successfully receiving a response from a host, its status is updated
immediately. This means if the writer has started traversing the hosts but hasn't finished yet, there will be
inconsistency in the data: some hosts will have new data, while others will not. Thanks to the lock-free design,
the writer cannot be blocked for long, so the window of inconsistency is quite small; however, it can grow depending
on the value of `pg_status__connect_timeout`.

For this project, it was more important to achieve the fastest response times and the most up-to-date data possible,
so consistency was intentionally sacrificed.


# Logging

The service writes to stdout and stderr. All errors, such as connection errors to pg hosts,
are written to stderr.

Informational messages about service startup and shutdown are written to stdout.

More importantly, information about host status changes is written to stdout:

If a host is dead, the message will be: `<host-name>: dead`

If a host is revived or becomes a master after failover, the message will be: `<host-name>: master`

If a host is revived or becomes a replica after failover, the message will be: `<host-name>: replica`

For replicas, there are also messages about replica synchronicity:

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

You can create and populate a `.env` file using [the provided example](.env_example), or specify the required
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
