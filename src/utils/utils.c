#include "utils.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <cjson/cJSON.h>

/**
 * Opens file descriptor in blocking mode and gets fstat
 */
FileDescriptor open_file(const char *path) {
    FileDescriptor mfd = {};

    mfd.fd = open(path, O_RDONLY);
    if (mfd.fd < 0) {
        printf_error("Failed to open file: %s", path);
        return mfd;
    }

    if (fstat(mfd.fd, &mfd.st) < 0) {
        printf_error("Failed to get fstat: %s\n", path);
        if (close(mfd.fd) < 0) {
            printf_error("Failed to close file: %s\n", path);
        }
        mfd.fd = -1;
    }
    return mfd;
}

/**
 * Prints the message in red with \n and also adds the error text from errno
 */
void printf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
}

/**
 * Prints the message in red with \n and also adds the
 * error text from errno and exit(1)
 */
void raise_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
    exit(1);
}

/**
 * Concatenates strings and returns the new string.
 * The result must be freed by the caller.
 */
char *concatenate_strings(const char *first, const char *second) {
    char *new = malloc(strlen(first) + strlen(second) + 1);
    strcpy(new, first);
    strcat(new, second);
    return new;
}

/**
 * Copies a string. The result must be freed by the caller.
 */
char *copy_string(const char *str) {
    if (str == nullptr)
        return nullptr;

    const size_t size = strlen(str) + 1;
    char *new_str = malloc(size);
    if (new_str == nullptr)
        return nullptr;

    memcpy(new_str, str, size);
    return new_str;
}

/**
 * Checks if strings are the same
 */
bool is_equal_strings(const char *first, const char *second) {
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
    if (vasprintf(&string, format, args) < 0) {
        printf_error("Unable to format_string %s", format);
        exit(1);
    }
    va_end(args);
    return string;
}

/**
 * Converts unsigned long to string. The result must be freed by the caller.
 */
char *ulong_to_str(const unsigned long value) {
    const int len = snprintf(nullptr, 0, "%lu", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%lu", value);
    return str;
}

/**
 * Converts long to string. The result must be freed by the caller.
 */
char *long_to_str(const long value) {
    const int len = snprintf(nullptr, 0, "%ld", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%ld", value);
    return str;
}

/**
 * Converts int to string. The result must be freed by the caller.
 */
char *int_to_str(const int value) {
    const int len = snprintf(nullptr, 0, "%d", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%d", value);
    return str;
}

/**
 * Converts unsigned int to string. The result must be freed by the caller.
 */
char *uint_to_str(const unsigned int value) {
    const int len = snprintf(nullptr, 0, "%u", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%u", value);
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
    )
        raise_error("Failed to convert '%s' to long", value);

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
    )
        raise_error("Failed to convert '%s' to ulong", value);

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
    )
        raise_error("Failed to convert '%s' to ull", value);

    return result;
}

/**
 * Converts string to int
 */
int str_to_int(const char *value) {
    return (int) str_to_long(value);
}

/**
 * Converts string to unsigned int
 */
unsigned int str_to_uint(const char *value) {
    return (unsigned int) str_to_ulong(value);
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env(const char *env_name, char **result) {
    char *env_val = getenv(env_name);
    if (env_val && *env_val)
        *result = env_val;
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result) {
    const char *env_val = getenv(env_name);
    if (env_val && *env_val)
        *result = str_to_uint(env_val);
}

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_ull(const char *env_name, unsigned long long *result) {
    const char *env_val = getenv(env_name);
    if (env_val && *env_val)
        *result = str_to_ull(env_val);
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
    if (!arr)
        raise_error("Can't create json array");
    return arr;
}

/**
 * Creates a new json object
 */
cJSON *json_object(void) {
    cJSON *arr = cJSON_CreateObject();
    if (!arr)
        raise_error("Can't create json object");
    return arr;
}

/**
 * Adds a new key and value to json
 */
void add_str_to_json_object(cJSON * obj, const char *key, const char *val) {
    if (!cJSON_AddStringToObject(obj, key, val)) {
        raise_error("Can't add str to object");
    }
}

/**
 * Converts json to string.
 * The string must be freed by the caller.
 */
char *json_to_str(cJSON *json) {
    char *result = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return result;
}
