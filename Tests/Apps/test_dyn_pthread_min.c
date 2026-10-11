// test_dyn_pthread_min.c — minimal glibc dynamic pthread test.
// Uses write() directly (unbuffered) to avoid printf buffering issues.
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static long counter = 0;

static void* worker(void* arg) {
    (void)arg;
    counter++;
    return NULL;
}

int main(void) {
    const char msg1[] = "start\n";
    write(1, msg1, sizeof(msg1) - 1);

    pthread_t t;
    int rc = pthread_create(&t, NULL, worker, NULL);
    
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "pthread_create returned %d\n", rc);
    write(1, buf, n);
    
    if (rc == 0) {
        pthread_join(t, NULL);
        n = snprintf(buf, sizeof(buf), "joined, counter=%ld\n", counter);
        write(1, buf, n);
    }

    const char msg2[] = "done\n";
    write(1, msg2, sizeof(msg2) - 1);
    return 0;
}
