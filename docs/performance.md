# Performance

For a repeatable fixed-rate localhost benchmark with an application-like route
mix and automatic SLO checks, see the
[manual sustainable-RPS benchmark](../test/rps/README.md).

The measurements were taken using
[this Docker compose setup](../test/docker/docker-compose.yml) with
[this container](../docker/alpine/Dockerfile_shared).

Host machine:
- Ubuntu 24.04 LTS
- Intel Ice Lake
- vCPU: 4
- RAM: 8 GB
- Container memory limit: - 9Mib
- Container CPU limit: - set to 1 or 0.1 - see below

The load was generated from the host machine using `hey`.

I decided to provide only the `/master` and `/hosts`.
Since the `/master` is the fastest, and the `/hosts` takes the slowest due to json serialization.

## CPU - 1

### /master

```
hey -c 200 -z 30s http://localhost:8000/master

Summary:
  Total:        30.0128 secs
  Slowest:      0.0848 secs
  Fastest:      0.0007 secs
  Average:      0.0222 secs
  Requests/sec: 8996.1888

  Total data:   2700010 bytes
  Size/request: 10 bytes

Response time histogram:
  0.001 [1]     |
  0.009 [8264]  |■■■
  0.018 [74037] |■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.026 [109683]        |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.034 [56365] |■■■■■■■■■■■■■■■■■■■■■
  0.043 [16994] |■■■■■■
  0.051 [3497]  |■
  0.060 [828]   |
  0.068 [247]   |
  0.076 [66]    |
  0.085 [19]    |


Latency distribution:
  10% in 0.0126 secs
  25% in 0.0164 secs
  50% in 0.0212 secs
  75% in 0.0270 secs
  90% in 0.0331 secs
  95% in 0.0371 secs
  99% in 0.0460 secs

Details (average, fastest, slowest):
  DNS+dialup:   0.0035 secs, 0.0007 secs, 0.0848 secs
  DNS-lookup:   0.0019 secs, 0.0000 secs, 0.0318 secs
  req write:    0.0014 secs, 0.0000 secs, 0.0340 secs
  resp wait:    0.0149 secs, 0.0001 secs, 0.0831 secs
  resp read:    0.0024 secs, 0.0000 secs, 0.0257 secs

Status code distribution:
  [200] 270001 responses
```

### /hosts

```
hey -c 200 -z 30s http://localhost:8000/hosts

Summary:
  Total:        30.0089 secs
  Slowest:      0.0885 secs
  Fastest:      0.0009 secs
  Average:      0.0232 secs
  Requests/sec: 8610.6892

  Total data:   38795305 bytes
  Size/request: 150 bytes

Response time histogram:
  0.001 [1]     |
  0.010 [7230]  |■■■
  0.018 [70173] |■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.027 [108643]        |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.036 [52437] |■■■■■■■■■■■■■■■■■■■
  0.045 [15441] |■■■■■■
  0.053 [3575]  |■
  0.062 [794]   |
  0.071 [74]    |
  0.080 [20]    |
  0.088 [9]     |


Latency distribution:
  10% in 0.0135 secs
  25% in 0.0174 secs
  50% in 0.0222 secs
  75% in 0.0280 secs
  90% in 0.0343 secs
  95% in 0.0385 secs
  99% in 0.0479 secs

Details (average, fastest, slowest):
  DNS+dialup:   0.0034 secs, 0.0009 secs, 0.0885 secs
  DNS-lookup:   0.0018 secs, 0.0000 secs, 0.0310 secs
  req write:    0.0014 secs, 0.0000 secs, 0.0370 secs
  resp wait:    0.0161 secs, 0.0001 secs, 0.0831 secs
  resp read:    0.0023 secs, 0.0000 secs, 0.0355 secs

Status code distribution:
  [200] 258397 responses
```


## CPU - 0.1

### /master

```
hey -c 200 -z 30s http://localhost:8000/master

Summary:
  Total:        30.1518 secs
  Slowest:      0.2809 secs
  Fastest:      0.0026 secs
  Average:      0.0965 secs
  Requests/sec: 2066.0455

  Total data:   622950 bytes
  Size/request: 10 bytes

Response time histogram:
  0.003 [1]     |
  0.030 [5152]  |■■■■
  0.058 [190]   |
  0.086 [183]   |
  0.114 [53326] |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.142 [385]   |
  0.170 [0]     |
  0.197 [2346]  |■■
  0.225 [710]   |■
  0.253 [0]     |
  0.281 [2]     |


Latency distribution:
  10% in 0.0883 secs
  25% in 0.0940 secs
  50% in 0.0983 secs
  75% in 0.1029 secs
  90% in 0.1082 secs
  95% in 0.1173 secs
  99% in 0.1982 secs

Details (average, fastest, slowest):
  DNS+dialup:   0.0029 secs, 0.0026 secs, 0.2809 secs
  DNS-lookup:   0.0016 secs, 0.0000 secs, 0.0173 secs
  req write:    0.0010 secs, 0.0000 secs, 0.0195 secs
  resp wait:    0.0904 secs, 0.0002 secs, 0.2791 secs
  resp read:    0.0022 secs, 0.0000 secs, 0.0207 secs

Status code distribution:
  [200] 62295 responses
```

### /hosts

```
hey -c 200 -z 30s http://localhost:8000/hosts

Summary:
  Total:        30.0999 secs
  Slowest:      0.4046 secs
  Fastest:      0.0055 secs
  Average:      0.1198 secs
  Requests/sec: 1664.7571
                                                                                                                                                Total data:   7516350 bytes
  Size/request: 150 bytes

Response time histogram:
  0.005 [1]     |
  0.045 [533]   |■
  0.085 [157]   |
  0.125 [41044] |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  0.165 [0]     |
  0.205 [5449]  |■■■■■
  0.245 [872]   |■
  0.285 [65]    |
  0.325 [1962]  |■■
  0.365 [0]     |
  0.405 [26]    |


Latency distribution:
  10% in 0.0952 secs
  25% in 0.0984 secs
  50% in 0.1020 secs
  75% in 0.1073 secs
  90% in 0.1931 secs
  95% in 0.2069 secs
  99% in 0.2990 secs

Details (average, fastest, slowest):
  DNS+dialup:   0.0024 secs, 0.0055 secs, 0.4046 secs
  DNS-lookup:   0.0012 secs, 0.0000 secs, 0.0207 secs
  req write:    0.0008 secs, 0.0000 secs, 0.0137 secs
  resp wait:    0.1148 secs, 0.0003 secs, 0.3906 secs
  resp read:    0.0017 secs, 0.0000 secs, 0.0141 secs

Status code distribution:
  [200] 50109 responses
```
