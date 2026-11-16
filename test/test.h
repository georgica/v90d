/* Minimal C test harness: no external deps. Count checks, report, exit nonzero
 * on any failure. Portable C99; builds on x86 and the ARM target alike. */
#ifndef V90D_TEST_H
#define V90D_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int t_checks = 0, t_fails = 0;

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { t_fails++; \
        printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ_I(got, want) do { \
    long _g = (long)(got), _w = (long)(want); \
    t_checks++; \
    if (_g != _w) { t_fails++; \
        printf("  FAIL %s:%d: %s == %ld, want %ld\n", __FILE__, __LINE__, #got, _g, _w); } \
} while (0)

/* near: |got-want| <= tol */
#define CHECK_NEAR(got, want, tol) do { \
    double _g = (double)(got), _w = (double)(want), _t = (double)(tol); \
    t_checks++; \
    if (fabs(_g - _w) > _t) { t_fails++; \
        printf("  FAIL %s:%d: %s = %g, want %g +/- %g\n", __FILE__, __LINE__, #got, _g, _w, _t); } \
} while (0)

#define T_REPORT(name) do { \
    printf("%s: %d checks, %d failures\n", (name), t_checks, t_fails); \
    return t_fails ? 1 : 0; \
} while (0)

#endif
