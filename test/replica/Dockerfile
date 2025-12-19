FROM postgres:16

ENV POSTGRES_USER=postgres
ENV POSTGRES_PASSWORD=postgres
ENV POSTGRES_DB=postgres
ENV MASTER_HOST=postgres_master
ENV MASTER_USER=postgres
ENV MASTER_PASSWORD=postgres
ENV REPLICATION_ROLE=replica

COPY replica-entrypoint.sh /usr/local/bin/replica-entrypoint.sh
RUN chmod +x /usr/local/bin/replica-entrypoint.sh
ENTRYPOINT ["replica-entrypoint.sh"]
