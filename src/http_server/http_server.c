/**
 * HTTP server
 */

#include "http_server.h"

#include <arpa/inet.h>
#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/listener.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/queue.h>
#include <unistd.h>

#include "logger.h"
#include "utils.h"

enum {
  HTTP_LISTEN_BACKLOG = 512,
  HTTP_MAX_HEADERS_SIZE = 8 * 1024,
  HTTP_IO_TIMEOUT_SECONDS = 5,
  HTTP_LISTENER_OPTIONS = LEV_OPT_CLOSE_ON_FREE | LEV_OPT_CLOSE_ON_EXEC |
                          LEV_OPT_REUSEABLE,
};

struct HTTPRequest {
  struct evhttp_request *request;
  struct evkeyvalq query_params;
};

typedef enum {
  HTTP_BODY_NONE,
  HTTP_BODY_BORROWED,
  HTTP_BODY_OWNED,
} HTTPBodyKind;

struct HTTPResponse {
  union {
    const char *borrowed;
    char *owned;
  } body;
  HTTPBodyKind body_kind;
  const char *content_type;
  response_body_cleanup_t body_cleanup;
  unsigned int status_code;
};

struct HTTPServer {
  struct event_base *event_base;
  struct evhttp *http;
  struct event *stop_event;
  const Route *routes;
  size_t cnt_routes;
  pthread_t thread;
  pthread_mutex_t start_mutex;
  pthread_cond_t start_cond;
  bool event_loop_started;
  int stop_pipe[2];
  uint16_t port;
};

static HTTPServer *init_http_server(
  const Route *routes, const size_t cnt_routes
) {
  HTTPServer *server = malloc(sizeof(*server));
  if (!server) {
    const int error_number = errno != 0 ? errno : ENOMEM;
    pg_status_log_system_fatal(
      "http", error_number, "failed to allocate HTTP server"
    );
  }

  *server = (HTTPServer){
    .event_base = nullptr,
    .http = nullptr,
    .stop_event = nullptr,
    .routes = routes,
    .cnt_routes = cnt_routes,
    .event_loop_started = false,
    .stop_pipe = {-1, -1},
    .port = 0,
  };

  int result = pthread_mutex_init(&server->start_mutex, nullptr);
  if (result != 0) {
    free(server);
    pg_status_log_system_fatal(
      "http", result, "failed to initialize HTTP server mutex"
    );
  }

  result = pthread_cond_init(&server->start_cond, nullptr);
  if (result != 0) {
    pthread_mutex_destroy(&server->start_mutex);
    free(server);
    pg_status_log_system_fatal(
      "http", result, "failed to initialize HTTP server condition variable"
    );
  }

  return server;
}

static bool is_optional_whitespace(const char ch) {
  return ch == ' ' || ch == '\t';
}

static const char *skip_optional_whitespace(const char *pos, const char *end) {
  while (pos < end && is_optional_whitespace(*pos)) {
    pos++;
  }
  return pos;
}

static const char *trim_optional_whitespace_end(
  const char *begin, const char *end
) {
  while (end > begin && is_optional_whitespace(end[-1])) {
    end--;
  }
  return end;
}

static bool is_zero_quality(const char *value, const char *end) {
  value = skip_optional_whitespace(value, end);
  end = trim_optional_whitespace_end(value, end);
  if (value == end || *value != '0') {
    return false;
  }
  value++;
  if (value < end && *value == '.') {
    value++;
  }
  while (value < end) {
    if (*value != '0') {
      return false;
    }
    value++;
  }
  return true;
}

static bool check_accepts_json_quality(
  const char *media_end, const char *range_end
) {
  const char *param = media_end;
  while (param < range_end) {
    param++;
    const char *param_end = memchr(param, ';', (size_t)(range_end - param));
    if (!param_end) {
      param_end = range_end;
    }
    const char *equals = memchr(param, '=', (size_t)(param_end - param));
    if (equals) {
      const char *name_begin = skip_optional_whitespace(param, equals);
      const char *name_end = trim_optional_whitespace_end(name_begin, equals);
      if (
        (size_t)(name_end - name_begin) == 1 &&
        (*name_begin == 'q' || *name_begin == 'Q')
      ) {
        return !is_zero_quality(equals + 1, param_end);
      }
    }
    param = param_end;
  }
  return true;
}

