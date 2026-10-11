#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

static int failures = 0;
#define CK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok:   %s\n", msg); } \
} while (0)

int main(void) {
    /* clock_gettime: result must be a plausible wall-clock time. */
    struct timespec ts;
    CK(clock_gettime(CLOCK_REALTIME, &ts) == 0, "clock_gettime(REALTIME) rc=0");
    CK(ts.tv_sec >= 1500000000 && ts.tv_sec < 2200000000LL, "clock_gettime tv_sec plausible");
    CK(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000LL, "clock_gettime tv_nsec in range");

    /* Monotonic must be non-decreasing across calls in a loop. */
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    long long last = (long long)a.tv_sec * 1000000000LL + a.tv_nsec;
    long long first = last;
    int monotonic_ok = 1;
    for (int i = 0; i < 100000; i++) {
        clock_gettime(CLOCK_MONOTONIC, &b);
        long long cur = (long long)b.tv_sec * 1000000000LL + b.tv_nsec;
        if (cur < last) { monotonic_ok = 0; break; }
        last = cur;
    }
    CK(monotonic_ok, "clock_gettime(MONOTONIC) non-decreasing over 100k calls");
    CK(last >= first, "monotonic advances");

    /* clock_getres: 1ns resolution. */
    struct timespec res;
    CK(clock_getres(CLOCK_REALTIME, &res) == 0, "clock_getres rc=0");
    CK(res.tv_sec == 0 && res.tv_nsec == 1, "clock_getres = {0,1}");

    /* gettimeofday: plausible timeval. */
    struct timeval tv;
    CK(gettimeofday(&tv, NULL) == 0, "gettimeofday rc=0");
    CK(tv.tv_sec >= 1500000000 && tv.tv_sec < 2200000000LL, "gettimeofday tv_sec plausible");
    CK(tv.tv_usec >= 0 && tv.tv_usec < 1000000LL, "gettimeofday tv_usec in range");

    printf(failures ? "FAILED: %d\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
