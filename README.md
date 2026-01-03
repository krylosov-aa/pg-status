# pg-status

A microservice (sidecar) that helps instantly determine the status of your PostgreSQL hosts including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.

pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.


## Usage

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

### API

The service provides several HTTP endpoints for retrieving host information.

All APIs support two response formats: plain text and JSON.

If you include the header `Accept: application/json`, the response will be in JSON format, for example: `{"host": "localhost"}`

If you omit this header, the response will be plain text: `localhost`

If the API cannot find a matching host, it will return a 404 status code.
In this case, the response body will be empty for plain text mode, and `{"host": null}` for json mode.


#### `GET /master`

Returns the host of the current master, if one exists. If no master is available, it returns null.

#### `GET /replica`

Returns the host of a replica, selected using the round-robin algorithm.
If no replicas are available, the master’s host is returned instead.

#### `GET /sync_by_time`

Returns the host of a replica considered time-synchronous — that is, its time lag is less than the value specified in `pg_status__sync_max_lag_ms`.
If no replica meets this condition, the master’s host is returned.

#### `GET /sync_by_bytes`

Returns the host of a replica considered byte-synchronous — that is, according to the WAL LSN, its lag is less than the value specified in `pg_status__sync_max_lag_bytes`.
If no replica meets this condition, the master’s host is returned.

#### `GET /sync_by_time_or_bytes`

Returns the host of a replica that is considered synchronous either by time or by bytes.
If no such replica exists, the master’s host is returned.

#### `GET /sync_by_time_and_bytes`

Returns the host of a replica that is considered synchronous by both time and bytes.
If no such replica exists, the master’s host is returned.

## Installation

You can currently set up and run the project in the following ways:

### Download binary

You can download pre-built linux amd64 binaries from [the Releases section](https://github.com/krylosov-aa/pg-status/releases/).
A dynamically linked binary is available, which requires the necessary dependencies to be installed on your system.
You can read about the required dependencies below, and also take a very clear look at the docker files,
which demonstrate the installation.

A statically linked binary is also provided you can simply download it and run it without any additional setup.

### Run a Docker container

There are several available options:

#### Alpine
- [Fast build, very lightweight container](docker/alpine/Dockerfile_shared)
- [The lightest container, but takes slightly longer to build](docker/alpine/Dockerfile_shared_disabled_https)
- [The heaviest among the lightweight options, but provides a static binary](docker/alpine/Dockerfile_static)

#### Ubuntu
- [With dynamic linking](docker/ubuntu/Dockerfile_shared)
- [Static binary](docker/ubuntu/Dockerfile_static)

The [Makefile](Makefile) contains several ready-to-use commands that you can either run directly or use as a reference.
Each Dockerfile describes a build process (which you can adapt if you’re not using these files) that allows you to
build a binary and either export it to the host or run it directly inside the container.

### Dependencies

This project depends on three external libraries:
- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) under [GNU LGPL v2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)
- [postgresql libpq](https://www.postgresql.org/docs/current/libpq.html)
- [CJson](https://github.com/DaveGamble/cJSON)


### Build with CMake

You can compile the project from source for any platform using CMake.
Required dependencies: libmicrohttpd, cJSON, and libpq.
You can refer to the Dockerfiles for examples of how to install dependencies and configure the build,
depending on whether you prefer a dynamically linked or static binary.


## Testing the service

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

Builds [the lightweight container](docker/alpine/Dockerfile_shared) using parameters defined in [test/docker-compose.yml](test/docker-compose.yml).

In addition to the main service, this setup launches two PostgreSQL instances — one acting as the master and the other as the replica.
To simulate host failover or disconnection, proxy services are used.
This approach allows you to test master-switch scenarios without actually restarting PostgreSQL — you can simply switch the proxy’s target.

Helper shell scripts are provided for this purpose:
- [test/pg-proxy-1_is_master.sh](test/pg-proxy-1_is_master.sh)
- [test/pg-proxy-2_is_master.sh](test/pg-proxy-2_is_master.sh)


## Performance

The measurements were taken inside a Docker container running the 3.20 version of alpine.

The load was generated from the host machine using `hey` for 30 seconds with 2 threads.

- CPU - 0.1
- Memory - 6Mib

Results:
```
Summary:
  Total:        30.0296 secs
  Slowest:      0.0719 secs
  Fastest:      0.0007 secs
  Average:      0.0013 secs
  Requests/sec: 1542.4118

  Total data:   2454854 bytes
  Size/request: 53 bytes

Response time histogram:
  0.001 [1]     |
  0.008 [45798] |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.015 [179]   |
  0.022 [168]   |
  0.029 [82]    |
  0.036 [34]    |
  0.043 [30]    |
  0.051 [17]    |
  0.058 [5]     |
  0.065 [2]     |
  0.072 [2]     |


Latency distribution:
  10% in 0.0010 secs
  25% in 0.0010 secs
  50% in 0.0010 secs
  75% in 0.0011 secs
  90% in 0.0012 secs
  95% in 0.0013 secs
  99% in 0.0102 secs

Details (average, fastest, slowest):
  DNS+dialup:   0.0002 secs, 0.0007 secs, 0.0719 secs
  DNS-lookup:   0.0000 secs, 0.0000 secs, 0.0000 secs
  req write:    0.0000 secs, 0.0000 secs, 0.0004 secs
  resp wait:    0.0011 secs, 0.0005 secs, 0.0716 secs
  resp read:    0.0000 secs, 0.0000 secs, 0.0005 secs

Status code distribution:
  [200] 46318 responses
```
