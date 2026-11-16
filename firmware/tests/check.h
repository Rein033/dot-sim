#ifndef CHECK_H_
#define CHECK_H_
#include <stdio.h>
#include <stdlib.h>
static int check_failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); check_failures++; } } while (0)
#define RUN(t) do { int f0 = check_failures; t(); printf("%s %s\n", check_failures == f0 ? "PASS" : "FAIL", #t); } while (0)
#define CHECK_DONE() return check_failures ? 1 : 0
#endif
