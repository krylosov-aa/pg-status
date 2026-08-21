/**
 * General purpose utilities
 */

#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H

#include <cjson/cJSON.h>
#include <stdint.h>

// ------------------------ Strings ------------------------

/**
 * Copies a string. The result must be freed by the caller.
 */
char *copy_string(const char *str);

/**
 * Checks if strings are the same
 */
bool is_equal_strings(const char *first, const char *second);

/**
 * Forms a new string and substitutes arguments in printf style.
 * The result must be freed by the caller.
 */
[[gnu::format(printf, 1, 2)]]
char *format_string(const char *format, ...);

/**
 * Converts string to long
 */
long str_to_long(const char *value);

/**
 * Converts string to unsigned long
 */
unsigned long str_to_ulong(const char *value);

/**
 * Converts string to uint64_t
 */
uint64_t str_to_ull(const char *value);

/**
 * Converts string to uint64_t without aborting on failure.
 * Returns true on success and writes the parsed value to *out.
 * Returns false on any malformed input: NULL/empty, leading '-',
 * non-numeric content, or overflow.
 */
bool try_str_to_ull(const char *value, uint64_t *out);

/**
 * Parses a PostgreSQL LSN in canonical "HEX/HEX" form into a 64-bit
 * integer (high 32 bits | low 32 bits). Returns true on success and
 * writes the result to *out. Returns false on any malformed input:
 * NULL/empty, missing or leading '/', non-hex content, overflow of
 * either half above 0xFFFFFFFF, or trailing garbage.
 */
bool try_parse_lsn(const char *value, uint64_t *out);

/**
 * Formats a 64-bit LSN value back to canonical "HEX/HEX" form
 * (uppercase, no leading zeros). The result must be freed by the caller.
 */
char *format_lsn(uint64_t lsn);

/**
 * Converts string to int
 */
int str_to_int(const char *value);

/**
 * Converts string to int greater than or equal to zero
 */
int str_to_int_greater_or_equal_zero(const char *value);

/**
 * Converts string to unsigned int
 */
unsigned int str_to_uint(const char *value);

/**
 * Converts string to unsigned int 16
 */
uint16_t str_to_uint16(const char *value);

// ------------------------ Environment variables ------------------------

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env(const char *env_name, const char **result);

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result);

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_ull(const char *env_name, uint64_t *result);

// ------------------------ JSON ------------------------

/**
 * Creates a new json array object
 */
cJSON *json_array(void);

/**
 * Creates a new json object
 */
cJSON *json_object(void);

/**
 * Adds a new key and string value to json
 */
void add_str_to_json_object(cJSON *obj, const char *key, const char *val);

/**
 * Adds a new key with value null to json
 */
void add_null_to_json_object(cJSON *obj, const char *key);

/**
 * Adds a new key and bool value to json
 */
void add_bool_to_json_object(cJSON *obj, const char *key, bool val);

/**
 * Adds a new key and uint64 value to json as a JSON number.
 */
void add_uint64_to_json_object(cJSON *obj, const char *key, uint64_t val);

/**
 * Converts json to string.
 * The string must be freed by the caller.
 */
char *json_to_str(cJSON *json);

/**
 * Monotonic time in milliseconds.
 */
uint64_t monotonic_ms(void);

#endif  // PG_STATUS_UTILS_H
