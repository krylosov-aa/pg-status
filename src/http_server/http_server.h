/**
 * Asynchronous HTTP server
 */

#ifndef PG_STATUS_HTTP_SERVER_H
#define PG_STATUS_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct HTTPRequest HTTPRequest;
typedef struct HTTPResponse HTTPResponse;
typedef struct HTTPServer HTTPServer;

/**
 * Interface for a registered route handler.
 */
typedef void (*request_handler_t)(
  const HTTPRequest *request, HTTPResponse *response
);

/**
 * Releases a response body after it has been copied to the output buffer.
 */
typedef void (*response_body_cleanup_t)(void *body);

/**
 * An exact GET route match.
 */
typedef struct Route {
  const char *path;
  request_handler_t handler;
} Route;

/**
 * Starts the HTTP server and its event-loop thread.
 * listen_address accepts an IPv4 address, an IPv6 address, or "*" for
 * best-effort dual-stack wildcard listeners.
 */
HTTPServer *start_http_server(
  const char *listen_address, uint16_t port, const Route *routes,
  size_t cnt_routes
);

/**
 * Returns the bound TCP port. This is useful when port 0 selected an
 * ephemeral port for a test server.
 */
uint16_t http_server_port(const HTTPServer *server);

/**
 * Stops the event loop, joins its thread, and releases the server.
 */
void stop_http_server(HTTPServer *server);

/**
 * Returns a decoded query parameter, or nullptr when it was not provided.
 * The returned pointer is valid only for the duration of the handler call.
 */
const char *http_request_get_query_param(
  const HTTPRequest *request, const char *name
);

/**
 * Whether application/json is explicitly accepted with a non-zero quality.
 */
bool http_request_accepts_json(const HTTPRequest *request);

/**
 * Sets a response body. If cleanup is non-null, it is called exactly once with
 * body after the body is no longer needed. A null cleanup means that the body
 * is borrowed and remains owned by the caller.
 */
void http_response_set_body(
  HTTPResponse *response, const char *body, const char *content_type,
  response_body_cleanup_t cleanup
);

void http_response_set_status(HTTPResponse *response, unsigned int status_code);

bool parse_get_param_uint(
  const HTTPRequest *request, const char *name, uint64_t *out
);

bool parse_get_param_lsn(
  const HTTPRequest *request, const char *name, uint64_t *out
);

void bad_request(HTTPResponse *response, const char *body);

#endif  // PG_STATUS_HTTP_SERVER_H
