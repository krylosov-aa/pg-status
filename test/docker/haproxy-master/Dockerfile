# HAProxy image initially routing traffic to the master.

FROM haproxy:2.9

USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends socat \
 && rm -rf /var/lib/apt/lists/* \
 && install -d -o haproxy -g haproxy /run/haproxy

USER haproxy

COPY haproxy.cfg /usr/local/etc/haproxy/haproxy.cfg

CMD ["haproxy", "-f", "/usr/local/etc/haproxy/haproxy.cfg", "-W", "-db"]
