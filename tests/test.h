/* Minimal assertion framework for JARVIS tests (console programs).
 *
 *   CHECK(cond, msg)        fails the test run (non-zero exit) if cond is false
 *   CHECKF(cond, fmt, ...)  same, printf-style message
 *   WARN(cond, msg)         non-fatal: prints a warning (used for KNOWN BUG cases)
 *   TEST_SUMMARY()          prints counters, evaluates to the process exit code
 *
 * Messages go through printf only (never mix with wprintf on stdout). */
#ifndef JARVIS_TEST_H
#define JARVIS_TEST_H

#include <stdio.h>

static int t_pass, t_fail, t_warn;

#define CHECKF(cond, ...) do {                                              \
        if (cond) t_pass++;                                                 \
        else {                                                              \
            t_fail++;                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

#define CHECK(cond, msg) CHECKF(cond, "%s  [%s]", msg, #cond)

#define WARN(cond, msg) do {                                                \
        if (!(cond)) {                                                      \
            t_warn++;                                                       \
            printf("WARN %s:%d: %s  [%s]\n", __FILE__, __LINE__, msg, #cond); \
        }                                                                   \
    } while (0)

#define TEST_SECTION(name) printf("-- %s\n", name)

#define TEST_SUMMARY() (printf("%s: %d passed, %d failed, %d warnings\n",   \
                               t_fail ? "FAILED" : "OK", t_pass, t_fail, t_warn), \
                        fflush(stdout), t_fail ? 1 : 0)

#endif
