/*
 * limntest.h — Limnx shared test framework
 *
 * TAP-compatible output. Include this instead of duplicating check/pass/fail.
 *
 * Usage:
 *   #include "../limntest.h"  (from subsystem dir)
 *   #include "limntest.h"     (from tests/ root)
 *
 *   int main(void) {
 *       lt_suite("proc/fork");
 *       lt_ok(pid > 0, "fork returns child PID");
 *       lt_skip("mte_check", "not supported on this arch");
 *       return lt_done();
 *   }
 */
#ifndef LIMNTEST_H
#define LIMNTEST_H

#include "../libc/libc.h"

static int _lt_pass = 0;
static int _lt_fail = 0;
static int _lt_skip = 0;
static int _lt_total = 0;
static const char *_lt_suite_name = "unknown";

/* Declare test suite name (printed in header and summary) */
static inline void lt_suite(const char *name) {
    _lt_suite_name = name;
    printf("=== %s ===\n", name);
}

/* Assert a condition. Prints TAP-compatible ok/not ok line. */
static inline void lt_ok(int cond, const char *name) {
    _lt_total++;
    if (cond) {
        _lt_pass++;
        printf("  PASS: %s\n", name);
    } else {
        _lt_fail++;
        printf("  FAIL: %s\n", name);
    }
}

/* Skip a test with a reason (counted separately from pass/fail) */
static inline void lt_skip(const char *name, const char *reason) {
    _lt_total++;
    _lt_skip++;
    printf("  SKIP: %s (%s)\n", name, reason);
}

/* Print a diagnostic message (not a test result) */
static inline void lt_diag(const char *msg) {
    printf("  # %s\n", msg);
}

/* Print summary and return exit code (0=pass, 1=fail) */
static inline int lt_done(void) {
    printf("\n=== Results: %d passed, %d failed, %d skipped ===\n",
           _lt_pass, _lt_fail, _lt_skip);
    if (_lt_fail == 0)
        printf("ALL TESTS PASSED\n");
    return _lt_fail > 0 ? 1 : 0;
}

/* Helper: check if string contains substring */
static inline int lt_strcontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

#endif /* LIMNTEST_H */
