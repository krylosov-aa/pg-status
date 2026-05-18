/**
 * HTTP server
 */

#include "http_server.h"

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils.h"

/**
 * list of routes
 */
typedef struct {
  Route *routes;
  unsigned int cnt;
} Routes;

/**
 * list of routes registered for processing
 */
static Routes routes_list = {
  .routes = nullptr,
  .cnt = 0,
};

/**
 * The default handler if no matching route is found is to return a 404.
 */
static void not_found(MHD_Connection *connection, HTTPResponse *response) {
  MHD_Response *mhd_response = MHD_create_response_from_buffer(
    0, NULL, MHD_RESPMEM_PERSISTENT
  );
  response->mhd_response = mhd_response;
  response->status_code = MHD_HTTP_NOT_FOUND;
}

/**
 * Generates an MHD_Response from string
 */
static MHD_Response *create_response_from_string(
  char *string, const MHD_ResponseMemoryMode memory_mode
) {
  return MHD_create_response_from_buffer(
    strlen(string), (void *)string, memory_mode
  );
}

/**
 * Generates an MHD_Response from static string
 */
static MHD_Response *create_response_from_const_string(const char *string) {
  return MHD_create_response_from_buffer(
    strlen(string), (void *)string, MHD_RESPMEM_PERSISTENT
  );
}

/**
 * Sends a response to the mhd queue for sending the response
 */
static MHD_Result queue_response(
  MHD_Connection *connection, HTTPResponse *response, const char *path,
  const char *method
) {
  MHD_Result ret = MHD_NO;
  MHD_Response *mhd_response = nullptr;

  if (response->mhd_response) {
    mhd_response = response->mhd_response;
    response->mhd_response = nullptr;
  }

  if (!mhd_response) {
    if (response->const_response) {
      mhd_response = create_response_from_const_string(
        response->const_response
      );
    } else if (response->response) {
      mhd_response = create_response_from_string(
        response->response, response->memory_mode
      );
    } else {
      mhd_response = MHD_create_response_from_buffer(
        0, NULL, MHD_RESPMEM_PERSISTENT
      );
    }
  }

  if (mhd_response) {
    if (response->content_type != nullptr) {
      MHD_add_response_header(
        mhd_response, MHD_HTTP_HEADER_CONTENT_TYPE, response->content_type
      );
    }

    ret = MHD_queue_response(connection, response->status_code, mhd_response);

    if (ret != MHD_YES) {
      printf_error("Failed to MHD_queue_response %s %s", method, path);
    }

    MHD_destroy_response(mhd_response);

  } else {
    printf_error(
      "mhd_response or response wasn't provided for queue_response %s %s",
      method, path
    );
  }
  return ret;
}

/**
 * Handler at the end of request processing
 */
static void request_completed(
  void *cls, MHD_Connection *connection, void **req_cls,
  const MHD_RequestTerminationCode toe
) {
  switch (toe) {
    case MHD_REQUEST_TERMINATED_COMPLETED_OK:
      // printf("request completed\n");
      break;
    case MHD_REQUEST_TERMINATED_WITH_ERROR:
      printf_error("request completed with error\n");
      break;
    case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED:
      printf_error("request completed with timeout\n");
      break;
    case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN:
      printf_error("request completed with MHD shutdown\n");
      break;
    case MHD_REQUEST_TERMINATED_READ_ERROR:
      printf_error("request completed with terminated read error\n");
      break;
    case MHD_REQUEST_TERMINATED_CLIENT_ABORT:
      printf_error("request completed with client abort\n");
      break;
  }
}

/**
 * Searches for a suitable route among registered routes.
 * Paths are unique within a method, so the path check happens first
 * and the method check runs only on the matching entry.
 */
static request_handler_t find_handler(const char *method, const char *path) {
  const Route *routes = routes_list.routes;
  for (unsigned int i = 0; i < routes_list.cnt; i++) {
    if (strcmp(routes[i].path, path) != 0) {
      continue;
    }
    if (strcmp(routes[i].method, method) == 0) {
      return routes[i].handler;
    }
  }
  return not_found;
}

/**
 * Starts execution of the handler registered in the route.
 */
static MHD_Result process_handler(
  const char *path, const char *method, HTTPResponse *response,
  MHD_Connection *connection
) {
  MHD_Result result = MHD_NO;
  const request_handler_t handler = find_handler(method, path);

  const char *content_type = MHD_lookup_connection_value(
    connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_ACCEPT
  );
  if (content_type != nullptr) {
    response->content_type = content_type;
  }

  handler(connection, response);

  result = queue_response(connection, response, path, method);
  return result;
}

/**
 * Processing a get request
 */
static MHD_Result process_get(
  void *cls, MHD_Connection *connection, const char *path, const char *method,
  const char *version, const char *upload_data, void **req_cls
) {
  HTTPResponse response = {
    .response = nullptr,
    .const_response = nullptr,
    .mhd_response = nullptr,
    .content_type = nullptr,
    .memory_mode = MHD_RESPMEM_MUST_COPY,
    .status_code = MHD_HTTP_OK,
  };

  return process_handler(path, method, &response, connection);
}

/**
 * Processing a request
 */
static MHD_Result answer_to_connection(
  void *cls, MHD_Connection *connection, const char *path, const char *method,
  const char *version, const char *upload_data, size_t *upload_data_size,
  void **req_cls
) {
  // pg-status only serves GET requests, so we don't need the two-step
  // upload-data processing MHD requires for POST.
  return process_get(
    cls, connection, path, method, version, upload_data, req_cls
  );
}

/**
 * Starts http server daemon
 *
 * Although all the code is thread-safe, the HTTP server runs in
 * single-threaded mode simply because the GET endpoints are so
 * fast that using multiple threads only makes things slower.
 */
MHD_Daemon *start_http_server(
  const uint16_t port, Route *routes, const unsigned int cnt_routes
) {
  routes_list.routes = routes;
  routes_list.cnt = cnt_routes;

  MHD_Daemon *daemon = MHD_start_daemon(
    MHD_USE_AUTO_INTERNAL_THREAD | MHD_USE_ERROR_LOG, port, nullptr, nullptr,
    answer_to_connection, nullptr, MHD_OPTION_NOTIFY_COMPLETED,
    request_completed, nullptr,
    // MHD_OPTION_NOTIFY_CONNECTION, notify_connection_callback, nullptr,
    MHD_OPTION_LISTEN_BACKLOG_SIZE, 512, MHD_OPTION_CONNECTION_LIMIT, 1000,
    MHD_OPTION_CONNECTION_MEMORY_LIMIT, 8 * 1024, MHD_OPTION_END
  );
  if (!daemon) {
    raise_error("Failed to start mhd daemon");
  }
  printf("http server started on the %d port\n", port);
  return daemon;
}

/**
 * Stops http server daemon
 */
void stop_http_server(MHD_Daemon *daemon) {
  MHD_stop_daemon(daemon);
  printf("http server stopped\n");
}

bool need_json_response(const HTTPResponse *response) {
  return response->content_type &&
         is_equal_strings(response->content_type, "application/json");
}

bool parse_get_param_uint(
  MHD_Connection *connection, const char *name, uint64_t *out
) {
  const char *raw = MHD_lookup_connection_value(
    connection, MHD_GET_ARGUMENT_KIND, name
  );
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
  MHD_Connection *connection, const char *name, uint64_t *out
) {
  const char *raw = MHD_lookup_connection_value(
    connection, MHD_GET_ARGUMENT_KIND, name
  );
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

void bad_request(HTTPResponse *response, const char *const_response) {
  response->status_code = 400;
  response->const_response = const_response;
}
