#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <cjson/cJSON.h>
#include <getopt.h>

void add_host_to_json(cJSON *json_obj, char *host) {
    if (!host) {
        add_null_to_json_object(json_obj, "host");
    }
    else {
        add_str_to_json_object(json_obj, "host", host);
    }
}

void get_all_hosts(MHD_Connection *connection, HTTPResponse *response) {
    const MonitorHost *mon_host = get_monitor_host_head();
    cJSON *arr = json_array();

    while (mon_host) {
        cJSON *json_obj = json_object();
        add_host_to_json(json_obj, mon_host -> host);

        MonitorStatus status = atomic_get_status(mon_host);
        add_bool_to_json_object(json_obj, "master", status.master);
        add_bool_to_json_object(json_obj, "alive", status.alive);

        cJSON_AddItemToArray(arr, json_obj);
        mon_host = mon_host -> next;
    }

    response -> response = json_to_str(arr);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> content_type = "application/json";
}

void return_single_host(HTTPResponse *response, char *host) {
    if (!host) {
        response -> status_code = 404;
    }

    if (need_json_response(response)) {
        cJSON *json_obj = json_object();
        add_host_to_json(json_obj, host);
        response -> response = json_to_str(json_obj);
        response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    }
    else {
        response -> response = host;
        response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    }
}

void get_random_replica(MHD_Connection *connection, HTTPResponse *response) {
    char *host = find_host_round_robin(is_alive_replica, true);
    return_single_host(response, host);
}

void get_master(MHD_Connection *connection, HTTPResponse *response) {
    char *host = get_master_host();
    return_single_host(response, host);
}

void get_sync_host_by_time(
    MHD_Connection *connection, HTTPResponse *response
) {
    char *host = find_host_round_robin(is_sync_replica_by_time, true);
    return_single_host(response, host);
}

void get_sync_host_by_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    char *host = find_host_round_robin(is_sync_replica_by_bytes, true);
    return_single_host(response, host);
}

void get_sync_host_by_time_or_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    char *host = find_host_round_robin(is_sync_replica_by_time_or_bytes, true);
    return_single_host(response, host);
}

void get_sync_host_by_time_and_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    char *host = find_host_round_robin(is_sync_replica_by_time_and_bytes, true);
    return_single_host(response, host);
}


void get_host_status(MHD_Connection *connection, HTTPResponse *response) {
    const char *host = MHD_lookup_connection_value(
        connection,
        MHD_GET_ARGUMENT_KIND,
        "host"
    );
    if (!host) {
        response -> status_code = 400;
        response -> response = "{\"error_text\": \"Get parameter 'host' wasn't passed\"}";
        response -> memory_mode = MHD_RESPMEM_PERSISTENT;
        return;
    }
    MonitorHost *mon_host = find_host_by_name(host);
    if (!mon_host) {
        response -> status_code = 404;
        return;
    }

    const MonitorStatus status = atomic_get_status(mon_host);
    cJSON *json_obj = json_object();
    add_bool_to_json_object(json_obj, "master", status.master);
    add_bool_to_json_object(json_obj, "alive", status.alive);
    bool sync_by_time = false;
    bool sync_by_bytes = false;
    if (status.master) {
        sync_by_time = true;
        sync_by_bytes = true;
    }
    else {
        sync_by_time = is_sync_replica_by_time(&status);
        sync_by_bytes = is_sync_replica_by_bytes(&status);
    }
    add_bool_to_json_object(json_obj, "sync_by_time", sync_by_time);
    add_bool_to_json_object(json_obj, "sync_by_bytes", sync_by_bytes);

    response -> response = json_to_str(json_obj);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> content_type = "application/json";
}

void block_termination_signals(sigset_t *sigset) {
    sigemptyset(sigset);
    sigaddset(sigset, SIGINT);
    sigaddset(sigset, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, sigset, NULL) != 0) {
        raise_error("pthread_sigmask");
    }
}

void wait_for_termination_signal(const sigset_t *sigset) {
    int sig;

    if (sigwait(sigset, &sig) == 0) {
        switch (sig) {
            case SIGINT:
                printf("Received SIGINT\n");
                break;
            case SIGTERM:
                printf("Received SIGTERM\n");
                break;
            default:
                printf("Received signal %d\n", sig);
        }
    }
}

typedef struct {
    uint16_t port;
} Config;

noreturn void print_help(void) {
    printf(
        "Options:\n"
        "  -port,  --port PORT   HTTP Server port (default: 8000)\n"
        "  -help , --help        Show this help message\n"
    );
    exit(EXIT_SUCCESS);
}

Config parse_args(int argc, char *argv[]) {
    Config config = {
        .port = 8000,
    };

    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"help", no_argument,       0, 'h'},
        {0,      0,                 0,  0 }
    };

    int opt;
    while ((opt = getopt_long_only(argc, argv, "", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'p':
                config.port = str_to_uint16(optarg);
                break;
            default:
                print_help();
        }
    }

    return config;
}

Route routes[] = {
    { "GET", "/master", get_master },
    { "GET", "/replica", get_random_replica },
    { "GET", "/hosts", get_all_hosts },
    { "GET", "/status", get_host_status },
    { "GET", "/sync_by_time", get_sync_host_by_time },
    { "GET", "/sync_by_bytes", get_sync_host_by_bytes },
    { "GET", "/sync_by_time_or_bytes", get_sync_host_by_time_or_bytes },
    { "GET", "/sync_by_time_and_bytes", get_sync_host_by_time_and_bytes },
};

int main(int argc, char *argv[]) {
    sigset_t sigset;
    block_termination_signals(&sigset);

    const Config conf = parse_args(argc, argv);

    start_pg_monitor();
    MHD_Daemon *daemon = start_http_server(
        conf.port, routes, sizeof(routes) / sizeof(routes[0])
    );

    wait_for_termination_signal(&sigset);

    stop_pg_monitor();
    stop_http_server(daemon);
    return 0;
}
