#define _GNU_SOURCE
#include "../include/bifrost/binder_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static int checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"Binder object check %d line %d failed errno=%d\n",checks,__LINE__,errno);return 1;} } while(0)
static unsigned char output[512];static struct bf_binder_write_read wr;
static int exchange(int fd,uint32_t command,const void* data) {
    unsigned char input[128];size_t size=command?4+((command>>16)&0x3fff):0;
    if(command) {memcpy(input,&command,4);if(size>4)memcpy(input+4,data,size-4);}
    wr=(struct bf_binder_write_read){size,0,(uintptr_t)input,sizeof(output),0,(uintptr_t)output};
    return ioctl(fd,BF_BINDER_WRITE_READ,&wr);
}
static int contains(uint32_t wanted,void* value) {
    for(size_t off=0;off+4<=wr.read_consumed;) {
        uint32_t command;memcpy(&command,output+off,4);off+=4;
        size_t size=(command>>16)&0x3fff;if(size>wr.read_consumed-off)return 0;
        if(command==wanted) {if(value && size)memcpy(value,output+off,size);return 1;}off+=size;
    }return 0;
}
static int write_command(int fd,uint32_t command,const void* data) {
    unsigned char input[128];size_t size=4+((command>>16)&0x3fff);
    memcpy(input,&command,4);if(size>4)memcpy(input+4,data,size-4);
    struct bf_binder_write_read request={size,0,(uintptr_t)input,0,0,0};
    return ioctl(fd,BF_BINDER_WRITE_READ,&request);
}
static int free_buffer(int fd,uint64_t buffer) {return write_command(fd,BF_BC_FREE_BUFFER,&buffer);}
int main(void) {
    int manager=open("/dev/binder",O_RDWR|O_NONBLOCK), service=open("/dev/binder",O_RDWR|O_NONBLOCK), client=open("/dev/binder",O_RDWR|O_NONBLOCK);
    CHECK(manager>=0 && service>=0 && client>=0);
    void* mm=mmap(0,65536,PROT_READ,MAP_PRIVATE,manager,0);
    void* sm=mmap(0,65536,PROT_READ,MAP_PRIVATE,service,0);
    void* cm=mmap(0,65536,PROT_READ,MAP_PRIVATE,client,0);
    CHECK(mm!=MAP_FAILED && sm!=MAP_FAILED && cm!=MAP_FAILED);
    CHECK(ioctl(manager,BF_BINDER_SET_CONTEXT_MGR,0)==0);
    CHECK(write_command(manager,BF_BC_ENTER_LOOPER,0)==0);
    struct bf_flat_binder object={BF_BINDER_TYPE_BINDER,0,0x12345678,0x87654321};
    uint64_t offset=0;
    struct bf_binder_transaction publish={0};publish.code=1;publish.data_size=sizeof(object);publish.buffer=(uintptr_t)&object;publish.offsets_size=8;publish.offsets=(uintptr_t)&offset;
    CHECK(exchange(service,BF_BC_TRANSACTION,&publish)==0 && contains(BF_BR_TRANSACTION_COMPLETE,0));
    // Initial ownership is delivered on the publishing thread, before it
    // can drop the outgoing Parcel, without requiring ENTER_LOOPER.
    struct bf_binder_ptr_cookie lifecycle;
    CHECK(contains(BF_BR_INCREFS,&lifecycle) && lifecycle.ptr==object.binder && lifecycle.cookie==object.cookie);
    CHECK(write_command(service,BF_BC_INCREFS_DONE,&lifecycle)==0);
    CHECK(contains(BF_BR_ACQUIRE,&lifecycle) && write_command(service,BF_BC_ACQUIRE_DONE,&lifecycle)==0);
    struct bf_binder_transaction tx;
    CHECK(exchange(manager,0,0)==0 && contains(BF_BR_TRANSACTION,&tx));
    struct bf_flat_binder transferred;memcpy(&transferred,(void*)(uintptr_t)tx.buffer,sizeof(transferred));
    CHECK(transferred.type==BF_BINDER_TYPE_HANDLE && transferred.binder>0 && transferred.cookie==0);
    CHECK(tx.offsets_size==8 && *(uint64_t*)(uintptr_t)tx.offsets==0);
    uint32_t manager_handle=transferred.binder;
    CHECK(write_command(manager,BF_BC_ACQUIRE,&manager_handle)==0 && free_buffer(manager,tx.buffer)==0);
    struct bf_binder_transaction reply={0};CHECK(exchange(manager,BF_BC_REPLY,&reply)==0);
    CHECK(write_command(service,BF_BC_ENTER_LOOPER,0)==0);
    CHECK(exchange(service,0,0)==0 && contains(BF_BR_REPLY,&tx));
    CHECK(free_buffer(service,tx.buffer)==0);
    struct bf_binder_transaction query={0};query.code=2;
    CHECK(exchange(client,BF_BC_TRANSACTION,&query)==0);
    CHECK(exchange(manager,0,0)==0 && contains(BF_BR_TRANSACTION,&tx));CHECK(free_buffer(manager,tx.buffer)==0);
    transferred.type=BF_BINDER_TYPE_HANDLE;transferred.binder=manager_handle;transferred.cookie=0;
    reply=(struct bf_binder_transaction){0};reply.buffer=(uintptr_t)&transferred;reply.data_size=sizeof(transferred);reply.offsets_size=8;reply.offsets=(uintptr_t)&offset;
    CHECK(exchange(manager,BF_BC_REPLY,&reply)==0);
    CHECK(exchange(client,0,0)==0 && contains(BF_BR_REPLY,&tx));
    memcpy(&transferred,(void*)(uintptr_t)tx.buffer,sizeof(transferred));
    uint32_t client_handle=transferred.binder;CHECK(transferred.type==BF_BINDER_TYPE_HANDLE && client_handle>0);
    CHECK(write_command(client,BF_BC_ACQUIRE,&client_handle)==0 && free_buffer(client,tx.buffer)==0);
    char text[]="service RPC";query=(struct bf_binder_transaction){0};query.target=client_handle;query.code=99;query.buffer=(uintptr_t)text;query.data_size=sizeof(text);
    CHECK(exchange(client,BF_BC_TRANSACTION,&query)==0);
    CHECK(exchange(service,0,0)==0 && contains(BF_BR_TRANSACTION,&tx));
    CHECK(tx.target==object.binder && tx.cookie==object.cookie && tx.code==99 && !memcmp((void*)(uintptr_t)tx.buffer,text,sizeof(text)));
    CHECK(free_buffer(service,tx.buffer)==0);
    int answer=123;reply=(struct bf_binder_transaction){0};reply.buffer=(uintptr_t)&answer;reply.data_size=4;
    CHECK(exchange(service,BF_BC_REPLY,&reply)==0);
    CHECK(exchange(client,0,0)==0 && contains(BF_BR_REPLY,&tx) && *(int*)(uintptr_t)tx.buffer==123);
    CHECK(free_buffer(client,tx.buffer)==0);
    // Passing a handle back to its original owner restores the local pointer/cookie.
    query.buffer=(uintptr_t)&transferred;query.data_size=sizeof(transferred);query.offsets=(uintptr_t)&offset;query.offsets_size=8;query.flags=BF_TF_ONE_WAY;
    CHECK(exchange(client,BF_BC_TRANSACTION,&query)==0);
    CHECK(exchange(service,0,0)==0 && contains(BF_BR_TRANSACTION,&tx));
    struct bf_flat_binder restored;memcpy(&restored,(void*)(uintptr_t)tx.buffer,sizeof(restored));
    CHECK(restored.type==BF_BINDER_TYPE_BINDER && restored.binder==object.binder && restored.cookie==object.cookie);
    CHECK(free_buffer(service,tx.buffer)==0);
    // Malformed offsets must not publish any partial object references.
    offset=4;publish.offsets=(uintptr_t)&offset;
    CHECK(exchange(service,BF_BC_TRANSACTION,&publish)==0 && contains(BF_BR_FAILED_REPLY,0));
    struct bf_binder_error error;CHECK(ioctl(service,BF_BINDER_GET_EXTENDED_ERROR,&error)==0 && error.param==-EINVAL);
    offset=0;object.type=0x66642a85; // BINDER_TYPE_FD is still unsupported.
    CHECK(exchange(service,BF_BC_TRANSACTION,&publish)==0 && contains(BF_BR_FAILED_REPLY,0));
    CHECK(ioctl(service,BF_BINDER_GET_EXTENDED_ERROR,&error)==0 && error.param==-ENOTSUP);
    CHECK(write_command(manager,BF_BC_RELEASE,&manager_handle)==0);
    CHECK(write_command(manager,BF_BC_RELEASE,&manager_handle)==-1 && errno==EINVAL);
    CHECK(write_command(client,BF_BC_RELEASE,&client_handle)==0);
    CHECK(exchange(service,0,0)==0 && contains(BF_BR_RELEASE,&lifecycle) && contains(BF_BR_DECREFS,0));
    CHECK(write_command(client,BF_BC_ACQUIRE,&client_handle)==-1 && errno==EINVAL);
    close(service);close(manager);close(client);munmap(mm,65536);munmap(sm,65536);munmap(cm,65536);
    printf("android_binder_objects: %d checks passed\n",checks);return 0;
}
