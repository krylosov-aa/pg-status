#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

# define MAX_HOSTS 10
#include <sys/_pthread/_pthread_t.h>

typedef struct Hosts {
    char *hosts[MAX_HOSTS];
    unsigned int cnt;
} Hosts;

typedef struct ConnectionStrings {
    char *connection_str[MAX_HOSTS];
    char *hosts[MAX_HOSTS];
    unsigned int cnt;
} ConnectionStrings;

typedef struct MonitorParameters {
    char *user;
    char *password;
    char *database;
    char *hosts_delimiter;
    char *hosts;
    char *port;
    char *connect_timeout;
    unsigned int sleep;
} MonitorParameters;


typedef struct HostLiveness {
    char *host;
    bool alive;
} HostLiveness;

typedef struct MonitorStatus {
    char *master;
    char **replicas;
    HostLiveness *liveness;
    unsigned int cnt;
    // TODO may be need refcounter
} MonitorStatus;

ConnectionStrings get_connection_strings(void);

void check_hosts(ConnectionStrings con_str_list);

const MonitorStatus *get_cur_stat(void);

pthread_t start_pg_monitor();

void stop_pg_monitor(void);

#endif //PG_STATUS_PG_MONITOR_H
