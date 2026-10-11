// test_localtime.c — minimal repro for the interpreter tzfile assertion.
// Calls localtime() which exercises __tzfile_compute. If the interpreter
// has a bug in an instruction that tzfile.c uses, this will crash with
// the same glibc assertion as curl --version.
#include <stdio.h>
#include <time.h>
#include <string.h>

int main(void) {
    time_t t = 0;  // epoch
    struct tm *tm = localtime(&t);
    if (!tm) {
        printf("FAIL: localtime returned NULL\n");
        return 1;
    }
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", tm);
    printf("localtime(0) = %s\n", buf);
    printf("tm_isdst = %d\n", tm->tm_isdst);
    printf("tzname[0] = %s\n", tzname[0]);
    printf("tzname[1] = %s\n", tzname[1]);

    // Test a few more times to exercise different code paths
    time_t t2 = 0x60000000;  // ~1989
    struct tm *tm2 = localtime(&t2);
    if (tm2) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", tm2);
        printf("localtime(0x60000000) = %s\n", buf);
        printf("tm_isdst = %d\n", tm2->tm_isdst);
    }

    time_t t3 = 0x70000000;  // ~1996
    struct tm *tm3 = localtime(&t3);
    if (tm3) {
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", tm3);
        printf("localtime(0x70000000) = %s\n", buf);
        printf("tm_isdst = %d\n", tm3->tm_isdst);
    }

    printf("ALL PASS\n");
    return 0;
}
