# pg-status

A microservice (sidecar) that helps instantly determine the status of your PostgreSQL hosts including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.

pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.


## Using

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

If you include the header Accept: application/json, the response will be in JSON format, for example: `{"host": "localhost"}`

If you omit this header, the response will be plain text: `localhost`

If the API cannot find a matching request, it will return a 404 status code.
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


## Launch / Testing

Currently, you can set up and run the project in the following ways:

- Build with CMake
- Run inside a Docker container ([using the provided Dockerfile](Dockerfile))

For convenience, you can also use [make commands](Makefile) to build and run the project, including two available
Docker variants:

### make build_up

Builds and runs the container as defined in [docker-compose.yml](docker-compose.yml).
Within this configuration, you can set environment variables and test the service with your own database.

### make build_up_test

Builds and runs the container as defined in [test/docker-compose.yml](test/docker-compose.yml).

In addition to the main service, this setup launches two PostgreSQL instances: one acting as the master and the other as a replica.
To simulate host failover or disconnection, proxy services are used.
This approach allows you to test master-switch scenarios without actually stopping PostgreSQL — you can simply switch the proxy’s target instead.

Helper shell scripts are provided for this purpose:
- [test/pg-proxy-1_is_master.sh](test/pg-proxy-1_is_master.sh)
- [test/pg-proxy-2_is_master.sh](test/pg-proxy-2_is_master.sh)
