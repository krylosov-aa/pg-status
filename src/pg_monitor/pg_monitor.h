#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

# define MAX_HOSTS 10
#include <pthread.h>
#include <stdatomic.h>

int is_host_in_recovery(const char* connection_str, bool* result);

typedef struct MonitorParameters {
    char *user;
    char *password;
    char *database;
    char *hosts_delimiter;
    char *hosts;
    char *port;
    char *connect_timeout;
    unsigned int sleep;
    unsigned int max_fails;
} MonitorParameters;

typedef struct MonitorStatus {
    char *host;
    char *connection_str;
    struct MonitorStatus *next;
    unsigned int failed_connections;
    atomic_bool is_master;
    atomic_bool alive;
} MonitorStatus;

pthread_t start_pg_monitor();

void stop_pg_monitor(void);

char *get_master_host(void);

MonitorStatus *get_monitor_status(void);

bool get_bool_atomic(const atomic_bool *ptr);

bool is_alive_replica(const MonitorStatus *host);

char *round_robin_replica(void);

#endif //PG_STATUS_PG_MONITOR_H
