#ifndef T_H
#define T_H
#include <stdio.h>

static int t_failures = 0;
static int t_checks = 0;

#define CHECK(cond) do { t_checks++; if (!(cond)) { t_failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) do { t_checks++; long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { t_failures++; \
    printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define RUN(fn) do { printf("- %s\n", #fn); fn(); } while (0)

#define T_END() do { printf("%d checks, %d failures\n", t_checks, t_failures); \
    return t_failures ? 1 : 0; } while (0)

#endif
