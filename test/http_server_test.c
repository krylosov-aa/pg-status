#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  RESPONSE_CAPACITY = 32 * 1024,
};

static void echo_handler(const HTTPRequest *request, HTTPResponse *response) {
  const char *value = http_request_get_query_param(request, "value");
  if (!value) {
    bad_request(response, "{\"error_text\":\"missing value\"}");
    return;
  }
  http_response_set_body(response, value, "text/plain; charset=utf-8", nullptr);
}

static void format_handler(const HTTPRequest *request, HTTPResponse *response) {
  if (http_request_accepts_json(request)) {
    http_response_set_body(
      response, "{\"format\":\"json\"}", "application/json", nullptr
    );
  } else {
    http_response_set_body(
      response, "text", "text/plain; charset=utf-8", nullptr
    );
  }
}

static char custom_body[] = "custom";
static atomic_bool custom_body_cleaned = false;

static void custom_body_cleanup(void *body) {
  atomic_store(&custom_body_cleaned, body == custom_body);
}

static void cleanup_handler(
  const HTTPRequest *request, HTTPResponse *response
) {
  (void)request;
  http_response_set_body(
    response, custom_body, "text/plain; charset=utf-8", custom_body_cleanup
  );
}

static const Route routes[] = {
  {"/echo", echo_handler},
  {"/format", format_handler},
  {"/cleanup", cleanup_handler},
};

static void fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(EXIT_FAILURE);
}

static int connect_to_server(const uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fail("socket");
  }
  const struct sockaddr_in address = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
  };
  if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    fail("connect");
  }
  return fd;
}

static int connect_to_server_ipv6(const uint16_t port) {
  const int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    fail("IPv6 socket");
  }
  const struct sockaddr_in6 address = {
    .sin6_family = AF_INET6,
    .sin6_port = htons(port),
    .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    fail("IPv6 connect");
  }
  return fd;
}

static bool ipv6_loopback_is_available(void) {
  const int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  const struct sockaddr_in6 address = {
    .sin6_family = AF_INET6,
    .sin6_port = 0,
    .sin6_addr = IN6ADDR_LOOPBACK_INIT,
  };
  const bool available =
    bind(fd, (const struct sockaddr *)&address, sizeof(address)) == 0;
  close(fd);
  return available;
}

static void write_all(const int fd, const char *request) {
  size_t remaining = strlen(request);
  const char *pos = request;
  while (remaining > 0) {
    const ssize_t written = write(fd, pos, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      fail("write");
    }
    pos += written;
    remaining -= (size_t)written;
  }
}

static char *send_request(const int fd, const char *raw_request) {
  write_all(fd, raw_request);

  char *response = calloc(RESPONSE_CAPACITY, 1);
  if (!response) {
    close(fd);
    fail("calloc");
  }
  size_t used = 0;
  while (used < RESPONSE_CAPACITY - 1) {
    const ssize_t received = read(
      fd, response + used, RESPONSE_CAPACITY - used - 1
    );
    if (received == 0) {
      break;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(response);
      close(fd);
      fail("read");
    }
    used += (size_t)received;
  }
  close(fd);
  return response;
}

static char *request(const uint16_t port, const char *raw_request) {
  return send_request(connect_to_server(port), raw_request);
}

static char *request_ipv6(const uint16_t port, const char *raw_request) {
  return send_request(connect_to_server_ipv6(port), raw_request);
}

static void assert_contains(const char *response, const char *expected) {
  if (!strstr(response, expected)) {
    fprintf(
      stderr, "Expected response to contain: %s\n%s\n", expected, response
    );
    fail("unexpected HTTP response");
  }
}

static size_t count_occurrences(const char *text, const char *needle) {
  size_t count = 0;
  const size_t needle_len = strlen(needle);
  while ((text = strstr(text, needle))) {
    count++;
    text += needle_len;
  }
  return count;
}

static void test_echo(const uint16_t port) {
  char *response = request(
    port,
    "GET /echo?value=hello%20world HTTP/1.1\r\n"
    "Host: localhost\r\nConnection: close\r\n\r\n"
  );
  assert_contains(response, "HTTP/1.1 200 OK");
  assert_contains(response, "Content-Type: text/plain; charset=utf-8");
  assert_contains(response, "\r\n\r\nhello world");
  free(response);
}