static bool media_range_accepts_json(
  const char *range_begin, const char *range_end
) {
  const char *media_end = memchr(
    range_begin, ';', (size_t)(range_end - range_begin)
  );
  if (!media_end) {
    media_end = range_end;
  }

  range_begin = skip_optional_whitespace(range_begin, media_end);
  const char *trimmed_media_end = trim_optional_whitespace_end(
    range_begin, media_end
  );
  static const char json_media_type[] = "application/json";
  const size_t media_len = (size_t)(trimmed_media_end - range_begin);
  if (
    media_len != sizeof(json_media_type) - 1 ||
    strncasecmp(range_begin, json_media_type, media_len) != 0
  ) {
    return false;
  }

  return check_accepts_json_quality(media_end, range_end);
}

bool http_request_accepts_json(const HTTPRequest *request) {
  const struct evkeyvalq *headers = evhttp_request_get_input_headers(
    request->request
  );
  const char *accept = evhttp_find_header(headers, "Accept");
  if (!accept) {
    return false;
  }

  const char *range_begin = accept;
  const char *accept_end = accept + strlen(accept);
  while (range_begin < accept_end) {
    const char *range_end = memchr(
      range_begin, ',', (size_t)(accept_end - range_begin)
    );
    if (!range_end) {
      range_end = accept_end;
    }
    if (media_range_accepts_json(range_begin, range_end)) {
      return true;
    }
    range_begin = range_end + (range_end < accept_end ? 1 : 0);
  }
  return false;
}

const char *http_request_get_query_param(
  const HTTPRequest *request, const char *name
) {
  return evhttp_find_header(&request->query_params, name);
}

static void release_response_body(HTTPResponse *response) {
  switch (response->body_kind) {
    case HTTP_BODY_NONE:
    case HTTP_BODY_BORROWED:
      break;
    case HTTP_BODY_OWNED:
      response->body_cleanup(response->body.owned);
      break;
    default:
      pg_status_log_fatal("http", "invalid HTTP response body kind");
  }
  response->body.borrowed = nullptr;
  response->body_kind = HTTP_BODY_NONE;
  response->body_cleanup = nullptr;
}

static const char *get_response_body(const HTTPResponse *response) {
  switch (response->body_kind) {
    case HTTP_BODY_NONE:
      return nullptr;
    case HTTP_BODY_BORROWED:
      return response->body.borrowed;
    case HTTP_BODY_OWNED:
      return response->body.owned;
    default:
      pg_status_log_fatal("http", "invalid HTTP response body kind");
  }
}

void http_response_set_borrowed_body(
  HTTPResponse *response, const char *body, const char *content_type
) {
  release_response_body(response);
  response->body.borrowed = body;
  response->body_kind = HTTP_BODY_BORROWED;
  response->content_type = content_type;
}

void http_response_set_owned_body(
  HTTPResponse *response, char *body, const char *content_type,
  const response_body_cleanup_t cleanup
) {
  if (!cleanup) {
    pg_status_log_fatal(
      "http", "owned HTTP response body cleanup must not be null"
    );
  }
  release_response_body(response);
  response->body.owned = body;
  response->body_kind = HTTP_BODY_OWNED;
  response->content_type = content_type;
  response->body_cleanup = cleanup;
}

void http_response_set_status(
  HTTPResponse *response, const unsigned int status_code
) {
  response->status_code = status_code;
}

static request_handler_t find_handler(
  const HTTPServer *server, const char *path
) {
  for (size_t i = 0; i < server->cnt_routes; i++) {
    if (strcmp(server->routes[i].path, path) == 0) {
      return server->routes[i].handler;
    }
  }
  return nullptr;
}

