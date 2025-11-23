# pg-status

The service is currently under development.

A C microservice that helps your web application instantly determine the state of your PostgreSQL hosts — identifying which one is the master and which are replicas.


## How It Works
pg-status polls database hosts in the background at a specified interval and exposes an interface that can be used to retrieve a list of hosts meeting given conditions.

It always serves data directly from memory and responds extremely quickly, so it can be safely used on every request.
