// test_env.c — Test host env var propagation to the guest.
//
// Without a sandbox bifrost-emu passes the caller's environment to the
// guest (PATH and HOME get a default only when the caller had none), so
// locale-aware programs see the user's TZ/LANG. This test verifies that:
//   1. PATH and HOME are set (their values are the caller's: not asserted)
//   2. localtime() + strftime() work without crashing
//   3. setenv() round-trips work (the guest can modify its own env)
//
// Note: We can't assert specific TZ/LANG values here because the test
// runner doesn't set them. We just verify the propagation mechanism
// doesn't crash and the core env vars are present.
//
// Build: make cross SRC=Tests/Apps/test_env.c OUT=Tests/Apps/test_env.elf
// Run:   ./bifrost-emu Tests/Apps/test_env.elf
//        TZ=America/New_York LANG=en_US.UTF-8 ./bifrost-emu Tests/Apps/test_env.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

int main(void) {
    char msg[256];

    // 1. Core env vars must always be present.
    const char* path = getenv("PATH");
    const char* home = getenv("HOME");
    const char* shell = getenv("SHELL");
    const char* term = getenv("TERM");
    snprintf(msg, sizeof(msg), "PATH=%s\n", path ? path : "(null)");
    write(2, msg, strlen(msg));
    CHECK(path && path[0] != '\0', "PATH is set");
    CHECK(home && home[0] != '\0', "HOME is set");
    snprintf(msg, sizeof(msg), "SHELL=%s TERM=%s\n", shell ? shell : "(unset)", term ? term : "(unset)");
    write(2, msg, strlen(msg));

    // 2. Verify localtime() + gmtime() don't crash.
    time_t now = time(NULL);
    struct tm* lt = localtime(&now);
    struct tm* gt = gmtime(&now);
    if (lt && gt) {
        passes++;
        write(2, "PASS: localtime() + gmtime() work\n", 34);
    } else {
        failures++;
        write(2, "FAIL: localtime() or gmtime() returned NULL\n", 44);
    }

    // 3. Verify strftime works (exercises the locale subsystem).
    char buf[64];
    if (lt) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", lt);
        snprintf(msg, sizeof(msg), "strftime: %s\n", buf);
        write(2, msg, strlen(msg));
        CHECK(strlen(buf) > 0, "strftime() produces non-empty output");
    }

    // 4. Verify setenv() round-trips work (the guest can modify its env).
    if (setenv("BIFROST_TEST_VAR", "hello123", 1) == 0) {
        const char* v = getenv("BIFROST_TEST_VAR");
        CHECK(v && strcmp(v, "hello123") == 0, "setenv() round-trip");
    } else {
        failures++;
        write(2, "FAIL: setenv() returned non-zero\n", 33);
    }

    // 5. Verify unsetenv() works.
    if (unsetenv("BIFROST_TEST_VAR") == 0) {
        const char* v = getenv("BIFROST_TEST_VAR");
        CHECK(v == NULL, "unsetenv() removes the var");
    } else {
        failures++;
        write(2, "FAIL: unsetenv() returned non-zero\n", 35);
    }

    // 6. Verify TZ is honored if set. If TZ=UTC, localtime should match
    //    gmtime exactly (same hour). This is a soft check — only runs
    //    if the test harness sets TZ=UTC.
    const char* tz = getenv("TZ");
    if (tz && strcmp(tz, "UTC") == 0 && lt && gt) {
        CHECK(lt->tm_hour == gt->tm_hour, "TZ=UTC: localtime matches gmtime");
    }

    // Final summary.
    snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, strlen(msg));
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
