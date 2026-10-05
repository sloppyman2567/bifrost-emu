#define _GNU_SOURCE
#include "../include/bifrost/binder_abi.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
static atomic_int ready;
static int fd;
static void* worker(void* unused) {
    (void)unused;
    uint32_t enter=BF_BC_ENTER_LOOPER, output[64];
    struct bf_binder_write_read wr={4,0,(uintptr_t)&enter,sizeof(output),0,(uintptr_t)output};
    atomic_store(&ready,1);
    ioctl(fd,BF_BINDER_WRITE_READ,&wr);
    return 0;
}
int main(void) {
    fd=open("/dev/binder",O_RDWR);
    if(fd<0) return 1;
    pthread_t thread;if(pthread_create(&thread,0,worker,0)) return 2;
    while(!atomic_load(&ready)) usleep(1000);
    usleep(20000);
    puts("android_binder_exit: shutdown checks passed");fflush(stdout);
    _exit(0); // exit_group must wake the blocked Binder ioctl before joining.
}
