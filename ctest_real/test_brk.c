// test_brk.c — Verify brk() syscall behavior (heap expansion/contraction).
//
// What it verifies:
//   1. Initial brk(0) returns a non-zero, page-aligned address.
//   2. brk(addr) for a higher addr extends the heap; subsequent brk(0) returns
//      the new break.
//   3. Memory in the extended region is readable and writable.
//   4. brk(addr) for a lower addr contracts the heap.
//   5. brk with an absurdly high address fails (returns the current brk
//      without changing it).
//
// Build: make cross SRC=ctest_real/test_brk.c OUT=ctest_real/test_brk.elf
// Run:   ./bifrost-emu ctest_real/test_brk.elf
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

static int passes = 0;
static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { passes++; write(2, "PASS: " msg "\n", 7 + sizeof(msg)); } \
    else { failures++; write(2, "FAIL: " msg "\n", 7 + sizeof(msg)); } \
} while (0)

int main(void) {
    // 1. Get initial brk.
    void *initial_brk = (void *)syscall(214, 0);  // AArch64 brk syscall number
    CHECK(initial_brk != NULL, "initial brk(0) is non-null");
    CHECK(((unsigned long)initial_brk & 0xFFF) == 0, "initial brk is page-aligned");

    // 2. Extend by 64 KiB.
    void *new_brk = (void *)((char *)initial_brk + 65536);
    void *actual = (void *)syscall(214, new_brk);
    CHECK(actual == new_brk, "brk() extends heap to requested address");

    // 3. brk(0) should return the new break.
    void *current_brk = (void *)syscall(214, 0);
    CHECK(current_brk == new_brk, "brk(0) returns current break after extend");

    // 4. Memory in extended region is accessible.
    volatile char *p = (volatile char *)initial_brk;
    for (int i = 0; i < 16; i++) {
        p[i] = (char)(i + 1);
    }
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += p[i];
    }
    CHECK(sum == 136, "extended heap memory is read/write (sum=136)");

    // 5. Contract to original.
    actual = (void *)syscall(214, initial_brk);
    CHECK(actual == initial_brk, "brk() contracts heap back to initial");

    // 6. Absurdly high address — should fail (return current brk unchanged).
    void *absurd = (void *)0xFFFFFFFFFFFFFFFFUL;
    actual = (void *)syscall(214, absurd);
    CHECK(actual == initial_brk, "brk(absurd) fails, returns current brk");

    // 7. Final summary.
    char msg[128];
    int len = snprintf(msg, sizeof(msg),
                       "passes=%d failures=%d\n", passes, failures);
    write(2, msg, len);
    if (failures == 0) {
        write(2, "ALL PASS\n", 9);
        return 0;
    }
    write(2, "SOME FAILURES\n", 14);
    return 1;
}
