#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"syscall option check %d line %d failed errno=%d\n",checks,__LINE__,errno);return 1;} } while(0)
static int inherited;
static void* child(void* ignored) {
    (void)ignored;
    inherited=prctl(39,0L,0L,0L,0L)==1 && prctl(30,0L,0L,0L,0L)==123456;
    prctl(29,789L,0L,0L,0L);
    prctl(29,0L,0L,0L,0L);
    inherited = inherited && prctl(30,0L,0L,0L,0L)==123456;
    return 0;
}
int main(void) {
    CHECK(prctl(39,0L,0L,0L,0L)==0);
    CHECK(prctl(38,0L,0L,0L,0L)==-1 && errno==EINVAL);
    CHECK(prctl(38,1L,1L,0L,0L)==-1 && errno==EINVAL);
    CHECK(prctl(38,1L,0L,0L,0L)==0 && prctl(39,0L,0L,0L,0L)==1);
    CHECK(prctl(39,1L,0L,0L,0L)==-1 && errno==EINVAL);
    CHECK(prctl(29,123456L,0L,0L,0L)==0 && prctl(30,0L,0L,0L,0L)==123456);
    pthread_t thread;CHECK(pthread_create(&thread,0,child,0)==0);
    CHECK(pthread_join(thread,0)==0 && inherited && prctl(30,0L,0L,0L,0L)==123456);
    CHECK(prctl(29,0L,0L,0L,0L)==0 && prctl(30,0L,0L,0L,0L)==50000);
    CHECK(prctl(4,0L,0L,0L,0L)==0 && prctl(3,0L,0L,0L,0L)==0);
    CHECK(prctl(4,2L,0L,0L,0L)==-1 && errno==EINVAL);
    CHECK(prctl(4,1L,0L,0L,0L)==0 && prctl(3,0L,0L,0L,0L)==1);
    CHECK(prctl(41,1L,0L,0L,0L)==0 && prctl(42,0L,0L,0L,0L)==1);
    CHECK(prctl(41,0L,0L,0L,0L)==0 && prctl(42,0L,0L,0L,0L)==0);
    CHECK(prctl(41,1L,1L,0L,0L)==-1 && errno==EINVAL);
    long query=syscall(283,0,0,0);
    CHECK(query>=0 && !(query & (128|256)));
    CHECK(syscall(283,3,0,0)==-1 && errno==EINVAL);
    CHECK(syscall(283,0,1,0)==-1 && errno==EINVAL);
    if(query&8) {
        CHECK(syscall(283,8,0,0)==-1 && errno==EPERM);
        CHECK(syscall(283,16,0,0)==0 && syscall(283,8,0,0)==0);
        CHECK((syscall(283,512,0,0)&16)!=0);
    }
    if(query&32) CHECK(syscall(283,64,0,0)==0 && syscall(283,32,0,0)==0);
    printf("android_syscall_options: %d checks passed\n",checks);return 0;
}
