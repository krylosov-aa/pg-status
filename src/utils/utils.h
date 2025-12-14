#ifndef PG_STATUS_UTILS_H
#define PG_STATUS_UTILS_H


#include <stdbool.h>
#include <sys/stat.h>

typedef struct FileDescriptor {
    int fd;
    struct stat st;
} FileDescriptor;

FileDescriptor open_file(const char *path);

void printf_error(const char *format, ...) __printflike(1, 2);

void raise_error(const char *format, ...) __printflike(1, 2);

char *concatenate_strings(const char *first, const char *second);

char *copy_string(const char *str);

bool is_equal_strings(const char *first, const char *second);

char *format_string(const char *format, ...) __attribute__((format(printf, 1, 2)));

char *ulong_to_str(const unsigned long value);

char *long_to_str(const long value);

char *int_to_str(const int value);

char *uint_to_str(const int value);

long str_to_long(const char *value);

unsigned long str_to_ulong(const char *value);

int str_to_int(const char *value);


#endif //PG_STATUS_UTILS_H
