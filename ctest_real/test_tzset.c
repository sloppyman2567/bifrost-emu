// test_tzset.c — test tzset() which initializes __tzname
#include <stdio.h>
#include <time.h>

int main(void) {
    printf("Before tzset:\n");
    printf("  tzname[0]=%s tzname[1]=%s\n", tzname[0], tzname[1]);

    tzset();

    printf("After tzset:\n");
    printf("  tzname[0]=%s tzname[1]=%s\n", tzname[0], tzname[1]);

    printf("Calling localtime...\n");
    time_t t = 0;
    struct tm *tm = localtime(&t);
    if (tm) {
        printf("  localtime ok, isdst=%d\n", tm->tm_isdst);
    }
    printf("PASS\n");
    return 0;
}
