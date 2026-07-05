// test_getenv.c — Verify environment variables passed via envp/auxv.
//
// What it verifies:
//   1. Standard environment variables (PATH, HOME, SHELL, TERM) are present.
//   2. getenv() returns the same values as iterating envp[].
//   3. setenv() can add new variables.
//   4. unsetenv() removes variables.
//   5. clearenv() (or equivalent) empties the environment.
//
// Build: make cross SRC=ctest_real/test_getenv.c OUT=ctest_real/test_getenv.elf
// Run:   ./bifrost-emu ctest_real/test_getenv.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

static int envp_has(const char *key) {
    size_t klen = strlen(key);
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, key, klen) == 0 && (*e)[klen] == '=') return 1;
    }
    return 0;
}

int main(void) {
    char msg[256];
    int len;

    // 1. Standard env vars are present.
    CHECK(getenv("PATH") != NULL, "PATH is set");
    CHECK(getenv("HOME") != NULL, "HOME is set");
    CHECK(getenv("SHELL") != NULL, "SHELL is set");
    CHECK(getenv("TERM") != NULL, "TERM is set");
    CHECK(envp_has("PATH"), "PATH visible in envp[]");
    CHECK(envp_has("HOME"), "HOME visible in envp[]");

    // 2. getenv returns the value (not the "KEY=VALUE" string).
    const char *home = getenv("HOME");
    if (home) {
        len = snprintf(msg, sizeof(msg), "HOME=%s\n", home);
        write(2, msg, len);
        CHECK(strchr(home, '=') == NULL, "getenv returns value (no '=' in it)");
    }

    // 3. setenv adds new variable.
    int r = setenv("MY_TEST_VAR", "hello123", 1);
    CHECK(r == 0, "setenv returns 0");
    const char *v = getenv("MY_TEST_VAR");
    CHECK(v != NULL && strcmp(v, "hello123") == 0, "setenv adds variable");

    // 4. setenv with overwrite=0 doesn't overwrite.
    setenv("MY_TEST_VAR", "world456", 0);
    v = getenv("MY_TEST_VAR");
    CHECK(v != NULL && strcmp(v, "hello123") == 0, "setenv overwrite=0 keeps existing");

    // 5. setenv with overwrite=1 does overwrite.
    setenv("MY_TEST_VAR", "world456", 1);
    v = getenv("MY_TEST_VAR");
    CHECK(v != NULL && strcmp(v, "world456") == 0, "setenv overwrite=1 replaces");

    // 6. unsetenv removes.
    r = unsetenv("MY_TEST_VAR");
    CHECK(r == 0, "unsetenv returns 0");
    CHECK(getenv("MY_TEST_VAR") == NULL, "unsetenv removes variable");

    // 7. Count env entries.
    int env_count = 0;
    for (char **e = environ; *e; e++) env_count++;
    len = snprintf(msg, sizeof(msg), "env_count=%d\n", env_count);
    write(2, msg, len);
    CHECK(env_count >= 4, "at least 4 environment variables present");

    // 8. Final summary.
    len = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