static void send_response(
  struct evhttp_request *request, HTTPResponse *response
) {
  struct evbuffer *body = evbuffer_new();
  if (!body) {
    release_response_body(response);
    evhttp_send_error(request, HTTP_INTERNAL, nullptr);
    return;
  }

  const char *response_body = get_response_body(response);
  if (response_body) {
    const size_t body_len = strlen(response_body);
    if (evbuffer_add(body, response_body, body_len) != 0) {
      evbuffer_free(body);
      release_response_body(response);
      evhttp_send_error(request, HTTP_INTERNAL, nullptr);
      return;
    }
  }

  if (response->content_type) {
    struct evkeyvalq *headers = evhttp_request_get_output_headers(request);
    if (
      evhttp_add_header(headers, "Content-Type", response->content_type) != 0
    ) {
      evbuffer_free(body);
      release_response_body(response);
      evhttp_send_error(request, HTTP_INTERNAL, nullptr);
      return;
    }
  }

  evhttp_send_reply(request, (int)response->status_code, nullptr, body);
  evbuffer_free(body);
  release_response_body(response);
}

static void send_empty_response(
  struct evhttp_request *request, const unsigned int status_code
) {
  HTTPResponse response = {
    .body = {.borrowed = nullptr},
    .body_kind = HTTP_BODY_NONE,
    .content_type = nullptr,
    .body_cleanup = nullptr,
    .status_code = status_code,
  };
  send_response(request, &response);
}

static bool initialize_request(
  struct evhttp_request *raw_request, HTTPRequest *request, char **decoded_path
) {
  request->request = raw_request;
  TAILQ_INIT(&request->query_params);

  const struct evhttp_uri *uri = evhttp_request_get_evhttp_uri(raw_request);
  if (!uri) {
    return false;
  }
  const char *raw_path = evhttp_uri_get_path(uri);
  if (!raw_path || *raw_path == '\0') {
    raw_path = "/";
  }

  size_t decoded_path_len = 0;
  *decoded_path = evhttp_uridecode(raw_path, 0, &decoded_path_len);
  if (!*decoded_path || strlen(*decoded_path) != decoded_path_len) {
    free(*decoded_path);
    *decoded_path = nullptr;
    return false;
  }

  const char *query = evhttp_uri_get_query(uri);
  if (query && evhttp_parse_query_str(query, &request->query_params) != 0) {
    free(*decoded_path);
    *decoded_path = nullptr;
    evhttp_clear_headers(&request->query_params);
    return false;
  }
  return true;
}

static void process_request(struct evhttp_request *raw_request, void *arg) {
  const HTTPServer *server = arg;

  const int parse_error = evhttp_request_get_response_code(raw_request);
  if (parse_error >= HTTP_BADREQUEST) {
    send_empty_response(raw_request, (unsigned int)parse_error);
    return;
  }

  if (evhttp_request_get_command(raw_request) != EVHTTP_REQ_GET) {
    struct evkeyvalq *headers = evhttp_request_get_output_headers(raw_request);
    (void)evhttp_add_header(headers, "Allow", "GET");
    send_empty_response(raw_request, HTTP_BADMETHOD);
    return;
  }

  HTTPRequest request;
  char *path = nullptr;
  if (!initialize_request(raw_request, &request, &path)) {
    send_empty_response(raw_request, HTTP_BADREQUEST);
    return;
  }

  HTTPResponse response = {
    .body = {.borrowed = nullptr},
    .body_kind = HTTP_BODY_NONE,
    .content_type = nullptr,
    .body_cleanup = nullptr,
    .status_code = HTTP_OK,
  };
  const request_handler_t handler = find_handler(server, path);
  if (handler) {
    handler(&request, &response);
  } else {
    response.status_code = HTTP_NOTFOUND;
  }

  send_response(raw_request, &response);
  evhttp_clear_headers(&request.query_params);
  free(path);
}

static void *run_event_loop(void *arg) {
  HTTPServer *server = arg;
  pthread_mutex_lock(&server->start_mutex);
  server->event_loop_started = true;
  pthread_cond_broadcast(&server->start_cond);
  pthread_mutex_unlock(&server->start_mutex);

  const int result = event_base_dispatch(server->event_base);
  if (result == -1) {
    pg_status_log(PG_STATUS_LOG_ERROR, "http", "event loop failed");
  }
  return nullptr;
}

