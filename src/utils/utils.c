#include "utils.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/fcntl.h>
#include <unistd.h>

/*Opens file descriptor in blocking mode and gets fstat*/
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

/*Prints the message in red with \n and also adds the error text from errno*/
void printf_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
}

/*Prints the message in red with \n and also adds the error text from errno and exit(1)*/
void raise_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, ". strerror: %s\n", strerror(errno));
    fprintf(stderr, "\n");
    exit(1);
}

/*Concatenates strings and returns the new string created with malloc.*/
char *concatenate_strings(const char *first, const char *second) {
    char *new = malloc(strlen(first) + strlen(second) + 1);
    strcpy(new, first);
    strcat(new, second);
    return new;
}

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

bool is_equal_strings(const char *first, const char *second) {
    return strcmp(first, second) == 0;
}

/*Why not just asptinf?*/
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

char *ulong_to_str(const unsigned long value) {
    const int len = snprintf(nullptr, 0, "%lu", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%lu", value);
    return str;
}

char *long_to_str(const long value) {
    const int len = snprintf(nullptr, 0, "%ld", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%ld", value);
    return str;
}

char *int_to_str(const int value) {
    const int len = snprintf(nullptr, 0, "%d", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%d", value);
    return str;
}

char *uint_to_str(const unsigned int value) {
    const int len = snprintf(nullptr, 0, "%u", value);
    const size_t size = (size_t)len + 1;
    char *str = malloc(size);
    snprintf(str, size, "%u", value);
    return str;
}

long str_to_long(const char *value) {
    return strtol(value, nullptr, 10);
}

unsigned long str_to_ulong(const char *value) {
    return strtoul(value, nullptr, 10);
}

int str_to_int(const char *value) {
    return (int) strtol(value, nullptr, 10);
}
