docker exec -i pg-proxy-1 socat stdio /tmp/haproxy.sock <<< "enable server pg_backends/master"
docker exec -i pg-proxy-1 socat stdio /tmp/haproxy.sock <<< "disable server pg_backends/replica"

docker exec -i pg-proxy-2 socat stdio /tmp/haproxy.sock <<< "enable server pg_backends/replica"
docker exec -i pg-proxy-2 socat stdio /tmp/haproxy.sock <<< "disable server pg_backends/master"
