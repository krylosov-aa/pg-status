/** Test scenarios for the asynchronous HTTP server abstraction. */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_server.h"
#include "http_test.h"

static void echo_handler(const HTTPRequest *request, HTTPResponse *response) {
  const char *value = http_request_get_query_param(request, "value");
  if (!value) {
    bad_request(response, "{\"error_text\":\"missing value\"}");
    return;
  }
  http_response_set_borrowed_body(response, value, "text/plain; charset=utf-8");
}

static void format_handler(const HTTPRequest *request, HTTPResponse *response) {
  if (http_request_accepts_json(request)) {
    http_response_set_borrowed_body(
      response, "{\"format\":\"json\"}", "application/json"
    );
  } else {
    http_response_set_borrowed_body(
      response, "text", "text/plain; charset=utf-8"
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
  http_response_set_owned_body(
    response, custom_body, "text/plain; charset=utf-8", custom_body_cleanup
  );
}

static const Route routes[] = {
  {"/echo", echo_handler},
  {"/format", format_handler},
  {"/cleanup", cleanup_handler},
};

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
  // Arrange
  const char *path = "/echo?value=hello%20world";

  // Act
  TestHTTPResponse response = http_test_get(port, path, nullptr);

  // Assert
  http_test_assert_status(&response, 200);
  http_test_assert_contains(
    &response, "Content-Type: text/plain; charset=utf-8"
  );
  http_test_assert_body(&response, "hello world");

  // Cleanup
  http_test_response_free(&response);
}

static void test_custom_body_cleanup(const uint16_t port) {
  // Arrange
  atomic_store(&custom_body_cleaned, false);

  // Act
  TestHTTPResponse response = http_test_get(port, "/cleanup", nullptr);

  // Assert
  http_test_assert_body(&response, "custom");
  http_test_assert_true(
    atomic_load(&custom_body_cleaned),
    "custom response body cleanup was not called"
  );

  // Cleanup
  http_test_response_free(&response);
}

static void test_ipv6(const uint16_t port) {
  // Arrange
  const char *request =
    "GET /echo?value=ipv6 HTTP/1.1\r\n"
    "Host: [::1]\r\nConnection: close\r\n\r\n";

  // Act
  TestHTTPResponse response = http_test_send_raw_ipv6(port, request);

  // Assert
  http_test_assert_status(&response, 200);
  http_test_assert_body(&response, "ipv6");

  // Cleanup
  http_test_response_free(&response);
}

static void test_accept_negotiation(const uint16_t port) {
  // Arrange
  const char *json_accepted = "Accept: text/plain, application/json; q=0.5\r\n";
  const char *json_rejected = "Accept: application/json; q=0, */*\r\n";

  // Act
  TestHTTPResponse response = http_test_get(port, "/format", json_accepted);

  // Assert
  http_test_assert_contains(&response, "Content-Type: application/json");
  http_test_assert_body(&response, "{\"format\":\"json\"}");
  http_test_response_free(&response);

  // Act
  response = http_test_get(port, "/format", json_rejected);

  // Assert
  http_test_assert_contains(
    &response, "Content-Type: text/plain; charset=utf-8"
  );
  http_test_assert_body(&response, "text");

  // Cleanup
  http_test_response_free(&response);
}

static void test_errors(const uint16_t port) {
  // Arrange
  const char *post_request =
    "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
    "Content-Length: 0\r\nConnection: close\r\n\r\n";
  const char *body_request =
    "GET /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n"
    "Connection: close\r\n\r\nx";

  // Act
  TestHTTPResponse response = http_test_send_raw(port, post_request);

  // Assert
  http_test_assert_status(&response, 405);
  http_test_assert_contains(&response, "Allow: GET");
  http_test_response_free(&response);

  // Act
  response = http_test_get(port, "/missing", nullptr);

  // Assert
  http_test_assert_status(&response, 404);
  http_test_response_free(&response);

  // Act
  response = http_test_send_raw(port, body_request);

  // Assert
  http_test_assert_status(&response, 413);

  // Cleanup
  http_test_response_free(&response);
}

static void test_keep_alive_pipeline(const uint16_t port) {
  // Arrange
  const char *request =
    "GET /format HTTP/1.1\r\nHost: localhost\r\n\r\n"
    "GET /echo?value=second HTTP/1.1\r\nHost: localhost\r\n"
    "Connection: close\r\n\r\n";

  // Act
  TestHTTPResponse response = http_test_send_raw(port, request);

  // Assert
  http_test_assert_true(
    count_occurrences(response.raw, "HTTP/1.1 200 OK") == 2,
    "HTTP keep-alive pipeline did not return two responses"
  );
  http_test_assert_contains(&response, "\r\n\r\ntext");
  http_test_assert_contains(&response, "\r\n\r\nsecond");

  // Cleanup
  http_test_response_free(&response);
}

static void test_configured_listen_address(
  const char *listen_address, const bool use_ipv6
) {
  // Arrange
  HTTPServer *server = start_http_server(
    listen_address, 0, routes, sizeof(routes) / sizeof(routes[0])
  );
  const uint16_t port = http_server_port(server);

  // Act & Assert
  if (use_ipv6) {
    test_ipv6(port);
  } else {
    test_echo(port);
  }

  // Cleanup
  stop_http_server(server);
}

int main(void) {
  // Arrange
  const bool ipv6_available = http_test_ipv6_loopback_is_available();
  HTTPServer *server = start_http_server(
    "*", 0, routes, sizeof(routes) / sizeof(routes[0])
  );
  const uint16_t port = http_server_port(server);

  // Act & Assert
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

  // Cleanup
  stop_http_server(server);

  // Arrange, Act & Assert
  test_configured_listen_address("0.0.0.0", false);
  test_configured_listen_address("127.0.0.1", false);
  if (ipv6_available) {
    test_configured_listen_address("::1", true);
    test_configured_listen_address("::", true);
  }

  puts("http_server_test passed");
  return EXIT_SUCCESS;
}
