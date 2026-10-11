// Binder protocol-8 death recipients, scatter/gather fixups and pool limits.
#define _GNU_SOURCE
#include "../include/bifrost/binder_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"Binder features check %d line %d errno=%d\n",checks,__LINE__,errno); return 1; } } while (0)
static unsigned char output[1024];
static struct bf_binder_write_read wr;
static int send_command(int fd, uint32_t cmd, const void* value, int read_result) {
    unsigned char input[128]; size_t size = cmd ? 4 + ((cmd >> 16) & 0x3fff) : 0;
    if (cmd) { memcpy(input,&cmd,4); if (size>4) memcpy(input+4,value,size-4); }
    wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,read_result?sizeof(output):0,0,(uintptr_t)output};
    return ioctl(fd,BF_BINDER_WRITE_READ,&wr);
}
static int contains(uint32_t wanted, void* value) {
    for (size_t off=0; off+4<=wr.read_consumed;) {
        uint32_t cmd; memcpy(&cmd,output+off,4); off+=4;
        size_t size=(cmd>>16)&0x3fff; if(size>wr.read_consumed-off)return 0;
        if(cmd==wanted) {if(value&&size)memcpy(value,output+off,size);return 1;}off+=size;
    }return 0;
}
static int free_buffer(int fd, uint64_t buffer) {return send_command(fd,BF_BC_FREE_BUFFER,&buffer,0);}
struct pool_result {int fd, registered, extra, exited;};
static void* pool_worker(void* argument) {
    struct pool_result* result=argument;
    unsigned char input[4];uint32_t cmd=BF_BC_REGISTER_LOOPER;memcpy(input,&cmd,4);
    struct bf_binder_write_read request={4,0,(uintptr_t)input,0,0,0};
    result->registered=ioctl(result->fd,BF_BINDER_WRITE_READ,&request);
    request.write_consumed=0;result->extra=ioctl(result->fd,BF_BINDER_WRITE_READ,&request);
    result->exited=ioctl(result->fd,BF_BINDER_THREAD_EXIT,0);return 0;
}
struct death_wait { int fd; uint32_t handle; int result; uint64_t cookie; };
static void* death_worker(void* argument) {
    struct death_wait* state=argument;
    unsigned char input[12],out[512];uint32_t cmd=BF_BC_ENTER_LOOPER;
    memcpy(input,&cmd,4);cmd=BF_BC_ACQUIRE;memcpy(input+4,&cmd,4);memcpy(input+8,&state->handle,4);
    struct bf_binder_write_read request={12,0,(uintptr_t)input,sizeof(out),0,(uintptr_t)out};
    for (;;) {
        state->result=ioctl(state->fd,BF_BINDER_WRITE_READ,&request);
        if(state->result)return 0;
        for(size_t at=0;at+4<=request.read_consumed;) {
            memcpy(&cmd,out+at,4);
            if(cmd==BF_BR_DEAD_BINDER){memcpy(&state->cookie,out+at+4,8);return 0;}
            at+=4+((cmd>>16)&0x3fff);
        }
        request.write_size=request.write_consumed=request.read_consumed=0;
    }
}
static int reject(int fd, struct bf_binder_transaction_sg* sg, int expected) {
    struct bf_binder_error error;
    return send_command(fd,BF_BC_TRANSACTION_SG,sg,1)==0&&contains(BF_BR_FAILED_REPLY,0)&&
        ioctl(fd,BF_BINDER_GET_EXTENDED_ERROR,&error)==0&&error.param==-expected;
}
int main(void) {
    CHECK(sizeof(struct bf_binder_handle_cookie)==12&&sizeof(struct bf_binder_transaction_sg)==72);
    int manager=open("/dev/hwbinder",O_RDWR|O_NONBLOCK),service=open("/dev/hwbinder",O_RDWR|O_NONBLOCK);
    CHECK(manager>=0&&service>=0);
    void* arena=mmap(0,65536,PROT_READ,MAP_PRIVATE,manager,0);
    void* source_arena=mmap(0,65536,PROT_READ,MAP_PRIVATE,service,0);
    CHECK(arena!=MAP_FAILED&&source_arena!=MAP_FAILED);
    CHECK(ioctl(manager,BF_BINDER_SET_CONTEXT_MGR,0)==0);
    uint32_t max_threads=1;CHECK(ioctl(manager,BF_BINDER_SET_MAX_THREADS,&max_threads)==0);
    CHECK(send_command(manager,BF_BC_ENTER_LOOPER,0,0)==0);
    struct bf_flat_binder object={BF_BINDER_TYPE_BINDER,0,0x112233,0x445566};
    uint64_t zero=0;
    struct bf_binder_transaction tx={0};tx.flags=BF_TF_ONE_WAY;tx.data_size=sizeof(object);tx.buffer=(uintptr_t)&object;
    tx.offsets_size=8;tx.offsets=(uintptr_t)&zero;
    CHECK(send_command(service,BF_BC_TRANSACTION,&tx,0)==0);
    CHECK(send_command(manager,0,0,1)==0&&contains(BF_BR_TRANSACTION,&tx)&&contains(BF_BR_SPAWN_LOOPER,0));
    struct bf_flat_binder received;memcpy(&received,(void*)(uintptr_t)tx.buffer,sizeof(received));
    uint32_t handle=received.binder;CHECK(received.type==BF_BINDER_TYPE_HANDLE);
    CHECK(send_command(manager,BF_BC_ACQUIRE,&handle,0)==0&&free_buffer(manager,tx.buffer)==0);
    struct bf_binder_handle_cookie death={handle,0x1122334455667788ull};
    CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==0);
    CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==-1&&errno==EINVAL);
    CHECK(send_command(manager,BF_BC_CLEAR_DEATH_NOTIFICATION,&death,1)==0&&contains(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,&zero)&&zero==death.cookie);
    struct pool_result pool={manager,99,99,99};pthread_t thread;
    CHECK(!pthread_create(&thread,0,pool_worker,&pool)&&!pthread_join(thread,0));
    CHECK(pool.registered==0&&pool.extra==-1&&pool.exited==0);
    CHECK(!pthread_create(&thread,0,pool_worker,&pool)&&!pthread_join(thread,0));
    CHECK(pool.registered==-1&&pool.extra==-1&&pool.exited==0);
    // Nested pointers precede the FD-array fixup, as required by the driver.
    int file=memfd_create("binder-scatter",MFD_CLOEXEC);CHECK(file>=0&&write(file,"asgard!",8)==8);
    char child[]="scatter";
    struct {uint64_t pointer;int32_t files[2];uint64_t tag;} root={(uintptr_t)child,{file,file},0xabcdef};
    struct {struct bf_binder_buffer root,child;struct bf_binder_fd_array files;} objects={
        {BF_BINDER_TYPE_PTR,0,(uintptr_t)&root,sizeof(root),0,0},
        {BF_BINDER_TYPE_PTR,BF_BINDER_BUFFER_FLAG_HAS_PARENT,(uintptr_t)child,sizeof(child),0,0},
        {BF_BINDER_TYPE_FDA,0,2,0,8}};
    uint64_t offsets[]={0,sizeof(struct bf_binder_buffer),2*sizeof(struct bf_binder_buffer)};
    struct bf_binder_transaction_sg sg={0};sg.buffers_size=sizeof(root)+sizeof(child);
    sg.transaction.flags=BF_TF_ONE_WAY;sg.transaction.buffer=(uintptr_t)&objects;sg.transaction.data_size=sizeof(objects);
    sg.transaction.offsets=(uintptr_t)offsets;sg.transaction.offsets_size=sizeof(offsets);
    CHECK(send_command(service,BF_BC_TRANSACTION_SG,&sg,1)==0&&contains(BF_BR_TRANSACTION_COMPLETE,0));
    CHECK(send_command(manager,0,0,1)==0&&contains(BF_BR_TRANSACTION,&tx)&&contains(BF_BR_SPAWN_LOOPER,0));
    typeof(objects) copied;memcpy(&copied,(void*)(uintptr_t)tx.buffer,sizeof(copied));
    typeof(root)* actual=(void*)(uintptr_t)copied.root.buffer;
    CHECK(copied.root.buffer>=(uintptr_t)arena&&copied.root.buffer+sizeof(root)<=(uintptr_t)arena+65536);
    CHECK(copied.child.buffer==actual->pointer&&!memcmp((void*)(uintptr_t)actual->pointer,child,sizeof(child))&&actual->tag==root.tag);
    int fd1=actual->files[0],fd2=actual->files[1];char text[8];
    CHECK(fd1!=fd2&&pread(fd1,text,8,0)==8&&!memcmp(text,"asgard!",8)&&pread(fd2,text,8,0)==8);
    CHECK(free_buffer(manager,tx.buffer)==0&&fcntl(fd1,F_GETFD)==-1&&errno==EBADF);
    CHECK(fcntl(fd2,F_GETFD)==-1&&errno==EBADF&&fcntl(file,F_GETFD)>=0);
    objects.files.parent_offset=0;CHECK(reject(service,&sg,EINVAL));objects.files.parent_offset=8;
    objects.child.parent=1;CHECK(reject(service,&sg,EINVAL));objects.child.parent=0;
    objects.child.parent_offset=1;CHECK(reject(service,&sg,EINVAL));objects.child.parent_offset=0;
    sg.buffers_size-=8;CHECK(reject(service,&sg,EINVAL));sg.buffers_size+=8;
    root.files[1]=0x7fffffff;CHECK(reject(service,&sg,EBADF));root.files[1]=file;
    // SG replies use the caller's FD acceptance flag and have the same lifetime.
    struct bf_binder_transaction call={0};call.flags=BF_TF_ACCEPT_FDS;
    CHECK(send_command(service,BF_BC_TRANSACTION,&call,0)==0);
    CHECK(send_command(manager,0,0,1)==0&&contains(BF_BR_TRANSACTION,&tx)&&free_buffer(manager,tx.buffer)==0);
    sg.transaction.flags=0;
    CHECK(send_command(manager,BF_BC_REPLY_SG,&sg,0)==0);
    CHECK(send_command(service,0,0,1)==0&&contains(BF_BR_REPLY,&tx));
    memcpy(&copied,(void*)(uintptr_t)tx.buffer,sizeof(copied));actual=(void*)(uintptr_t)copied.root.buffer;
    fd1=actual->files[0];CHECK(pread(fd1,text,8,0)==8&&!memcmp(text,"asgard!",8));
    CHECK(free_buffer(service,tx.buffer)==0&&fcntl(fd1,F_GETFD)==-1&&errno==EBADF);
    // Clearing a queued death must wait for delivery and acknowledgement.
    CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==0);
    int flags=fcntl(manager,F_GETFL);CHECK(fcntl(manager,F_SETFL,flags&~O_NONBLOCK)==0);
    struct death_wait waiting={manager,handle,99,0};
    CHECK(!pthread_create(&thread,0,death_worker,&waiting));
    usleep(20000);CHECK(close(service)==0);
    CHECK(!pthread_join(thread,0)&&waiting.result==0&&waiting.cookie==death.cookie);
    CHECK(fcntl(manager,F_SETFL,flags)==0);
    CHECK(send_command(manager,BF_BC_CLEAR_DEATH_NOTIFICATION,&death,0)==0);
    zero=waiting.cookie;
    CHECK(send_command(manager,BF_BC_DEAD_BINDER_DONE,&zero,1)==0&&contains(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,&zero));
    CHECK(send_command(manager,BF_BC_DEAD_BINDER_DONE,&zero,0)==-1&&errno==EINVAL);
    // A new subscription to an already dead handle is delivered immediately.
    ++death.cookie;CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,1)==0&&contains(BF_BR_DEAD_BINDER,&zero)&&zero==death.cookie);
    CHECK(send_command(manager,BF_BC_DEAD_BINDER_DONE,&zero,0)==0);
    CHECK(send_command(manager,BF_BC_CLEAR_DEATH_NOTIFICATION,&death,1)==0&&contains(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,&zero));
    ++death.cookie;CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==0);
    CHECK(send_command(manager,BF_BC_CLEAR_DEATH_NOTIFICATION,&death,0)==0);
    CHECK(send_command(manager,0,0,1)==0&&contains(BF_BR_DEAD_BINDER,&zero)&&zero==death.cookie&&!contains(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,0));
    CHECK(send_command(manager,BF_BC_DEAD_BINDER_DONE,&zero,1)==0&&contains(BF_BR_CLEAR_DEATH_NOTIFICATION_DONE,&zero));
    ++death.cookie;CHECK(send_command(manager,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==0);
    CHECK(send_command(manager,BF_BC_RELEASE,&handle,0)==0);
    CHECK(send_command(manager,BF_BC_RELEASE,&handle,0)==0);
    CHECK(send_command(manager,0,0,1)==0&&!contains(BF_BR_DEAD_BINDER,0));
    // Handle zero refers to a guest context manager and supports death too.
    int observer=open("/dev/hwbinder",O_RDWR|O_NONBLOCK);CHECK(observer>=0);
    CHECK(send_command(observer,BF_BC_ENTER_LOOPER,0,0)==0);
    max_threads=0;CHECK(ioctl(observer,BF_BINDER_SET_MAX_THREADS,&max_threads)==0);
    death=(struct bf_binder_handle_cookie){0,0x123456789abc};
    CHECK(send_command(observer,BF_BC_REQUEST_DEATH_NOTIFICATION,&death,0)==0);
    CHECK(close(manager)==0);
    CHECK(send_command(observer,0,0,1)==0&&contains(BF_BR_DEAD_BINDER,&zero)&&zero==death.cookie&&!contains(BF_BR_SPAWN_LOOPER,0));
    close(observer);close(file);munmap(arena,65536);munmap(source_arena,65536);
    printf("android_binder_features: %d checks passed\n",checks);return 0;
}
