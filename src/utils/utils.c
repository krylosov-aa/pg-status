/**
 * General purpose utilities
 */

#include "utils.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>


static int fputs_error(void) {
    char buf[256];
    if (strerror_r(errno, buf, sizeof(buf)) == 0) {
        return fputs(buf, stderr);
    }
    return fputs("Unknown error", stderr);
}

/**
 * Copies a string. The result must be freed by the caller.
 */
char *copy_string(const char *str) {
    char *result = strdup(str);
    if (!result) {
        raise_error("Error when trying to copy a string");
    }
    return result;
}

/**
 * Prints the message with \n and also adds the error text from errno
 */
void printf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vfprintf(stderr, format, args);
    va_end(args);
    (void)fputs(". strerror: ", stderr);
    (void)fputs_error();
    (void)fputc('\n', stderr);
}

/**
 * Prints the message with \n and also adds the
 * error text from errno and abort
 */
noreturn void raise_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    (void)vfprintf(stderr, format, args);
    va_end(args);
    (void)fputs(". strerror: ", stderr);
    (void)fputs_error();
    (void)fputc('\n', stderr);
    abort();
}

/**
 * Concatenates strings and returns the new string.
 * The result must be freed by the caller.
 */
char *concatenate_strings(const char *first, const char *second) {
    const size_t len1 = strlen(first);
    const size_t len2 = strlen(second);
    char *new = malloc(len1 + len2 + 1);
    if (!new) {
        raise_error(
            "Can't allocate memory for concatenate strings %s and %s",
            first,
            second
        );
    }
    strlcpy(new, first, len1 + len2 + 1);
    strlcat(new, second, len1 + len2 + 1);
    return new;
}

/**
 * Checks if strings are the same
 */
bool is_equal_strings(const char *first, const char *second) {
    if (!first || !second) {
        return false;
    }
    return strcmp(first, second) == 0;
}


/**
 * Forms a new string and substitutes arguments in printf style.
 * The result must be freed by the caller.
 */
char *format_string(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *string = nullptr;
    const int len = vasprintf(&string, format, args);
    va_end(args);

    if (len < 0) {
        raise_error("Unable to format_string %s", format);
    }
    return string;
}

/**
 * Converts unsigned long to string. The result must be freed by the caller.
 */
