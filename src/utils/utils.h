#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H


#include <sys/stat.h>
#include <cjson/cJSON.h>

typedef struct FileDescriptor {
    int fd;
    struct stat st;
} FileDescriptor;

/**
 * Opens file descriptor in blocking mode and gets fstat
 */
FileDescriptor open_file(const char *path);

#ifndef __printflike
#  ifdef __GNUC__
#    define __printflike(fmtpos, argpos) __attribute__((format(printf, fmtpos, argpos)))
#  else
#    define __printflike(fmtpos, argpos)
#  endif
#endif

/**
 * Prints the message in red with \n and also adds the error text from errno
 */
void printf_error(const char *format, ...) __printflike(1, 2);

/**
 * Prints the message in red with \n and also adds the
 * error text from errno and exit(1)
 */
void raise_error(const char *format, ...) __printflike(1, 2);

/**
 * Concatenates strings and returns the new string.
 * The result must be freed by the caller.
 */
char *concatenate_strings(const char *first, const char *second);

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
char *format_string(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Converts unsigned long to string. The result must be freed by the caller.
 */
char *ulong_to_str(const unsigned long value);

/**
 * Converts long to string. The result must be freed by the caller.
 */
char *long_to_str(const long value);

/**
 * Converts int to string. The result must be freed by the caller.
 */
char *int_to_str(const int value);

/**
 * Converts unsigned int to string. The result must be freed by the caller.
 */
char *uint_to_str(const unsigned int value);

/**
 * Converts string to long
 */
long str_to_long(const char *value);

/**
 * Converts string to unsigned long
 */
unsigned long str_to_ulong(const char *value);

/**
 * Converts string to unsigned long long
 */
unsigned long long str_to_ull(const char *value);

/**
 * Converts string to int
 */
int str_to_int(const char *value);

/**
 * Converts string to unsigned int
 */
unsigned int str_to_uint(const char *value);

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env(const char *env_name, char **result);

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_uint(const char *env_name, unsigned int *result);

/**
 * Takes a value from the environment variables if it is set,
 * pastes it by the result pointer.
 */
void replace_from_env_ull(const char *env_name, unsigned long long *result);

/**
 * Takes a value from the environment variables if it is set,
 * copies it and pastes it by the result pointer.
 * The string must be freed by the caller.
 */
void replace_from_env_copy(const char *env_name, char **result);

/**
 * Creates a new json array object
 */
cJSON *json_array(void);

/**
 * Creates a new json object
 */
cJSON *json_object(void);

/**
 * Adds a new key and value to json
 */
void add_str_to_json_object(cJSON * obj, const char *key, const char *val);

/**
 * Adds a new key with value null to json
 */
void add_null_to_json_object(cJSON * obj, const char *key);

/**
 * Converts json to string.
 * The string must be freed by the caller.
 */
char *json_to_str(cJSON *json);

#endif //PG_STATUS_UTILS_H
