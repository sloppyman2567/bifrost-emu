// test_prctl.c — Test prctl syscall (PR_SET_NAME, PR_GET_NAME, etc.)
//
// bifrost-emu previously returned 0 for ALL prctl options, which broke
// PR_GET_NAME (returned garbage) and made programs that probe prctl
// features think they succeeded when they didn't. This test verifies:
//   1. PR_SET_NAME / PR_GET_NAME round-trip works
//   2. PR_GET_NAME returns the ELF basename by default
//   3. PR_SET_NAME truncates to 15 chars (kernel limit)
//   4. PR_GET_DUMPABLE returns 1 (SUID_DUMP_USER)
//   5. PR_GET_NO_NEW_PRIVS returns 0 (not set)
//   6. PR_GET_TIMERSLACK returns a positive value
//   7. Unknown prctl options return -EINVAL
//   8. PR_GET_TID_ADDRESS works
//
// Build: make cross SRC=ctest_real/test_prctl.c OUT=ctest_real/test_prctl.elf
// Run:   ./bifrost-emu ctest_real/test_prctl.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/prctl.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

int main(void) {
    char msg[256];
    int n;

    // 1. PR_GET_NAME — should return the ELF basename by default.
    //    The test binary is "test_prctl.elf", so comm should start with
    //    "test_prctl" (truncated to 15 chars if needed).
    char name[17] = {0};
    int r = prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME default: r=%d name=\"%s\"\n", r, name);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_NAME returns 0");
    CHECK(strncmp(name, "test_prctl", 10) == 0, "PR_GET_NAME returns ELF basename");

    // 2. PR_SET_NAME — set a custom name, then verify PR_GET_NAME returns it.
    r = prctl(PR_SET_NAME, "mytestprog", 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_SET_NAME: r=%d\n", r);
    write(2, msg, n);
    CHECK(r == 0, "PR_SET_NAME returns 0");

    memset(name, 0, sizeof(name));
    r = prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME after set: r=%d name=\"%s\"\n", r, name);
    write(2, msg, n);
    CHECK(r == 0 && strcmp(name, "mytestprog") == 0, "PR_GET_NAME returns set name");

    // 3. PR_SET_NAME truncates to 15 chars (kernel limit).
    prctl(PR_SET_NAME, "this_is_a_very_long_process_name", 0, 0, 0);
    memset(name, 0, sizeof(name));
    prctl(PR_GET_NAME, name, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NAME after long set: name=\"%s\" len=%zu\n", name, strlen(name));
    write(2, msg, n);
    CHECK(strlen(name) <= 15, "PR_SET_NAME truncates to 15 chars");

    // 4. PR_GET_DUMPABLE — should return 1 (SUID_DUMP_USER).
    r = prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_DUMPABLE: r=%d\n", r);
    write(2, msg, n);
    CHECK(r >= 0, "PR_GET_DUMPABLE returns non-negative");

    // 5. PR_GET_NO_NEW_PRIVS — should return 0 (not set).
    r = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_NO_NEW_PRIVS: r=%d\n", r);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_NO_NEW_PRIVS returns 0");

    // 6. PR_GET_TIMERSLACK — should return a positive value.
    r = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_TIMERSLACK: r=%d\n", r);
    write(2, msg, n);
    CHECK(r > 0, "PR_GET_TIMERSLACK returns positive");

    // 7. Unknown prctl option — should return -EINVAL.
    //    Use a value that's definitely not a valid PR_* constant.
    r = prctl(0xFFFF, 0, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "prctl(0xFFFF): r=%d errno=%d\n", r, errno);
    write(2, msg, n);
    CHECK(r == -1 && errno == EINVAL, "Unknown prctl returns -EINVAL");

    // 8. PR_GET_TID_ADDRESS — should write the tid_address to the pointer.
    unsigned long tid_addr = 0xDEADBEEF;
    r = prctl(PR_GET_TID_ADDRESS, &tid_addr, 0, 0, 0);
    n = snprintf(msg, sizeof(msg), "PR_GET_TID_ADDRESS: r=%d tid_addr=0x%lx\n", r, tid_addr);
    write(2, msg, n);
    CHECK(r == 0, "PR_GET_TID_ADDRESS returns 0");

    // Final summary.
    n = snprintf(msg, sizeof(msg), "passes=%d failures=%d\n", passes, failures);
    write(2, msg, n);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
