#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H


#include <stdbool.h>
#include <sys/stat.h>

typedef struct PSFileDescriptor {
    int fd;
    struct stat st;
} PSFileDescriptor;

PSFileDescriptor ps_open(const char *path);

void ps_printf_error(const char *format, ...) __printflike(1, 2);

void ps_raise_error(const char *format, ...) __printflike(1, 2);

char *ps_concatenate_strings(const char *first, const char *second);

char *ps_copy_string(const char *str);

bool ps_is_equal_strings(const char *first, const char *second);

char *ps_format_string(const char *format, ...) __attribute__((format(printf, 1, 2)));

char *ps_ulong_to_str(const unsigned long value);

char *ps_long_to_str(const long value);

char *ps_int_to_str(const int value);

char *ps_uint_to_str(const int value);

long ps_str_to_long(const char *value);

unsigned long ps_str_to_ulong(const char *value);

int ps_str_to_int(const char *value);


#endif //PG_STATUS_UTILS_H
