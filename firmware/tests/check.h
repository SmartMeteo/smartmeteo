// The whole assert harness. Deliberately this small: a test suite is only believed if a person can
// read what it does in a minute, and anything bigger would need tests of its own.
#pragma once
#include <cstdio>
#include <cstring>
#include <cmath>

static int g_checks = 0, g_failed = 0;
static const char *g_area = "";

static inline void area(const char *a) { g_area = a; }

static inline void ok(bool cond, const char *what) {
    g_checks++;
    if (!cond) { g_failed++; printf("  ПРОВАЛ  %s: %s\n", g_area, what); }
}

static inline void eq_int(long got, long want, const char *what) {
    g_checks++;
    if (got != want) { g_failed++; printf("  ПРОВАЛ  %s: %s — получено %ld, ожидалось %ld\n",
                                          g_area, what, got, want); }
}

static inline void eq_str(const char *got, const char *want, const char *what) {
    g_checks++;
    if (!got || !want || strcmp(got, want) != 0)
        { g_failed++; printf("  ПРОВАЛ  %s: %s — получено \"%s\", ожидалось \"%s\"\n",
                             g_area, what, got ? got : "(null)", want); }
}

static inline void near(double got, double want, double tol, const char *what) {
    g_checks++;
    if (!(fabs(got - want) <= tol))
        { g_failed++; printf("  ПРОВАЛ  %s: %s — получено %.3f, ожидалось %.3f ± %.3f\n",
                             g_area, what, got, want, tol); }
}

static inline int report(const char *suite) {
    printf("%-22s проверок %3d, провалов %d\n", suite, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
