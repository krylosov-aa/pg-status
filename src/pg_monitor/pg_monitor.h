#ifndef PG_STATUS_PG_MONITOR_H
#define PG_STATUS_PG_MONITOR_H

# define MAX_HOSTS 10

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


typedef struct ReplicaStatus {
    char *host;
} ReplicaStatus;

typedef struct MonitorStatus {
    char *master;
    ReplicaStatus *replicas;
    unsigned int replicas_cnt;
    // TODO may be need refcounter
} MonitorStatus;

ConnectionStrings get_connection_strings(void);

void check_hosts(ConnectionStrings con_str_list);

const MonitorStatus *get_cur_stat(void);

void *pg_monitor_thread(void *arg);

#endif //PG_STATUS_PG_MONITOR_H
