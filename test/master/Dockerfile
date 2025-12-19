FROM postgres:16

ENV POSTGRES_USER=postgres
ENV POSTGRES_PASSWORD=postgres
ENV POSTGRES_DB=postgres
ENV REPLICATION_ROLE=master

COPY master-entrypoint.sh /docker-entrypoint-initdb.d/master-entrypoint.sh
RUN chmod +x /docker-entrypoint-initdb.d/master-entrypoint.sh
