// test_dyn_write.c — test that a dynamically-linked glibc binary can
// start, call write() directly (not printf), and exit cleanly.
// This verifies the dynamic linker loads libc.so.6 and resolves
// the write() symbol correctly. printf/sprintf have a separate
// SIMD-related issue that is being tracked separately.
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *msg = "dyn_write_ok\n";
    write(1, msg, strlen(msg));
    // Also test that malloc works (it's a common libc function)
    char *p = (char*)malloc(64);
    if (p) {
        p[0] = 'm'; p[1] = 'k'; p[2] = '\n'; p[3] = 0;
        write(1, p, 3);
        free(p);
    }
    return 0;
}
