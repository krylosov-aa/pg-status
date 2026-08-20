build_static_alpine:
	docker build -f docker/alpine/Dockerfile_static -t pg-status-static-alpine .

build_shared_alpine:
	docker build -f docker/alpine/Dockerfile_shared -t pg-status-shared-alpine .

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

build_push:
	sudo docker buildx build \
		--platform linux/amd64 \
		--provenance=false \
		--sbom=false \
		-f docker/alpine/Dockerfile_shared \
		-t ${r}/pg-status:${v} \
		-t ${r}/pg-status:latest \
		--push .

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
	docker-compose -p test -f test/docker/docker-compose.yml --profile pg-status up -d

build_up_test_only_pg:
	make down_test
	docker-compose -p test -f test/docker/docker-compose.yml up -d

down_test:
	docker-compose -p test -f test/docker/docker-compose.yml --profile pg-status down --remove-orphans

1-master:
	./test/docker/pg-proxy-1_is_master.sh

2-master:
	./test/docker/pg-proxy-2_is_master.sh



scan-build:
	scan-build -o scan_reports cmake --build cmake-build-release

clean:
	 cmake --build cmake-build-debug --verbose --target clean

clean_release:
	 cmake --build cmake-build-release --verbose --target clean

build_valgrind:
	docker build -f test/valgrind/Dockerfile -t pg-status-valgrind .

docker-compose := $(shell docker compose version -f docker-compose.yml &> /dev/null && echo docker compose -f docker-compose.yml || echo docker-compose -f docker-compose.yml)

colima_start:
	colima start --arch aarch64 --vm-type=vz --vz-rosetta --cpu 6 --memory 8

build_push_amd_64:
	sudo docker build --platform linux/amd64 -f docker/alpine/Dockerfile_shared -t pg-status:${v} .
	sudo docker tag pg-status:${v} ${r}/pg-status:${v}
	sudo docker tag pg-status:${v} ${r}/pg-status:latest
	sudo docker push ${r}/pg-status:${v}
	sudo docker push ${r}/pg-status:latest

format-warn:
	cmake --build cmake-build-debug --target format-warn

benchmark_rps:
	./test/rps/run.sh
