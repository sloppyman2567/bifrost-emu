// test_localtime_min.c — absolute minimal localtime test
#include <stdio.h>
#include <time.h>

int main(void) {
    time_t t = 0;
    struct tm *tm = localtime(&t);
    if (!tm) {
        printf("FAIL: localtime returned NULL\n");
        return 1;
    }
    printf("localtime(0) ok: isdst=%d\n", tm->tm_isdst);
    printf("PASS\n");
    return 0;
}
