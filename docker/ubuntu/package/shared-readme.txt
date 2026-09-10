pg-status v2.2.0
================

An HTTP service that reports the observed roles, availability and replication
lag of PostgreSQL hosts. It polls hosts in the background and serves the
latest results from memory.

Requirements
------------
This binary requires the following shared libraries:
  - libpq5
  - libevent-2.1-7t64
  - libcjson1

For more information, visit:
  https://github.com/krylosov-aa/pg-status
