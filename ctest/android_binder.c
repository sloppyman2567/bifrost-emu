#define _GNU_SOURCE
#include "../include/bifrost/binder_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"binder check %d line %d failed errno=%d\n",checks,__LINE__,errno); return 1; } } while(0)
static int exchange(int fd, const void* input, size_t size, void* output, size_t capacity,
                    struct bf_binder_write_read* wr) {
    *wr = (struct bf_binder_write_read){size,0,(uintptr_t)input,capacity,0,(uintptr_t)output};
    return ioctl(fd,BF_BINDER_WRITE_READ,wr);
}
static size_t packet(uint8_t* buffer, uint32_t cmd, const void* data) {
    size_t n=((cmd >> 16) & 0x3fff); memcpy(buffer,&cmd,4); if(n) memcpy(buffer+4,data,n); return 4+n;
}
static int find(const uint8_t* data, size_t size, uint32_t cmd, struct bf_binder_transaction* tx) {
    for(size_t off=0; off+4<=size;) {
        uint32_t current; memcpy(&current,data+off,4); off+=4;
        size_t n=((current >> 16) & 0x3fff); if(n>size-off) return 0;
        if(current==cmd) { if(tx && n==sizeof(*tx)) memcpy(tx,data+off,n); return 1; }
        off+=n;
    } return 0;
}
struct worker { int fd, result, error; struct bf_binder_transaction tx; };
static void* receive(void* p) {
    struct worker* w=p; uint8_t input[4],out[256]; struct bf_binder_write_read wr;
    packet(input,BF_BC_ENTER_LOOPER,0);
    w->result=exchange(w->fd,input,4,out,sizeof(out),&wr);
    w->error=errno;
    if(!w->result && !find(out,wr.read_consumed,BF_BR_TRANSACTION,&w->tx)) w->result=-2;
    return 0;
}
static void interrupt_handler(int signal) { (void)signal; }
int main(void) {
    int server=open("/dev/binder",O_RDWR|O_NONBLOCK), client=open("/dev/binder",O_RDWR|O_NONBLOCK);
    CHECK(server>=0 && client>=0);
    struct stat device;
    CHECK(stat("/dev/binder",&device)==0 && S_ISCHR(device.st_mode));
    CHECK(fstat(client,&device)==0 && S_ISCHR(device.st_mode));
    CHECK(access("/dev/binder",R_OK|W_OK)==0);
    CHECK(access("/dev/binder",X_OK)==-1 && errno==EACCES);
    CHECK(syscall(439,-1234,"/dev/binder",R_OK|W_OK,0)==0);
    unsigned char sx[256]={0};
    CHECK(syscall(291,-1234,"/dev/binder",0,0x7ff,sx)==0);
    unsigned short mode;memcpy(&mode,sx+28,2);CHECK(S_ISCHR(mode));
    int32_t version=0; CHECK(ioctl(client,BF_BINDER_VERSION,&version)==0 && version==8);
    CHECK(ioctl(client,BF_BINDER_VERSION,(void*)-1)==-1 && errno==EINVAL);
    CHECK(ioctl(client,_IO('b',99),0)==-1 && errno==ENOTTY);
    size_t arena=65536;
    void* sm=mmap(0,arena,PROT_READ,MAP_PRIVATE,server,0);
    void* cm=mmap(0,arena,PROT_READ,MAP_PRIVATE,client,0);
    CHECK(sm!=MAP_FAILED && cm!=MAP_FAILED && sm!=cm);
    int alias=dup(client);CHECK(alias>=0 && close(client)==0);client=alias;
    CHECK(ioctl(client,BF_BINDER_VERSION,&version)==0 && version==8);
    CHECK(mmap(0,4096,PROT_READ,MAP_PRIVATE,client,0)==MAP_FAILED && errno==EBUSY);
    CHECK(mprotect(cm,arena,PROT_READ|PROT_WRITE)==-1 && errno==EACCES);
    struct bf_flat_binder manager={BF_BINDER_TYPE_BINDER,0,0x12345678,0xabcdef};
    CHECK(ioctl(server,BF_BINDER_SET_CONTEXT_MGR_EXT,&manager)==0);
    int32_t zero=0;
    CHECK(ioctl(client,BF_BINDER_SET_CONTEXT_MGR,&zero)==-1 && errno==EBUSY);
    uint8_t input[256],out[256]; struct bf_binder_write_read wr;
    size_t n=packet(input,BF_BC_ENTER_LOOPER,0);
    CHECK(exchange(server,input,n,out,sizeof(out),&wr)==-1 && errno==EAGAIN && wr.write_consumed==4);
    uint8_t value[]={0,1,2,255,0,9};
    struct bf_binder_transaction tx={0}; tx.code=42;tx.data_size=sizeof(value);tx.buffer=(uintptr_t)value;
    n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0 && wr.write_consumed==n && find(out,wr.read_consumed,BF_BR_TRANSACTION_COMPLETE,0));
    CHECK(exchange(server,0,0,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_TRANSACTION,&tx));
    CHECK(tx.code==42 && tx.target==manager.binder && tx.cookie==manager.cookie && tx.data_size==sizeof(value));
    CHECK(tx.buffer>=(uintptr_t)sm && tx.buffer+sizeof(value)<=(uintptr_t)sm+arena && memcmp((void*)(uintptr_t)tx.buffer,value,sizeof(value))==0);
    uint64_t received=tx.buffer;
    uint8_t answer[]={7,0,8}; tx=(struct bf_binder_transaction){0};tx.data_size=sizeof(answer);tx.buffer=(uintptr_t)answer;
    n=packet(input,BF_BC_REPLY,&tx); n+=packet(input+n,BF_BC_FREE_BUFFER,&received);
    CHECK(exchange(server,input,n,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_TRANSACTION_COMPLETE,0));
    CHECK(exchange(client,0,0,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_REPLY,&tx));
    CHECK(tx.buffer>=(uintptr_t)cm && tx.buffer<(uintptr_t)cm+arena && tx.data_size==3 && !memcmp((void*)(uintptr_t)tx.buffer,answer,3));
    uint64_t reply=tx.buffer;n=packet(input,BF_BC_FREE_BUFFER,&reply);
    CHECK(exchange(client,input,n,0,0,&wr)==0);
    CHECK(exchange(client,input,n,0,0,&wr)==-1 && errno==EINVAL && wr.write_consumed==0);
    tx=(struct bf_binder_transaction){0};tx.flags=BF_TF_ONE_WAY;tx.data_size=6;tx.buffer=(uintptr_t)value;
    n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0);
    CHECK(exchange(server,0,0,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_TRANSACTION,&tx) && (tx.flags&BF_TF_ONE_WAY));
    received=tx.buffer;n=packet(input,BF_BC_FREE_BUFFER,&received); CHECK(exchange(server,input,n,0,0,&wr)==0);
    tx=(struct bf_binder_transaction){0};n=packet(input,BF_BC_REPLY,&tx);
    CHECK(exchange(server,input,n,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_FAILED_REPLY,0));
    tx.offsets_size=8;n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_FAILED_REPLY,0));
    struct bf_binder_error error; CHECK(ioctl(client,BF_BINDER_GET_EXTENDED_ERROR,&error)==0 && error.param==-EINVAL);
    tx=(struct bf_binder_transaction){0};tx.target=99;n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_DEAD_REPLY,0));
    uint32_t bad=0xdeadbeef;CHECK(exchange(client,&bad,4,0,0,&wr)==-1 && errno==EINVAL && wr.write_consumed==0);
    tx=(struct bf_binder_transaction){0};n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n-1,0,0,&wr)==-1 && errno==EINVAL && wr.write_consumed==0);
    int hw=open("/dev/hwbinder",O_RDWR|O_NONBLOCK),vendor=open("/dev/vndbinder",O_RDWR|O_NONBLOCK);CHECK(hw>=0&&vendor>=0);
    CHECK(ioctl(hw,BF_BINDER_SET_CONTEXT_MGR,0)==0 && ioctl(vendor,BF_BINDER_SET_CONTEXT_MGR,0)==0);
    CHECK(close(hw)==0 && close(vendor)==0);
    CHECK(fcntl(server,F_SETFL,0)==0);
    struct worker w={server,-1,0,{0}}; pthread_t thread;
    CHECK(pthread_create(&thread,0,receive,&w)==0);
    tx=(struct bf_binder_transaction){0};tx.flags=BF_TF_ONE_WAY;tx.data_size=6;tx.buffer=(uintptr_t)value;
    n=packet(input,BF_BC_TRANSACTION,&tx); CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0);
    CHECK(pthread_join(thread,0)==0 && w.result==0 && !memcmp((void*)(uintptr_t)w.tx.buffer,value,6));
    received=w.tx.buffer;n=packet(input,BF_BC_FREE_BUFFER,&received);CHECK(exchange(server,input,n,0,0,&wr)==0);
    struct sigaction action={0};action.sa_handler=interrupt_handler;
    CHECK(sigaction(SIGUSR1,&action,0)==0);
    w.result=0; w.error=0;
    CHECK(pthread_create(&thread,0,receive,&w)==0);
    usleep(10000);
    CHECK(pthread_kill(thread,SIGUSR1)==0);
    CHECK(pthread_join(thread,0)==0 && w.result==-1 && w.error==EINTR);
    CHECK(fcntl(server,F_SETFL,O_NONBLOCK)==0);
    tx=(struct bf_binder_transaction){0};n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0);
    CHECK(close(server)==0);
    CHECK(exchange(client,0,0,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_DEAD_REPLY,0));
    int next=open("/dev/binder",O_RDWR|O_NONBLOCK);CHECK(next>=0 && ioctl(next,BF_BINDER_SET_CONTEXT_MGR,&zero)==0);
    void* nm=mmap(0,arena,PROT_READ,MAP_PRIVATE,next,0);CHECK(nm!=MAP_FAILED);
    CHECK(munmap(nm,4096)==0);
    tx=(struct bf_binder_transaction){0};n=packet(input,BF_BC_TRANSACTION,&tx);
    CHECK(exchange(client,input,n,out,sizeof(out),&wr)==0 && find(out,wr.read_consumed,BF_BR_FAILED_REPLY,0));
    close(next);close(client);munmap(sm,arena);munmap(cm,arena);munmap((char*)nm+4096,arena-4096);
    printf("android_binder: %d checks passed\n",checks);return 0;
}
