#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H

#include <cjson/cJSON.h>
#include <stdint.h>

// Strings

/**
 * Copies a string. The result must be freed by the caller.
 */
char *copy_string(const char *str);

/**
 * Returns false if either pointer is null, including when both are null.
 */
bool is_equal_strings(const char *first, const char *second);

/**
 * Allocates a printf-formatted string. The caller must free the result.
 */
[[gnu::format(printf, 1, 2)]]
char *format_string(const char *format, ...);

/**
 * Parses a decimal integer; invalid input or overflow terminates the process.
 */
long str_to_long(const char *value);

/**
 * Parses an unsigned decimal integer; invalid input or overflow is fatal.
 */
unsigned long str_to_ulong(const char *value);

/**
 * Parses a decimal uint64_t; invalid input or overflow is fatal.
 */
uint64_t str_to_ull(const char *value);

/**
 * Parses a decimal uint64_t into *out. Returns false on invalid input or
 * overflow and leaves *out unchanged.
 */
bool try_str_to_ull(const char *value, uint64_t *out);

/**
 * Parses an LSN as two hexadecimal 32-bit halves separated by '/'.
 * Returns false on invalid input or overflow and leaves *out unchanged.
 */
bool try_parse_lsn(const char *value, uint64_t *out);

/**
 * Formats a 64-bit LSN value back to canonical "HEX/HEX" form
 * (uppercase, no leading zeros). The result must be freed by the caller.
 */
char *format_lsn(uint64_t lsn);

/**
 * Parses a decimal int; invalid input or overflow is fatal.
 */
int str_to_int(const char *value);

/**
 * Parses a non-negative decimal int; invalid input or overflow is fatal.
 */
int str_to_int_greater_or_equal_zero(const char *value);

/**
 * Parses a decimal unsigned int; invalid input or overflow is fatal.
 */
unsigned int str_to_uint(const char *value);

/**
 * Parses a decimal uint16_t; invalid input or overflow is fatal.
 */
uint16_t str_to_uint16(const char *value);

// Environment variables

/**
 * Replaces *result with a borrowed environment value when it is non-empty.
 */
void replace_from_env(const char *env_name, const char **result);

/**
 * Replaces *result from a non-empty environment value; invalid input is fatal.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result);

/**
 * Replaces *result from a non-empty environment value; invalid input is fatal.
 */
void replace_from_env_ull(const char *env_name, uint64_t *result);

// JSON

/**
 * Allocates a JSON array; allocation failure is fatal.
 */
cJSON *json_array(void);

/**
 * Allocates a JSON object; allocation failure is fatal.
 */
cJSON *json_object(void);

/**
 * Copies the string into the object; allocation failure is fatal.
 */
void add_str_to_json_object(cJSON *obj, const char *key, const char *val);

/**
 * Copies the string into the object, or adds JSON null for a null pointer.
 */
void add_nullable_str_to_json_object(
  cJSON *json_obj, const char *name, const char *value
);

/**
 * Adds JSON null; allocation failure is fatal.
 */
void add_null_to_json_object(cJSON *obj, const char *key);

/**
 * Adds a JSON boolean; allocation failure is fatal.
 */
void add_bool_to_json_object(cJSON *obj, const char *key, bool val);

/**
 * Adds a JSON number via double conversion, which can lose integer precision.
 */
void add_uint64_to_json_object(cJSON *obj, const char *key, uint64_t val);

/**
 * Serializes and deletes the JSON tree. The caller must free the string.
 */
char *json_to_str(cJSON *json);

/**
 * Monotonic time in milliseconds.
 */
uint64_t monotonic_ms(void);

#endif  // PG_STATUS_UTILS_H
