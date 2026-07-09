// test_localtime_noZ.c — test localtime without %Z to isolate the bug
#include <stdio.h>
#include <time.h>

int main(void) {
    time_t t = 0;
    struct tm *tm = localtime(&t);
    if (!tm) { printf("FAIL: localtime NULL\n"); return 1; }
    printf("localtime(0) ok: isdst=%d\n", tm->tm_isdst);

    // Test strftime WITHOUT %Z
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm);
    printf("strftime (no %%Z): %s\n", buf);

    // Test %Z separately
    printf("tzname[0]=%s tzname[1]=s\n", tzname[0]);
    strftime(buf, sizeof(buf), "%Z", tm);
    printf("strftime %%Z: %s\n", buf);

    printf("PASS\n");
    return 0;
}
