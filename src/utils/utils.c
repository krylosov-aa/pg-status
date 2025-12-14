#include "utils.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/fcntl.h>
#include <unistd.h>

/*Opens file descriptor in blocking mode and gets fstat*/
PSFileDescriptor ps_open(const char *path) {
    PSFileDescriptor mfd = {};

    mfd.fd = open(path, O_RDONLY);
    if (mfd.fd < 0) {
        ps_printf_error("Failed to open file: %s", path);
        return mfd;
    }

    if (fstat(mfd.fd, &mfd.st) < 0) {
        ps_printf_error("Failed to get fstat: %s\n", path);
        if (close(mfd.fd) < 0) {
            ps_printf_error("Failed to close file: %s\n", path);
        }
        mfd.fd = -1;
    }
    return mfd;
}

/*Prints the message in red with \n and also adds the error text from errno*/
void ps_printf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
}

/*Prints the message in red with \n and also adds the error text from errno and exit(1)*/
void ps_raise_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
    exit(1);
}

/*Concatenates strings and returns the new string created with malloc.*/
char *ps_concatenate_strings(const char *first, const char *second) {
    char *new = malloc(strlen(first) + strlen(second) + 1);
    strcpy(new, first);
    strcat(new, second);
    return new;
}

char *ps_copy_string(const char *str) {
    char *new_str = malloc(strlen(str) + 1);
    strcpy(new_str, str);
    return new_str;
}

bool ps_is_equal_strings(const char *first, const char *second) {
    return strcmp(first, second) == 0;
}

/*Why not just asptinf?*/
char *ps_format_string(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *string = nullptr;
    if (vasprintf(&string, format, args) < 0) {
        ps_printf_error("Unable to format_string %s", format);
        exit(1);
    }
    va_end(args);
    return string;
}

char *ps_ulong_to_str(const unsigned long value) {
    int len = snprintf(nullptr, 0, "%lu", value);
    char *str = malloc((size_t)len + 1);
    sprintf(str, "%lu", value);
    return str;
}

char *ps_long_to_str(const long value) {
    int len = snprintf(nullptr, 0, "%ld", value);
    char *str = malloc((size_t)len + 1);
    sprintf(str, "%ld", value);
    return str;
}

char *ps_int_to_str(const int value) {
    int len = snprintf(nullptr, 0, "%d", value);
    char *str = malloc((size_t)len + 1);
    sprintf(str, "%d", value);
    return str;
}

char *ps_uint_to_str(const int value) {
    int len = snprintf(nullptr, 0, "%u", value);
    char *str = malloc((size_t)len + 1);
    sprintf(str, "%u", value);
    return str;
}

long ps_str_to_long(const char *value) {
    return strtol(value, nullptr, 10);
}

unsigned long ps_str_to_ulong(const char *value) {
    return strtoul(value, nullptr, 10);
}

int ps_str_to_int(const char *value) {
    return (int) strtol(value, nullptr, 10);
}
