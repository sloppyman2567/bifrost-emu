/* Self-loop back-edge writeback. Android 15 Bionic's __strrchr_aarch64 (Arm
 * optimized-routines) keeps its scan in a self-loop block whose CBZ jumps to
 * its own start. Each iteration's `mov x11, v17.d[1]` is carried into the
 * next iteration's `csel x7, x11, x7, ne`. After the unaligned entry compiled
 * the loop first, x11 stayed dirty in a host register across the back-edge,
 * so aligned scans paired the newest block pointer (x6) with an older match
 * mask (x7): Unity's il2cpp extraction got basenames like "ystem.Drawing...".
 * The instruction words are copied unchanged from that libc.so. */
#include <stdio.h>
#include <string.h>

__asm__(".text\n.balign 256\n.global bionic_strrchr\nbionic_strrchr:\n"
    ".inst 0xd503245f,0x52808024,0x72a80204,0x4e010c20,0x927be802,0x4e040c90\n"
    ".inst 0xd2800007,0xf2401003,0x4eb08607,0x54000340,0x4cdfa041,0xcb0303e3\n"
    ".inst 0x4e209823,0x6e208c25,0x4e209844,0x6e208c46,0x4e271c63,0x4e301ca5\n"
    ".inst 0x4e271c84,0x4e301cc6,0x4e24bc63,0x4e26bca5,0x4e25bc71,0x4e083e2a\n"
    ".inst 0xd37ff863,0x92800008,0x9ac32505,0x4e183e2b,0x8a25014a,0x8a25016b\n"
    ".inst 0xb500030a,0xd503201f,0xf100017f,0x9a861046,0x9a871167,0x4cdfa041\n"
    ".inst 0x6e208c25,0x6e208c46,0x6e22ac31,0x4e301ca5,0x4e301cc6,0x4e209a31\n"
    ".inst 0x4e26bca5,0x4e25be31,0x4e083e2a,0x4e183e2b,0xb4fffe4a,0x4e209823\n"
    ".inst 0x4e209844,0x4e271c63,0x4e271c84,0x4e24bc63,0x4e23bc63,0x4e083c6a\n"
    ".inst 0xd1000549,0xca0a0129,0xea09016b,0x9a861046,0x9a871167,0xdac010e5\n"
    ".inst 0x910008a5,0xcb4504c0,0xf10000ff,0x9a9f1000,0xd65f03c0\n"
    );
char *bionic_strrchr(const char *, int);
static char *(*volatile call_strrchr)(const char *, int) = bionic_strrchr;

static const char *reference(const char *s, int c) {
    const char *last = 0;
    do { if (*s == (char)c) last = s; } while (*s++);
    return last;
}

static char buf[1024] __attribute__((aligned(64)));

int main(void) {
    static const char *paths[] = {
        "/data/app/com.tiltingpoint.spongebob-4cfed5636e904aae7e9184cb/base.apk/assets/bin/Data/Managed/Resources",
        "/data/app/com.tiltingpoint.spongebob-4cfed5636e904aae7e9184cb/base.apk/assets/bin/Data/Managed/Resources/System.Drawing.dll-resources.dat",
        "assets/bin/Data/Managed/Resources", "a/b", "/", "noslash", ""};
    const int count = sizeof(paths) / sizeof(*paths);
    int failures = 0, checks = 0;
    // Unaligned scans first compile the loop the way the failing app did.
    for (int off = 1; off < 32; off++) {
        strcpy(buf + off, paths[0]);
        call_strrchr(buf + off, '/');
    }
    for (int fill = 0; fill < 2; fill++)
        for (int p = 0; p < count; p++)
            for (int off = 0; off < 64; off++) {
                memset(buf, fill ? '/' : 'x', sizeof(buf));
                strcpy(buf + off, paths[p]);
                const char *want = reference(buf + off, '/');
                const char *got = call_strrchr(buf + off, '/');
                checks++;
                if (got != want && failures++ < 4)
                    printf("FAIL path %d offset %d: index %ld, expected %ld\n", p, off,
                           got ? (long)(got - (buf + off)) : -1L,
                           want ? (long)(want - (buf + off)) : -1L);
            }
    if (failures) {
        printf("jit_selfloop_writeback: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("jit_selfloop_writeback: ALL PASS (%d checks)\n", checks);
    return 0;
}
