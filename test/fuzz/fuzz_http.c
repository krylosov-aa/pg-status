/** Exercise production query decoding/parsing with arbitrary escaped bytes. */
#include <event2/event.h>
#include <event2/http.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_server.h"
#include "logger.h"

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static HTTPServer *server;
static struct event_base *client_base;
static struct evhttp_connection *client;

static void parse_query(const HTTPRequest *request, HTTPResponse *response) {
  uint64_t value;
  (void)parse_get_param_uint(request, "lag_ms", &value);
  (void)parse_get_param_uint(request, "lag_bytes", &value);
  (void)parse_get_param_lsn(request, "min_lsn", &value);
  http_response_set_borrowed_body(response, "ok", "text/plain");
}

static void cleanup(void) {
  evhttp_connection_free(client);
  event_base_free(client_base);
  stop_http_server(server);
  pg_status_log_shutdown();
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
  (void)argc;
  (void)argv;
  pg_status_log_init();
  pg_status_log_set_level(PG_STATUS_LOG_FATAL);
  static const Route routes[] = {{"/", parse_query}};
  server = start_http_server("127.0.0.1", 0, routes, 1);
  client_base = event_base_new();
  if (!client_base) {
    abort();
  }
  client = evhttp_connection_base_new(
    client_base, nullptr, "127.0.0.1", http_server_port(server)
  );
  if (!client) {
    abort();
  }
  evhttp_connection_set_timeout(client, 2);
  if (atexit(cleanup) != 0) {
    abort();
  }
  return 0;
}

static void received(struct evhttp_request *request, void *context) {
  (void)context;
  if (!request) {
    abort();
  }
  (void)event_base_loopbreak(client_base);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
  if (size > 512) {
    return 0;
  }
  char encoded[1537];
  for (size_t i = 0; i < size; i++) {
    (void)snprintf(encoded + i * 3, 4, "%%%02X", (unsigned int)data[i]);
  }
  encoded[size * 3] = '\0';
  char path[4700];
  (void)snprintf(
    path, sizeof(path), "/?lag_ms=%s&lag_bytes=%s&min_lsn=%s", encoded, encoded,
    encoded
  );
  // Reuse one connection so fuzzing does not exhaust ephemeral TCP ports.
  struct evhttp_request *request = evhttp_request_new(received, nullptr);
  if (!request) {
    abort();
  }
  if (
    evhttp_add_header(
      evhttp_request_get_output_headers(request), "Host", "localhost"
    ) != 0 ||
    evhttp_make_request(client, request, EVHTTP_REQ_GET, path) != 0
  ) {
    abort();
  }
  if (event_base_dispatch(client_base) < 0) {
    abort();
  }
  return 0;
}
