#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H


#include <stdbool.h>
#include <sys/stat.h>

typedef struct FileDescriptor {
    int fd;
    struct stat st;
} FileDescriptor;

FileDescriptor open_file(const char *path);

#ifndef __printflike
#  ifdef __GNUC__
#    define __printflike(fmtpos, argpos) __attribute__((format(printf, fmtpos, argpos)))
#  else
#    define __printflike(fmtpos, argpos)
#  endif
#endif

void printf_error(const char *format, ...) __printflike(1, 2);

void raise_error(const char *format, ...) __printflike(1, 2);

char *concatenate_strings(const char *first, const char *second);

char *copy_string(const char *str);

bool is_equal_strings(const char *first, const char *second);

char *format_string(const char *format, ...) __attribute__((format(printf, 1, 2)));

char *ulong_to_str(const unsigned long value);

char *long_to_str(const long value);

char *int_to_str(const int value);

char *uint_to_str(const unsigned int value);

long str_to_long(const char *value);

unsigned long str_to_ulong(const char *value);

int str_to_int(const char *value);

unsigned int str_to_uint(const char *value);

void replace_from_env(const char *env_name, char **result);

void replace_from_env_uint(const char *env_name, unsigned int *result);

void replace_from_env_copy(const char *env_name, char **result);

#endif //PG_STATUS_UTILS_H
