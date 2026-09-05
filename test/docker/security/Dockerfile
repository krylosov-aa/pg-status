# TLS assets and PostgreSQL used only by connection-security e2e tests.

FROM postgres:16 AS cert-generator

COPY generate-certs.sh /usr/local/bin/generate-certs
RUN chmod 0755 /usr/local/bin/generate-certs
ENTRYPOINT ["/usr/local/bin/generate-certs"]

FROM postgres:16

COPY security-entrypoint.sh \
    /docker-entrypoint-initdb.d/security-entrypoint.sh
RUN chmod 0755 /docker-entrypoint-initdb.d/security-entrypoint.sh
