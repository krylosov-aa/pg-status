docker-compose := $(shell docker compose version -f docker-compose.yml &> /dev/null && echo docker compose -f docker-compose.yml || echo docker-compose -f docker-compose.yml)

colima_start:
	colima start --arch aarch64 --vm-type=vz --vz-rosetta --cpu 6 --memory 8

build:
	docker build --platform linux/amd64 -t pg-status .

up:
	$(docker-compose) up -d

down:
	$(docker-compose) down --remove-orphans

build_up:
	make build
	make up

down_test:
	docker-compose -f test/docker-compose.yml down --remove-orphans

build_up_test:
	make down_test
	make build
	docker-compose -f test/docker-compose.yml up -d

1-master:
	./test/pg-proxy-1_is_master.sh

2-master:
	./test/pg-proxy-2_is_master.sh

scan-build:
	scan-build -o scan_reports cmake --build cmake-build-release

clean:
	 cmake --build cmake-build-debug --verbose --target clean

build_push:
	docker build --platform linux/amd64 -t pg-status:${v} .
	docker tag pg-status:${v} ${r}pg-status:${v}
	docker push ${r}pg-status:${v}

build_valgrind:
	docker build -f test/valgrind/Dockerfile -t pg-status-valgrind .

build_static_alpine:
	docker build -f docker/alpine/Dockerfile_static --target export -o out .

build_static_alpine_linux:
	docker buildx build \
	  --platform linux/amd64 \
	  -f docker/alpine/Dockerfile_static \
	  --target export \
	  -o out .

build_shared_alpine:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status-shared-alpine .

build_shared_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_shared -t pg-status-shared-ubuntu .

build_shared_wo_https_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_shared_disabled_https -t pg-status-shared-wo-https-ubuntu .

build_static_ubuntu:
	docker build --platform linux/amd64 -f docker/ubuntu/Dockerfile_static -t pg-status-static-ubuntu .
