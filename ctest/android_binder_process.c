// Independent guest process Binder RPC, object round-trip and fd transfer.
#define _GNU_SOURCE
#include "../include/bifrost/binder_abi.h"
#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"process Binder line %d errno %d\n",__LINE__,errno); return 1; } } while (0)
struct scatter_root { uint64_t pointer; int32_t files[2]; uint64_t tag; };
struct payload {
    struct bf_flat_binder file, object; uint32_t pid, uid;
    struct bf_binder_buffer root, child;
    struct bf_binder_fd_array array;
};
static size_t command(uint8_t* data, uint32_t type, const void* value) {
    size_t size = ((type >> 16) & 0x3fff); memcpy(data,&type,4); if(size) memcpy(data+4,value,size); return size+4;
}
static int exchange(int fd, const void* data, size_t size, uint8_t* out, struct bf_binder_write_read* wr) {
    *wr=(struct bf_binder_write_read){size,0,(uintptr_t)data,512,0,(uintptr_t)out};
    return ioctl(fd,BF_BINDER_WRITE_READ,wr);
}
static int find(const uint8_t* data, size_t size, uint32_t wanted, struct bf_binder_transaction* tx) {
    for(size_t at=0;at+4<=size;) {
        uint32_t type;memcpy(&type,data+at,4);size_t length=((type >> 16) & 0x3fff);at+=4;
        if(length>size-at)return 0;
        if(type==wanted){if(tx)memcpy(tx,data+at,sizeof(*tx));return 1;} at+=length;
    }return 0;
}
static volatile sig_atomic_t signalled;
static int leader;
static void receive_signal(int sig) { signalled = sig; }
static void* identity_worker(void* unused) {
    (void)unused;
    if (getpid()!=leader || syscall(SYS_gettid)==leader || kill(leader,SIGUSR1))
        return (void*)1;
    return 0;
}
int main(int argc,char** argv) {
    CHECK(getpid()==syscall(SYS_gettid));
    leader=getpid();CHECK(signal(SIGUSR1,receive_signal)!=SIG_ERR);
    pthread_t thread;void* worker_result;
    CHECK(!pthread_create(&thread,0,identity_worker,0));
    CHECK(!pthread_join(thread,&worker_result)&&!worker_result&&signalled==SIGUSR1);
    int server=argc>1&&!strcmp(argv[1],"server");
    int death=argc>2&&!strcmp(argv[2],"death");
    int notify=argc>2&&!strcmp(argv[2],"notify");
    int fd=open("/dev/hwbinder",O_RDWR);CHECK(fd>=0);
    void* arena=mmap(0,65536,PROT_READ,MAP_PRIVATE,fd,0);CHECK(arena!=MAP_FAILED);
    uint8_t input[256],output[512];struct bf_binder_write_read wr;struct bf_binder_transaction tx={0};
    if(argc>2&&!strcmp(argv[2],"stress")) {
        enum {ROUNDS=200,CLIENTS=8};
        if(server) {
            CHECK(ioctl(fd,BF_BINDER_SET_CONTEXT_MGR,0)==0);
            puts("PROCESS_SERVER_READY");fflush(stdout);
            size_t size=command(input,BF_BC_ENTER_LOOPER,0);
            for(int i=0;i<ROUNDS*CLIENTS;++i) {
                do {CHECK(exchange(fd,input,size,output,&wr)==0);size=0;}
                while(!find(output,wr.read_consumed,BF_BR_TRANSACTION,&tx));
                CHECK(tx.data_size==4);
                uint32_t value=*(uint32_t*)(uintptr_t)tx.buffer^0x5a5a5a5a;
                uint64_t buffer=tx.buffer;
                tx=(struct bf_binder_transaction){0};tx.buffer=(uintptr_t)&value;tx.data_size=4;
                size=command(input,BF_BC_REPLY,&tx);size+=command(input+size,BF_BC_FREE_BUFFER,&buffer);
                wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,0,0,0};
                CHECK(ioctl(fd,BF_BINDER_WRITE_READ,&wr)==0);size=0;
            }
            puts("PROCESS_STRESS_SERVER_OK");
        } else {
            for(uint32_t i=0;i<ROUNDS;++i) {
                tx=(struct bf_binder_transaction){0};tx.buffer=(uintptr_t)&i;tx.data_size=4;
                size_t size=command(input,BF_BC_TRANSACTION,&tx);
                do {CHECK(exchange(fd,input,size,output,&wr)==0);size=0;}
                while(!find(output,wr.read_consumed,BF_BR_REPLY,&tx));
                CHECK(tx.data_size==4&&*(uint32_t*)(uintptr_t)tx.buffer==(i^0x5a5a5a5a));
                size=command(input,BF_BC_FREE_BUFFER,&tx.buffer);
                wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,0,0,0};
                CHECK(ioctl(fd,BF_BINDER_WRITE_READ,&wr)==0);
            }
            puts("PROCESS_STRESS_CLIENT_OK");
        }
        munmap(arena,65536);close(fd);return 0;
    }
    if(server) {
        struct bf_flat_binder manager={BF_BINDER_TYPE_BINDER,BF_FLAT_BINDER_FLAG_ACCEPTS_FDS,0x123400,0x987600};
        CHECK(ioctl(fd,BF_BINDER_SET_CONTEXT_MGR_EXT,&manager)==0);
        printf("PROCESS_SERVER_READY pid=%d uid=%d\n",getpid(),getuid());fflush(stdout);
        size_t size=command(input,BF_BC_ENTER_LOOPER,0);
        do { CHECK(exchange(fd,input,size,output,&wr)==0);size=0; }
        while(!find(output,wr.read_consumed,BF_BR_TRANSACTION,&tx));
        if(death){puts("PROCESS_SERVER_WAITING");fflush(stdout);for(;;)pause();}
        CHECK(tx.target==manager.binder&&tx.cookie==manager.cookie&&tx.data_size==sizeof(struct payload));
        struct payload payload;memcpy(&payload,(void*)(uintptr_t)tx.buffer,sizeof(payload));
        CHECK(payload.pid==(uint32_t)tx.sender_pid&&payload.uid==tx.sender_euid&&payload.pid!=(uint32_t)getpid());
        CHECK(payload.file.type==BF_BINDER_TYPE_FD&&payload.object.type==BF_BINDER_TYPE_HANDLE);
        char text[8]={0};CHECK(pread(payload.file.binder,text,7,0)==7&&!strcmp(text,"asgard!"));
        struct scatter_root* received_root=(void*)(uintptr_t)payload.root.buffer;
        CHECK(received_root->pointer==payload.child.buffer&&received_root->tag==0xabcdef);
        CHECK(!memcmp((void*)(uintptr_t)received_root->pointer,"scatter",8));
        CHECK(pread(received_root->files[0],text,7,0)==7&&!strcmp(text,"asgard!"));
        CHECK(pread(received_root->files[1],text,7,0)==7&&!strcmp(text,"asgard!"));
        if(notify) payload.object=(struct bf_flat_binder){BF_BINDER_TYPE_BINDER,0,0xabc123,0xdef456};
        payload.pid=getpid();payload.uid=getuid();
        uint64_t offsets[]={0,sizeof(struct bf_flat_binder),offsetof(struct payload,root),offsetof(struct payload,child),offsetof(struct payload,array)};
        uint64_t received=tx.buffer;tx=(struct bf_binder_transaction){0};
        tx.data_size=sizeof(payload);tx.buffer=(uintptr_t)&payload;tx.offsets_size=sizeof(offsets);tx.offsets=(uintptr_t)offsets;
        struct bf_binder_transaction_sg sg={tx,sizeof(struct scatter_root)+8};
        size=command(input,BF_BC_REPLY_SG,&sg);size+=command(input+size,BF_BC_FREE_BUFFER,&received);
        CHECK(exchange(fd,input,size,output,&wr)==0);
        puts("PROCESS_SERVER_OK");fflush(stdout);close(payload.file.binder);
        if(notify){puts("PROCESS_SERVER_WAITING");fflush(stdout);for(;;)pause();}
    } else {
        int file=memfd_create("asgard-process-test",MFD_CLOEXEC);CHECK(file>=0&&write(file,"asgard!",7)==7);
        char child[]="scatter";struct scatter_root root={(uintptr_t)child,{file,file},0xabcdef};
        struct payload payload={{BF_BINDER_TYPE_FD,0,file,0},{BF_BINDER_TYPE_BINDER,0,0x765400,0xabcdef},getpid(),getuid(),
            {BF_BINDER_TYPE_PTR,0,(uintptr_t)&root,sizeof(root),0,0},
            {BF_BINDER_TYPE_PTR,BF_BINDER_BUFFER_FLAG_HAS_PARENT,(uintptr_t)child,sizeof(child),2,0},
            {BF_BINDER_TYPE_FDA,0,2,2,8}};
        uint64_t offsets[]={0,sizeof(struct bf_flat_binder),offsetof(struct payload,root),offsetof(struct payload,child),offsetof(struct payload,array)};
        tx.flags=BF_TF_ACCEPT_FDS;tx.data_size=sizeof(payload);tx.buffer=(uintptr_t)&payload;
        tx.offsets_size=sizeof(offsets);tx.offsets=(uintptr_t)offsets;
        struct bf_binder_transaction_sg sg={tx,sizeof(root)+sizeof(child)};
        size_t size=command(input,BF_BC_TRANSACTION_SG,&sg);
        do {CHECK(exchange(fd,input,size,output,&wr)==0);size=0;}
        while(!find(output,wr.read_consumed,death?BF_BR_DEAD_REPLY:BF_BR_REPLY,&tx));
        if(!death) {
            CHECK(tx.data_size==sizeof(payload));memcpy(&payload,(void*)(uintptr_t)tx.buffer,sizeof(payload));
            CHECK(tx.sender_pid==0&&payload.uid==tx.sender_euid&&payload.pid!=(uint32_t)getpid());
            CHECK(payload.file.type==BF_BINDER_TYPE_FD);
            if(!notify) {
                CHECK(payload.object.type==BF_BINDER_TYPE_BINDER);
                CHECK(payload.object.binder==0x765400&&payload.object.cookie==0xabcdef);
            } else CHECK(payload.object.type==BF_BINDER_TYPE_HANDLE);
            char text[8]={0};CHECK(pread(payload.file.binder,text,7,0)==7&&!strcmp(text,"asgard!"));
            CHECK(payload.uid!=getuid());close(payload.file.binder);
            struct scatter_root* received_root=(void*)(uintptr_t)payload.root.buffer;
            CHECK(received_root->pointer==payload.child.buffer&&!memcmp((void*)(uintptr_t)received_root->pointer,"scatter",8));
            int array_fd=received_root->files[0];
            CHECK(pread(array_fd,text,7,0)==7&&!strcmp(text,"asgard!"));
            uint64_t received=tx.buffer;
            if(notify) {
                uint32_t handle=payload.object.binder;
                struct bf_binder_handle_cookie death_cookie={handle,0xfedcba9876543210ull};
                size=command(input,BF_BC_ACQUIRE,&handle);
                size+=command(input+size,BF_BC_REQUEST_DEATH_NOTIFICATION,&death_cookie);
                size+=command(input+size,BF_BC_FREE_BUFFER,&received);
                wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,0,0,0};
                CHECK(ioctl(fd,BF_BINDER_WRITE_READ,&wr)==0);
                puts("PROCESS_NOTIFY_READY");fflush(stdout);
                size=command(input,BF_BC_ENTER_LOOPER,0);
                do {CHECK(exchange(fd,input,size,output,&wr)==0);size=0;}
                while(!find(output,wr.read_consumed,BF_BR_DEAD_BINDER,0));
                uint64_t cookie=0;
                for(size_t at=0;at+4<=wr.read_consumed;) {
                    uint32_t cmd;memcpy(&cmd,output+at,4);
                    if(cmd==BF_BR_DEAD_BINDER)memcpy(&cookie,output+at+4,8);
                    at+=4+((cmd>>16)&0x3fff);
                }
                CHECK(cookie==death_cookie.cookie);
                size=command(input,BF_BC_CLEAR_DEATH_NOTIFICATION,&death_cookie);
                size+=command(input+size,BF_BC_DEAD_BINDER_DONE,&cookie);
                CHECK(exchange(fd,input,size,output,&wr)==0&&find(output,wr.read_consumed,BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,0));
                puts("PROCESS_NOTIFY_OK");
            } else {
                size=command(input,BF_BC_FREE_BUFFER,&received);
                wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,0,0,0};
                CHECK(ioctl(fd,BF_BINDER_WRITE_READ,&wr)==0);
            }
            CHECK(fcntl(array_fd,F_GETFD)==-1&&errno==EBADF);
        }
        close(file);puts(death?"PROCESS_DEATH_OK":"PROCESS_CLIENT_OK");fflush(stdout);
    }
    munmap(arena,65536);close(fd);return 0;
}