char *ulong_to_str(const unsigned long value) {
    const int len = snprintf(nullptr, 0, "%lu", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    if (!str) {
        raise_error(
            "Can't allocate memory for ulong_to_str %lu",
            value
        );
    }
    (void)snprintf(str, size, "%lu", value);
    return str;
}

/**
 * Converts long to string. The result must be freed by the caller.
 */
char *long_to_str(const long value) {
    const int len = snprintf(nullptr, 0, "%ld", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    if (!str) {
        raise_error(
            "Can't allocate memory for long_to_str %ld",
            value
        );
    }
    (void)snprintf(str, size, "%ld", value);
    return str;
}

/**
 * Converts int to string. The result must be freed by the caller.
 */
char *int_to_str(const int value) {
    const int len = snprintf(nullptr, 0, "%d", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    if (!str) {
        raise_error(
            "Can't allocate memory for int_to_str %d",
            value
        );
    }
    (void)snprintf(str, size, "%d", value);
    return str;
}

/**
 * Converts unsigned int to string. The result must be freed by the caller.
 */
char *uint_to_str(const unsigned int value) {
    const int len = snprintf(nullptr, 0, "%u", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    if (!str) {
        raise_error(
            "Can't allocate memory for uint_to_str %u",
            value
        );
    }
    (void)snprintf(str, size, "%u", value);
    return str;
}

/**
 * Converts string to long
 */
long str_to_long(const char *value) {
    char *end_ptr = nullptr;
    errno = 0;

    const long result = strtol(value, &end_ptr, 10);

    if (
        end_ptr == value ||
        *end_ptr != '\0' ||
        errno == ERANGE
    ) {
        raise_error("Failed to convert '%s' to long", value);
    }

    return result;
}

/**
 * Converts string to unsigned long
 */
unsigned long str_to_ulong(const char *value) {
    char *end_ptr = nullptr;
    errno = 0;

    const unsigned long result = strtoul(value, &end_ptr, 10);

    if (
        end_ptr == value ||
        *end_ptr != '\0' ||
        errno == ERANGE
    ) {
        raise_error("Failed to convert '%s' to ulong", value);
    }

    return result;
}

/**
 * Converts string to unsigned long long
 */
unsigned long long str_to_ull(const char *value) {
    char *end_ptr = nullptr;
    errno = 0;

    const unsigned long long result = strtoull(value, &end_ptr, 10);

    if (
        end_ptr == value ||
        *end_ptr != '\0' ||
        errno == ERANGE
    ) {
        raise_error("Failed to convert '%s' to ull", value);
    }

    return result;
}

/**
 * Converts string to int
 */
int str_to_int(const char *value) {
    const long result = str_to_long(value);
    if (result < INT_MIN || result > INT_MAX) {
        raise_error("Failed to convert '%s' to int", value);
    }
    return (int) result;
}

/**
 * Converts string to unsigned int
 */
unsigned int str_to_uint(const char *value) {
    const unsigned long result = str_to_ulong(value);
    if (result > UINT_MAX) {
        raise_error("Failed to convert '%s' to uint", value);
    }
    return (unsigned int) result;
}

/**
 * Converts string to unsigned int 16
 */
uint16_t str_to_uint16(const char *value) {
    const unsigned long result = str_to_ulong(value);
    if (result > UINT16_MAX) {
        raise_error("Failed to convert '%s' to uint16", value);
    }
    return (uint16_t) result;
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env(const char *env_name, char **result) {
    char *env_val = getenv(env_name);
    if (env_val && *env_val) {
        *result = env_val;
    }
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result) {
    const char *env_val = getenv(env_name);
    if (env_val && *env_val) {
        *result = str_to_uint(env_val);
    }
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_ull(const char *env_name, unsigned long long *result) {
    const char *env_val = getenv(env_name);
    if (env_val && *env_val) {
        *result = str_to_ull(env_val);
    }
}

/**
 * Takes a value from the environment variables if it is set,
 * copies it and pastes it by the result pointer.
 * The string must be freed by the caller.
 */
void replace_from_env_copy(const char *env_name, char **result) {
    const char *env_val = getenv(env_name);
    if (env_val != nullptr && *env_val) {
        char *env_val_copy = copy_string(env_val);
        *result = env_val_copy;
    }
}

/**
 * Creates a new json array object
 */
cJSON *json_array(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        raise_error("Can't create json array");
    }
    return arr;
}

/**
 * Creates a new json object
 */
cJSON *json_object(void) {
    cJSON *arr = cJSON_CreateObject();
    if (!arr) {
        raise_error("Can't create json object");
    }
    return arr;
}

/**
 * Adds a new key and string value to json
 */
void add_str_to_json_object(cJSON * obj, const char *key, const char *val) {
    if (!cJSON_AddStringToObject(obj, key, val)) {
        raise_error("Can't add str to object");
    }
}

/**
 * Adds a new key with value null to json
 */
void add_null_to_json_object(cJSON * obj, const char *key) {
    if (!cJSON_AddNullToObject(obj, key)) {
        raise_error("Can't add null to object");
    }
}

/**
 * Adds a new key and bool value to json
 */
void add_bool_to_json_object(cJSON * obj, const char *key, const bool val) {
    if (!cJSON_AddBoolToObject(obj, key, val)) {
        raise_error("Can't add bool to object");
    }
}

/**
 * Converts json to string.
 * The string must be freed by the caller.
 */
char *json_to_str(cJSON *json) {
    char *result = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!result) {
        raise_error("Can't convert json to string");
    }
    return result;
}
