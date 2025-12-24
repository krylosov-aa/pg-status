docker-compose := $(shell docker compose version -f docker-compose.yml &> /dev/null && echo docker compose -f docker-compose.yml || echo docker-compose -f docker-compose.yml)

colima_start:
	colima start --arch aarch64 --vm-type=vz --vz-rosetta --cpu 6 --memory 8

build:
	docker build -t pg-status .

up:
	$(docker-compose) up -d

down:
	$(docker-compose) down --remove-orphans

build_up:
	make build
	make up

build_up_test:
	make build
	docker-compose -f test/docker-compose.yml up -d

1-master:
	./test/pg-proxy-1_is_master.sh

2-master:
	./test/pg-proxy-2_is_master.sh
