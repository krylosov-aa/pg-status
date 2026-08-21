/** Shared HTTP client and assertions for compiled tests. */

#include "http_test.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  RESPONSE_CAPACITY = 64 * 1024,
  REQUEST_CAPACITY = 2 * 1024,
};

[[noreturn]] void http_test_fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(EXIT_FAILURE);
}

void http_test_assert_true(const bool condition, const char *message) {
  if (!condition) {
    http_test_fail(message);
  }
}

static int connect_to_server(const uint16_t port, const bool use_ipv6) {
  const int family = use_ipv6 ? AF_INET6 : AF_INET;
  const int fd = socket(family, SOCK_STREAM, 0);
  if (fd < 0) {
    http_test_fail(use_ipv6 ? "IPv6 socket" : "socket");
  }

  int result;
  if (use_ipv6) {
    const struct sockaddr_in6 address = {
      .sin6_family = AF_INET6,
      .sin6_port = htons(port),
      .sin6_addr = IN6ADDR_LOOPBACK_INIT,
    };
    result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
  } else {
    const struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)},
    };
    result = connect(fd, (const struct sockaddr *)&address, sizeof(address));
  }

  if (result != 0) {
    close(fd);
    http_test_fail(use_ipv6 ? "IPv6 connect" : "connect");
  }
  return fd;
}

bool http_test_ipv6_loopback_is_available(void) {
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

static void write_all(const int fd, const char *data) {
  size_t remaining = strlen(data);
  while (remaining > 0) {
    const ssize_t written = write(fd, data, remaining);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      http_test_fail("write");
    }
    data += written;
    remaining -= (size_t)written;
  }
}

static unsigned int parse_status(const char *response) {
  const char *status_begin = strchr(response, ' ');
  if (!status_begin) {
    http_test_fail("HTTP response has no status");
  }
  status_begin++;

  errno = 0;
  char *status_end;
  const unsigned long status = strtoul(status_begin, &status_end, 10);
  if (
    errno != 0 || status_end == status_begin || *status_end != ' ' ||
    status > UINT_MAX
  ) {
    http_test_fail("invalid HTTP response status");
  }
  return (unsigned int)status;
}

static TestHTTPResponse read_response(const int fd) {
  char *raw = calloc(RESPONSE_CAPACITY, 1);
  if (!raw) {
    close(fd);
    http_test_fail("calloc");
  }

  size_t used = 0;
  while (used < RESPONSE_CAPACITY - 1) {
    const ssize_t received = read(fd, raw + used, RESPONSE_CAPACITY - used - 1);
    if (received == 0) {
      break;
    }
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(raw);
      close(fd);
      http_test_fail("read");
    }
    used += (size_t)received;
  }
  close(fd);

  const char *body = strstr(raw, "\r\n\r\n");
  if (!body) {
    free(raw);
    http_test_fail("HTTP response has no header terminator");
  }

  return (TestHTTPResponse){
    .raw = raw,
    .body = body + 4,
    .status = parse_status(raw),
  };
}

static TestHTTPResponse send_raw(
  const uint16_t port, const char *raw_request, const bool use_ipv6
) {
  const int fd = connect_to_server(port, use_ipv6);
  write_all(fd, raw_request);
  return read_response(fd);
}

TestHTTPResponse http_test_send_raw(
  const uint16_t port, const char *raw_request
) {
  return send_raw(port, raw_request, false);
}

TestHTTPResponse http_test_send_raw_ipv6(
  const uint16_t port, const char *raw_request
) {
  return send_raw(port, raw_request, true);
}

TestHTTPResponse http_test_get(
  const uint16_t port, const char *path, const char *headers
) {
  if (!headers) {
    headers = "";
  }

  char request[REQUEST_CAPACITY];
  const int request_length = snprintf(
    request, sizeof(request),
    "GET %s HTTP/1.1\r\nHost: localhost\r\n%sConnection: close\r\n\r\n", path,
    headers
  );
  if (request_length < 0 || (size_t)request_length >= sizeof(request)) {
    http_test_fail("request is too large");
  }
  return http_test_send_raw(port, request);
}

void http_test_response_free(TestHTTPResponse *response) {
  free(response->raw);
  *response = (TestHTTPResponse){0};
}

void http_test_assert_contains(
  const TestHTTPResponse *response, const char *expected
) {
  if (!strstr(response->raw, expected)) {
    fprintf(
      stderr, "Expected response to contain:\n%s\nActual response:\n%s\n",
      expected, response->raw
    );
    http_test_fail("unexpected HTTP response");
  }
}

void http_test_assert_status(
  const TestHTTPResponse *response, const unsigned int expected
) {
  if (response->status != expected) {
    fprintf(
      stderr, "Expected HTTP status: %u\nActual HTTP status: %u\n%s\n",
      expected, response->status, response->raw
    );
    http_test_fail("unexpected HTTP status");
  }
}

void http_test_assert_body(
  const TestHTTPResponse *response, const char *expected
) {
  if (strcmp(response->body, expected) != 0) {
    fprintf(
      stderr, "Expected body: %s\nActual body: %s\n", expected, response->body
    );
    http_test_fail("unexpected HTTP body");
  }
}
