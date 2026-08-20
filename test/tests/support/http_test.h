#ifndef HTTP_TEST_SUPPORT_H
#define HTTP_TEST_SUPPORT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char *raw;
  const char *body;
  unsigned int status;
} TestHTTPResponse;

void http_test_fail(const char *message);
void http_test_assert_true(bool condition, const char *message);

bool http_test_ipv6_loopback_is_available(void);

TestHTTPResponse http_test_send_raw(uint16_t port, const char *raw_request);
TestHTTPResponse http_test_send_raw_ipv6(
  uint16_t port, const char *raw_request
);
TestHTTPResponse http_test_get(
  uint16_t port, const char *path, const char *headers
);

void http_test_response_free(TestHTTPResponse *response);
void http_test_assert_contains(
  const TestHTTPResponse *response, const char *expected
);
void http_test_assert_status(
  const TestHTTPResponse *response, unsigned int expected
);
void http_test_assert_body(
  const TestHTTPResponse *response, const char *expected
);

#endif  // HTTP_TEST_SUPPORT_H
