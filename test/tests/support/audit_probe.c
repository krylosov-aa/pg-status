/** Deliberately broken child processes used to validate the audit gate. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(const int argc, char **argv) {
  if (argc != 2) {
    return 2;
  }
  fputs("expected failure diagnostic\n", stderr);
  fflush(stderr);
  if (strcmp(argv[1], "crash") == 0) {
    raise(SIGABRT);
  } else if (strcmp(argv[1], "diagnostic") == 0) {
    fputs("runtime error: injected audit diagnostic\n", stderr);
  } else if (strcmp(argv[1], "memory") == 0) {
#ifndef __clang_analyzer__
    // The analyzer must not reject a fixture whose defect is intentional.
    char *allocation = malloc(1);
    if (!allocation) {
      return 2;
    }
    volatile size_t offset = (size_t)argc;
    volatile char *destination = allocation;
    destination[offset] = 'x';
    free(allocation);
#endif
  } else if (strcmp(argv[1], "normal") != 0) {
    return 2;
  }
  return 1;
}