static void stop_event_callback(
  const evutil_socket_t fd, const short events, void *arg
) {
  HTTPServer *server = arg;
  (void)events;

  char byte;
  ssize_t read_result;
  do {
    read_result = read(fd, &byte, 1);
  } while (read_result < 0 && errno == EINTR);
  if (read_result != 1) {
    const int error_number = read_result < 0 ? errno : EPIPE;
    pg_status_log_system_error(
      PG_STATUS_LOG_ERROR, "http", error_number, "failed to read stop pipe"
    );
  }

  if (event_base_loopbreak(server->event_base) != 0) {
    pg_status_log(PG_STATUS_LOG_ERROR, "http", "failed to stop event loop");
  }
}

static void init_event_base(HTTPServer *server) {
  server->event_base = event_base_new();
  if (!server->event_base) {
    pg_status_log_fatal("http", "failed to create event base");
  }
}

static void init_evhttp(HTTPServer *server) {
  server->http = evhttp_new(server->event_base);
  if (!server->http) {
    pg_status_log_fatal("http", "failed to create evhttp server");
  }

  evhttp_set_allowed_methods(
    server->http, EVHTTP_REQ_GET | EVHTTP_REQ_POST | EVHTTP_REQ_HEAD |
                    EVHTTP_REQ_PUT | EVHTTP_REQ_DELETE | EVHTTP_REQ_OPTIONS |
                    EVHTTP_REQ_TRACE | EVHTTP_REQ_CONNECT | EVHTTP_REQ_PATCH
  );
  evhttp_set_max_headers_size(server->http, HTTP_MAX_HEADERS_SIZE);
  evhttp_set_max_body_size(server->http, 0);
  evhttp_set_timeout(server->http, HTTP_IO_TIMEOUT_SECONDS);
  evhttp_set_default_content_type(server->http, nullptr);
  evhttp_set_gencb(server->http, process_request, server);
}

static void init_stop_event(HTTPServer *server) {
  if (pipe(server->stop_pipe) != 0) {
    const int error_number = errno;
    pg_status_log_system_fatal(
      "http", error_number, "failed to create stop pipe"
    );
  }
  if (
    fcntl(server->stop_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
    fcntl(server->stop_pipe[1], F_SETFD, FD_CLOEXEC) != 0
  ) {
    const int error_number = errno;
    pg_status_log_system_fatal(
      "http", error_number, "failed to set FD_CLOEXEC on stop pipe"
    );
  }

  server->stop_event = event_new(
    server->event_base, server->stop_pipe[0], EV_READ, stop_event_callback,
    server
  );
  if (!server->stop_event) {
    pg_status_log_fatal("http", "failed to create stop event");
  }
  if (event_add(server->stop_event, nullptr) != 0) {
    pg_status_log_fatal("http", "failed to register stop event");
  }
}

static struct evconnlistener *create_listener(
  const HTTPServer *server, const struct sockaddr *address,
  const int address_length, const unsigned options, int *error_code
) {
  errno = 0;
  struct evconnlistener *listener = evconnlistener_new_bind(
    server->event_base, nullptr, nullptr, options, HTTP_LISTEN_BACKLOG, address,
    address_length
  );
  *error_code = listener ? 0 : errno;
  return listener;
}

static struct evconnlistener *create_ipv4_listener(
  const HTTPServer *server, const struct in_addr *listen_address,
  const uint16_t port, int *error_code
) {
  const struct sockaddr_in address = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr = *listen_address,
  };
  return create_listener(
    server, (const struct sockaddr *)&address, sizeof(address),
    HTTP_LISTENER_OPTIONS, error_code
  );
}

static struct evconnlistener *create_ipv6_listener(
  const HTTPServer *server, const struct in6_addr *listen_address,
  const uint16_t port, int *error_code
) {
  const struct sockaddr_in6 address = {
    .sin6_family = AF_INET6,
    .sin6_port = htons(port),
    .sin6_addr = *listen_address,
  };
  return create_listener(
    server, (const struct sockaddr *)&address, sizeof(address),
    HTTP_LISTENER_OPTIONS | LEV_OPT_BIND_IPV6ONLY, error_code
  );
}

static bool is_address_family_unavailable(const int error_code) {
  bool unavailable = error_code == EAFNOSUPPORT ||
                     error_code == EPROTONOSUPPORT ||
                     error_code == EADDRNOTAVAIL;
#ifdef EPFNOSUPPORT
  unavailable = unavailable || error_code == EPFNOSUPPORT;
#endif
  return unavailable;
}

