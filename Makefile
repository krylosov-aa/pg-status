build_static_alpine:
	docker build -f docker/alpine/Dockerfile_static -t pg-status-static-alpine .

build_shared_alpine:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status-shared-alpine .

build_shared_alpine_wo_https:
	docker build -f docker/alpine/Dockerfile_shared_disabled_https -t pg-status-shared-alpine-wo-https .

build_shared_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_shared -t pg-status-shared-ubuntu .

build_static_ubuntu:
	docker build -f docker/ubuntu/Dockerfile_static -t pg-status-static-ubuntu .

build_shared_executable:
	sudo docker build -f docker/ubuntu/Dockerfile_shared --target export -o out/shared .
	sudo chown -R $$(id -u):$$(id -g) out/shared

build_static_executable:
	sudo docker build -f docker/ubuntu/Dockerfile_static --target export -o out/static .
	sudo chown -R $$(id -u):$$(id -g) out/static

build_deb:
	sudo docker build -f docker/ubuntu/Dockerfile_deb --target export -o out/deb .
	sudo chown -R $$(id -u):$$(id -g) out/deb

build_publish:
	sudo docker build -f docker/alpine/Dockerfile_shared -t pg-status:${v} .
	sudo docker tag pg-status:${v} krylosovaa/pg-status:${v}
	sudo docker tag pg-status:${v} krylosovaa/pg-status:latest
	sudo docker push krylosovaa/pg-status:${v}
	sudo docker push krylosovaa/pg-status:latest


build:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status .

up:
	$(docker-compose) up -d

down:
	$(docker-compose) down --remove-orphans

build_up:
	make down
	make build
	make up

build_up_test:
	make down_test
	make build
	docker-compose -f test/docker-compose.yml up -d

down_test:
	docker-compose -f test/docker-compose.yml down --remove-orphans

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

docker-compose := $(shell docker compose version -f docker-compose.yml &> /dev/null && echo docker compose -f docker-compose.yml || echo docker-compose -f docker-compose.yml)

colima_start:
	colima start --arch aarch64 --vm-type=vz --vz-rosetta --cpu 6 --memory 8
