#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

# define MAX_HOSTS 10
#include <pthread.h>

int is_host_in_recovery(const char* connection_str, bool* result);

typedef struct MonitorParameters {
    char *user;
    char *password;
    char *database;
    char *hosts_delimiter;
    char *hosts;
    char *port;
    char *connect_timeout;
    unsigned long long sync_max_lag_ms;
    unsigned long long sync_max_lag_bytes;
    unsigned int sleep;
    unsigned int max_fails;
} MonitorParameters;

typedef struct MonitorStatus {
    unsigned long long delay_ms;
    unsigned long long delay_bytes;
    bool is_master;
    bool alive;
} MonitorStatus;

typedef struct MonitorHost {
    char *host;
    char *connection_str;
    struct MonitorHost *next;
    _Atomic(MonitorStatus *) status;
    _Atomic(MonitorStatus *) not_actual_status;
    unsigned int failed_connections;
} MonitorHost;

pthread_t start_pg_monitor();

void stop_pg_monitor(void);

MonitorHost *get_monitor_host_head(void);

MonitorStatus *atomic_get_status(const MonitorHost *host);

char *round_robin_replica(void);

void check_host_streaming_replication(
    MonitorHost *host, const unsigned int max_fails
);

typedef bool (*condition_handler)(const MonitorStatus *);

char *find_host(
    const condition_handler handler, const bool master_if_not_found
);

bool is_master(const MonitorStatus *status);
bool is_alive_replica(const MonitorStatus *status);
bool is_sync_replica_by_time(const MonitorStatus *status);
bool is_sync_replica_by_bytes(const MonitorStatus *status);
bool is_sync_replica_by_time_or_bytes(const MonitorStatus *status);
bool is_sync_replica_by_time_and_bytes(const MonitorStatus *status);

#endif //PG_STATUS_PG_MONITOR_H