static uint16_t get_listener_port(struct evconnlistener *listener) {
  struct sockaddr_storage address;
  socklen_t address_length = sizeof(address);
  if (
    getsockname(
      evconnlistener_get_fd(listener), (struct sockaddr *)&address,
      &address_length
    ) != 0
  ) {
    const int error_number = errno;
    pg_status_log_system_fatal(
      "http", error_number, "failed to determine bound port"
    );
  }

  if (address.ss_family == AF_INET) {
    const struct sockaddr_in *ipv4_address =
      (const struct sockaddr_in *)&address;
    return ntohs(ipv4_address->sin_port);
  }
  if (address.ss_family == AF_INET6) {
    const struct sockaddr_in6 *ipv6_address =
      (const struct sockaddr_in6 *)&address;
    return ntohs(ipv6_address->sin6_port);
  }

  pg_status_log_system_fatal(
    "http", EAFNOSUPPORT, "listener has an unsupported address family"
  );
}

static void raise_listener_error(
  const char *address_family, const uint16_t port, const int error_code,
  struct evconnlistener *other_listener
) {
  if (other_listener) {
    evconnlistener_free(other_listener);
  }
  const int actual_error = error_code != 0 ? error_code : EIO;
  pg_status_log_system_fatal(
    "http", actual_error, "failed to bind %s listener port=%u", address_family,
    (unsigned int)port
  );
}

static void attach_listener(
  const HTTPServer *server, struct evconnlistener *listener,
  const char *address_family
) {
  if (!listener) {
    return;
  }
  if (!evhttp_bind_listener(server->http, listener)) {
    const int error_code = errno;
    evconnlistener_free(listener);
    const int actual_error = error_code != 0 ? error_code : EIO;
    pg_status_log_system_fatal(
      "http", actual_error, "failed to attach %s listener", address_family
    );
  }
}

static void init_best_effort_http_listeners(
  HTTPServer *server, const uint16_t port
) {
  const struct in_addr ipv4_any = {.s_addr = htonl(INADDR_ANY)};
  int ipv4_error = 0;
  struct evconnlistener *ipv4_listener = create_ipv4_listener(
    server, &ipv4_any, port, &ipv4_error
  );

  const uint16_t shared_port = ipv4_listener ? get_listener_port(ipv4_listener)
                                             : port;
  const struct in6_addr ipv6_any = IN6ADDR_ANY_INIT;
  int ipv6_error = 0;
  struct evconnlistener *ipv6_listener = create_ipv6_listener(
    server, &ipv6_any, shared_port, &ipv6_error
  );

  if (!ipv4_listener && !is_address_family_unavailable(ipv4_error)) {
    raise_listener_error("IPv4", port, ipv4_error, ipv6_listener);
  }
  if (!ipv6_listener && !is_address_family_unavailable(ipv6_error)) {
    raise_listener_error("IPv6", shared_port, ipv6_error, ipv4_listener);
  }
  if (!ipv4_listener && !ipv6_listener) {
    const int error_number = ipv6_error != 0 ? ipv6_error : ipv4_error;
    pg_status_log_system_fatal(
      "http", error_number, "neither IPv4 nor IPv6 listener is available"
    );
  }

  if (!ipv4_listener) {
    pg_status_log_system_error(
      PG_STATUS_LOG_WARNING, "http", ipv4_error,
      "IPv4 listener is unavailable; continuing with IPv6"
    );
  }
  if (!ipv6_listener) {
    pg_status_log_system_error(
      PG_STATUS_LOG_WARNING, "http", ipv6_error,
      "IPv6 listener is unavailable; continuing with IPv4"
    );
  }

  server->port = ipv4_listener ? shared_port : get_listener_port(ipv6_listener);
  attach_listener(server, ipv4_listener, "IPv4");
  attach_listener(server, ipv6_listener, "IPv6");
}

