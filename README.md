# pg-status

The service is currently under development.

A C microservice that helps instantly determine the status of your PostgreSQL hosts — including whether they are alive,
which one is the master, which ones are replicas, and how far each replica is lagging behind the master.

It’s designed as a sidecar that runs alongside your main application. It’s
lightweight, resource-efficient, and delivers high performance.
You can access it on every request without noticeable overhead.


## How It Works
pg-status polls database hosts in the background at a specified interval and exposes an interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.
