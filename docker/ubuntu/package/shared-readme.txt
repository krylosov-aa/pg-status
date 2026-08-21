pg-status v2.1.1
================

A microservice (sidecar) that helps instantly determine the status of your
PostgreSQL hosts including whether they are alive, which one is the master,
which ones are replicas, and how far each replica is lagging behind the master.

Requirements
------------
This binary requires the following shared libraries:
  - libpq5
  - libevent-2.1-7t64
  - libcjson1

For more information, visit:
  https://github.com/krylosov-aa/pg-status