static void init_configured_http_listener(
  HTTPServer *server, const char *listen_address, const uint16_t port
) {
  struct evconnlistener *listener = nullptr;
  const char *address_family = nullptr;
  int error_code = 0;

  struct in_addr ipv4_address;
  if (inet_pton(AF_INET, listen_address, &ipv4_address) == 1) {
    address_family = "IPv4";
    listener = create_ipv4_listener(server, &ipv4_address, port, &error_code);
  } else {
    struct in6_addr ipv6_address;
    if (inet_pton(AF_INET6, listen_address, &ipv6_address) == 1) {
      address_family = "IPv6";
      listener = create_ipv6_listener(server, &ipv6_address, port, &error_code);
    } else {
      pg_status_log_fatal(
        "config",
        "invalid HTTP listen address '%s'; expected an IPv4 address, an IPv6 "
        "address, or '*'",
        listen_address
      );
    }
  }

  if (!listener) {
    raise_listener_error(address_family, port, error_code, nullptr);
  }
  server->port = get_listener_port(listener);
  attach_listener(server, listener, address_family);
}

static void init_http_listeners(
  HTTPServer *server, const char *listen_address, const uint16_t port
) {
  if (!listen_address || *listen_address == '\0') {
    pg_status_log_fatal("config", "HTTP listen address must not be empty");
  }
  if (strcmp(listen_address, "*") == 0) {
    init_best_effort_http_listeners(server, port);
  } else {
    init_configured_http_listener(server, listen_address, port);
  }
}

static void start_event_loop_thread(HTTPServer *server) {
  pthread_mutex_lock(&server->start_mutex);
  const int started = pthread_create(
    &server->thread, nullptr, run_event_loop, server
  );

  if (started != 0) {
    pthread_mutex_unlock(&server->start_mutex);
    pg_status_log_system_fatal(
      "http", started, "failed to start event loop thread"
    );
  }

  while (!server->event_loop_started) {
    pthread_cond_wait(&server->start_cond, &server->start_mutex);
  }
  pthread_mutex_unlock(&server->start_mutex);
}

HTTPServer *start_http_server(
  const char *listen_address, const uint16_t port, const Route *routes,
  const size_t cnt_routes
) {
  HTTPServer *server = init_http_server(routes, cnt_routes);
  init_event_base(server);
  init_stop_event(server);
  init_evhttp(server);
  init_http_listeners(server, listen_address, port);

  start_event_loop_thread(server);

  pg_status_log(
    PG_STATUS_LOG_INFO, "http", "server started address=%s port=%u",
    listen_address, (unsigned int)server->port
  );
  return server;
}

uint16_t http_server_port(const HTTPServer *server) {
  return server->port;
}

void stop_http_server(HTTPServer *server) {
  if (!server) {
    return;
  }

  const char byte = 1;
  ssize_t write_result;
  do {
    write_result = write(server->stop_pipe[1], &byte, 1);
  } while (write_result < 0 && errno == EINTR);
  if (write_result != 1) {
    const int error_number = write_result < 0 ? errno : EIO;
    pg_status_log_system_fatal(
      "http", error_number, "failed to write stop pipe"
    );
  }
  const int joined = pthread_join(server->thread, nullptr);
  if (joined != 0) {
    pg_status_log_system_fatal(
      "http", joined, "failed to join event loop thread"
    );
  }

  event_free(server->stop_event);
  close(server->stop_pipe[0]);
  close(server->stop_pipe[1]);
  evhttp_free(server->http);
  event_base_free(server->event_base);
  pthread_cond_destroy(&server->start_cond);
  pthread_mutex_destroy(&server->start_mutex);
  free(server);
  pg_status_log(PG_STATUS_LOG_INFO, "http", "server stopped");
}

bool parse_get_param_uint(
  const HTTPRequest *request, const char *name, uint64_t *out
) {
  const char *raw = http_request_get_query_param(request, name);
  if (!raw) {
    return true;
  }
  uint64_t parsed;
  if (!try_str_to_ull(raw, &parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

bool parse_get_param_lsn(
  const HTTPRequest *request, const char *name, uint64_t *out
) {
  const char *raw = http_request_get_query_param(request, name);
  if (!raw) {
    return true;
  }
  uint64_t parsed;
  if (!try_parse_lsn(raw, &parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

void bad_request(HTTPResponse *response, const char *body) {
  http_response_set_status(response, HTTP_BADREQUEST);
  http_response_set_borrowed_body(response, body, "application/json");
}
