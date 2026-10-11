// test_dyn_hello.c — minimal glibc dynamic test using write() only.
// Bypasses printf to isolate where glibc dynamic fails.
#include <unistd.h>
#include <string.h>

static const char hello[] = "Hello, glibc dynamic!\n";
static const char write_ok[] = "write ok\n";

int main(int argc, char **argv) {
    // Direct write — bypasses glibc stdio buffering entirely.
    write(1, hello, sizeof(hello) - 1);
    write(1, write_ok, sizeof(write_ok) - 1);
    return 0;
}
