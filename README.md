# pg-status

A microservice (sidecar) that helps instantly determine the status of your PostgreSQL hosts including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.

pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.


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

### Parameters

You can configure various parameters using environment variables:

- `pg_status__pg_user` — The user under which SQL queries to PostgreSQL will be executed. Default: `postgres`
- `pg_status__pg_password` — The password for the PostgreSQL user. Default: `postgres`
- `pg_status__pg_database` — The name of the database to connect to. Default: `postgres`
- `pg_status__hosts` — A list of PostgreSQL hosts, separated by the character specified in `pg_status__delimiter`.
- `pg_status__delimiter` — The delimiter used to separate hosts. Default: `,`
- `pg_status__port` — The connection port. You can specify separate ports for individual hosts using the same delimiter. Default: `5432`
- `pg_status__connect_timeout` — The time limit (in seconds) for establishing a connection to PostgreSQL. Default: `2`
- `pg_status__max_fails` — The number of consecutive errors allowed when checking a host’s status before it is considered dead. Default: `3`
- `pg_status__sleep` — The delay (in seconds) between consecutive host status checks. Default: `5`
- `pg_status__sync_max_lag_ms` — The maximum acceptable replication lag (in milliseconds) for a replica to still be considered time-synchronous. Default: `1000`
- `pg_status__sync_max_lag_bytes` — The maximum acceptable lag (in bytes) for a replica to still be considered byte-synchronous. Default: `1000000` (1 MB)

You can configure the port on which the http server will listen via the startup arguments. For example:

```shell
pg-status -port 12345
```

or

```shell
pg-status --port 12345
```


# Installation

You can currently set up and run the project in the following ways:

## Install deb package

You can download a deb package for linux amd64 from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest deb package](https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64.deb)

```shell
wget https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64.deb && sudo dpkg -i pg-status_1.3.2_amd64.deb
```

then run:
```shell
pg-status
```

## Static binary

A statically linked binary is provided you can simply download it and run it without any additional setup.
You can download pre-built linux amd64 binaries from [releases](https://github.com/krylosov-aa/pg-status/releases/).

[Latest static binary](https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64_static)

```shell
wget https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64_static && chmod +x pg-status_1.3.2_amd64_static
```

## Shared binary

A dynamically linked binary is available, which requires the necessary dependencies to be installed on your system.
You can read about the required dependencies below, and also take a very look at the docker files,
which demonstrate the installation. [Latest shared binary](https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64_shared)

```shell
sudo apt-get install libpq5 libmicrohttpd12 libcjson1 && wget https://github.com/krylosov-aa/pg-status/releases/download/1.3.2/pg-status_1.3.2_amd64_shared && chmod +x pg-status_1.3.2_amd64_shared
```

## Run a Docker container

There are several available options:

### docker-hub:
- [Fast build, very lightweight container](https://hub.docker.com/r/krylosovaa/pg-status)

### Alpine
- [Fast build, very lightweight container](docker/alpine/Dockerfile_shared)
- [The lightest container, but takes slightly longer to build](docker/alpine/Dockerfile_shared_disabled_https)
- [The heaviest among the lightweight options, but provides a static binary](docker/alpine/Dockerfile_static)

### Ubuntu
- [With dynamic linking](docker/ubuntu/Dockerfile_shared)
- [Static binary](docker/ubuntu/Dockerfile_static)

The [Makefile](Makefile) contains several ready-to-use commands that you can either run directly or use as a reference.
Each Dockerfile describes a build process (which you can adapt if you’re not using these files) that allows you to
build a binary and either export it to the host or run it directly inside the container.


## Build with CMake

You can compile the project from source for any platform using CMake.
You can refer to the Dockerfiles for examples of how to install dependencies and configure the build,
depending on whether you prefer a dynamically linked or static binary.

## Dependencies

This project depends on three external libraries:
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) under [GNU LGPL v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- [postgresql libpq](https://www.postgresql.org/docs/current/libpq.html)
- [CJson](https://github.com/DaveGamble/cJSON)


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


# Performance

Memory - 9Mib

Depending on the API being called and the format selected
(plain `/master` is the fastest, json `/hosts` is the slowest):

- 0.1 CPU — Requests/sec: ~1600-2000
- 1 CPU — Requests/sec: ~8600-9000

[Detailed performance reports](docs/performance.md)


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

# Third‑party components

It uses the following third‑party components:

- libmicrohttpd — licensed under [the GNU Lesser General Public License v2.1 or later](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- cJSON — licensed under [the MIT License](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)
- libpq — licensed under [the PostgreSQL License](https://www.postgresql.org/about/licence/)