static void test_custom_body_cleanup(const uint16_t port) {
  atomic_store(&custom_body_cleaned, false);
  char *response = request(
    port,
    "GET /cleanup HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
  );
  assert_contains(response, "\r\n\r\ncustom");
  if (!atomic_load(&custom_body_cleaned)) {
    fail("custom response body cleanup was not called");
  }
  free(response);
}

static void test_ipv6(const uint16_t port) {
  char *response = request_ipv6(
    port,
    "GET /echo?value=ipv6 HTTP/1.1\r\n"
    "Host: [::1]\r\nConnection: close\r\n\r\n"
  );
  assert_contains(response, "HTTP/1.1 200 OK");
  assert_contains(response, "\r\n\r\nipv6");
  free(response);
}

static void test_accept_negotiation(const uint16_t port) {
  char *response = request(
    port,
    "GET /format HTTP/1.1\r\nHost: localhost\r\n"
    "Accept: text/plain, application/json; q=0.5\r\n"
    "Connection: close\r\n\r\n"
  );
  assert_contains(response, "Content-Type: application/json");
  assert_contains(response, "{\"format\":\"json\"}");
  free(response);

  response = request(
    port,
    "GET /format HTTP/1.1\r\nHost: localhost\r\n"
    "Accept: application/json; q=0, */*\r\n"
    "Connection: close\r\n\r\n"
  );
  assert_contains(response, "Content-Type: text/plain; charset=utf-8");
  assert_contains(response, "\r\n\r\ntext");
  free(response);
}

static void test_errors(const uint16_t port) {
  char *response = request(
    port,
    "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n"
  );
  assert_contains(response, "HTTP/1.1 405 Method Not Allowed");
  assert_contains(response, "Allow: GET");
  free(response);

  response = request(
    port,
    "GET /missing HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: close\r\n\r\n"
  );
  assert_contains(response, "HTTP/1.1 404 Not Found");
  free(response);

  response = request(
    port,
    "GET /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n"
    "Connection: close\r\n\r\nx"
  );
  assert_contains(response, "HTTP/1.1 413 Request Entity Too Large");
  free(response);
}

static void test_keep_alive_pipeline(const uint16_t port) {
  char *response = request(
    port,
    "GET /format HTTP/1.1\r\nHost: localhost\r\n\r\n"
    "GET /echo?value=second HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: close\r\n\r\n"
  );
  if (count_occurrences(response, "HTTP/1.1 200 OK") != 2) {
    fail("HTTP keep-alive pipeline did not return two responses");
  }
  assert_contains(response, "\r\n\r\ntext");
  assert_contains(response, "\r\n\r\nsecond");
  free(response);
}

static void test_configured_listen_address(
  const char *listen_address, const bool use_ipv6
) {
  HTTPServer *server = start_http_server(
    listen_address, 0, routes, sizeof(routes) / sizeof(routes[0])
  );
  const uint16_t port = http_server_port(server);
  if (use_ipv6) {
    test_ipv6(port);
  } else {
    test_echo(port);
  }
  stop_http_server(server);
}

int main(void) {
  const bool ipv6_available = ipv6_loopback_is_available();
  HTTPServer *server = start_http_server(
    "*", 0, routes, sizeof(routes) / sizeof(routes[0])
  );
  const uint16_t port = http_server_port(server);

  test_echo(port);
  test_custom_body_cleanup(port);
  if (ipv6_available) {
    test_ipv6(port);
  } else {
    puts("IPv6 loopback is unavailable; skipping IPv6 HTTP check");
  }
  test_accept_negotiation(port);
  test_errors(port);
  test_keep_alive_pipeline(port);

  stop_http_server(server);

  test_configured_listen_address("0.0.0.0", false);
  test_configured_listen_address("127.0.0.1", false);
  if (ipv6_available) {
    test_configured_listen_address("::1", true);
    test_configured_listen_address("::", true);
  }

  puts("http_server_test passed");
  return EXIT_SUCCESS;
}
