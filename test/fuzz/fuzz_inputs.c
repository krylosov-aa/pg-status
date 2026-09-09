/** Bounded fuzzing of numeric/LSN parsing and JSON log escaping. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "log_formatter.h"
#include "utils.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, const size_t size) {
  if (size > 4096) {
    return 0;
  }
  char input[4097];
  memcpy(input, data, size);
  input[size] = '\0';
  uint64_t value;
  (void)try_str_to_ull(input, &value);
  if (try_parse_lsn(input, &value)) {
    char *canonical = format_lsn(value);
    uint64_t roundtrip;
    if (!try_parse_lsn(canonical, &roundtrip) || roundtrip != value) {
      abort();
    }
    free(canonical);
  }
  char output[8192];
  const PGStatusLogEntry entry = {
    .timestamp = "2026-01-01T00:00:00.000Z",
    .level = PG_STATUS_LOG_INFO,
    .level_name = "INFO",
    .component = input,
    .message = input,
  };
  const size_t length = pg_status_format_json(output, sizeof(output), &entry);
  if (length) {
    cJSON *parsed = cJSON_Parse(output);
    if (!parsed || strlen(output) != length) {
      abort();
    }
    cJSON_Delete(parsed);
  }
  return 0;
}
