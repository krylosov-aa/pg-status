# pg-status

The service is currently under development.

A microservice (sidecar) that helps instantly determine the status of your PostgreSQL hosts — including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.


## How It Works
pg-status polls database hosts in the background at a specified interval and exposes an HTTP
interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.


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
