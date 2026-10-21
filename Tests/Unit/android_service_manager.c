#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../../Include/Bifrost/BinderABI.h"
static int checks;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(x)) {                                                                                \
            printf("service manager failed line %d\n", __LINE__);                                  \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
static unsigned char output[512], parcel[512];
static size_t length;
static size_t read_capacity = sizeof(output);
static uint32_t target;
static struct bf_binder_write_read wr;
static void u32(uint32_t x) {
    memcpy(parcel + length, &x, 4);
    length += 4;
}
static void string16(const char *s) {
    size_t n = strlen(s);
    u32(n);
    for (size_t i = 0; i <= n; ++i) {
        uint16_t c = s[i];
        memcpy(parcel + length, &c, 2);
        length += 2;
    }
    while (length & 3)
        parcel[length++] = 0;
}
static void token(void) {
    length = 0;
    u32(0);
    u32(-1);
    u32(0x53595354);
    string16("android.os.IServiceManager");
}
static int write_command(int fd, uint32_t cmd, const void *data, int readback) {
    unsigned char input[128];
    size_t n = cmd ? 4 + ((cmd >> 16) & 0x3fff) : 0;
    memcpy(input, &cmd, 4);
    if (n > 4)
        memcpy(input + 4, data, n - 4);
    wr = (struct bf_binder_write_read){
        n, 0, (uintptr_t)input, readback ? read_capacity : 0, 0, (uintptr_t)output};
    return ioctl(fd, BF_BINDER_WRITE_READ, &wr);
}
static int find(uint32_t wanted, void *value) {
    for (size_t pos = 0; pos + 4 <= wr.read_consumed;) {
        uint32_t cmd;
        memcpy(&cmd, output + pos, 4);
        pos += 4;
        size_t n = (cmd >> 16) & 0x3fff;
        if (n > wr.read_consumed - pos)
            return 0;
        if (cmd == wanted) {
            if (value && n)
                memcpy(value, output + pos, n);
            return 1;
        }
        pos += n;
    }
    return 0;
}
static int transact(int fd, uint32_t code, uint64_t *offsets, size_t count,
                    struct bf_binder_transaction *reply) {
    struct bf_binder_transaction tx = {0};
    tx.target = target;
    // Java proxies pass FLAG_COLLECT_NOTED_APP_OPS plus TF_ACCEPT_FDS.
    // Neither flag changes the synchronous transaction into a one-way call.
    tx.flags = BF_TF_COLLECT_NOTED_APP_OPS | 0x10;
    tx.code = code;
    tx.data_size = length;
    tx.buffer = (uintptr_t)parcel;
    tx.offsets = (uintptr_t)offsets;
    tx.offsets_size = count * 8;
    return !write_command(fd, BF_BC_TRANSACTION, &tx, 1) && find(BF_BR_REPLY, reply);
}
static int release_reply(int fd, struct bf_binder_transaction *tx) {
    return write_command(fd, BF_BC_FREE_BUFFER, &tx->buffer, 0);
}

// Register `name` for the Binder (ptr, cookie) owned by endpoint `fd`, taking
// the publisher's INCREFS/ACQUIRE acknowledgements that precede the reply.
static int publish(int fd, const char *name, uint64_t ptr, uint64_t cookie) {
    struct bf_flat_binder flat = {BF_BINDER_TYPE_BINDER, 0, ptr, cookie};
    token();
    string16(name);
    uint64_t offset = length;
    memcpy(parcel + length, &flat, sizeof(flat));
    length += sizeof(flat);
    u32(12);
    u32(0);
    u32(8);
    struct bf_binder_transaction reply;
    if (!transact(fd, 3, &offset, 1, &reply) || reply.flags)
        return 0;
    struct bf_binder_ptr_cookie incref, acquire;
    int has_incref = find(BF_BR_INCREFS, &incref), has_acquire = find(BF_BR_ACQUIRE, &acquire);
    if (has_incref && write_command(fd, BF_BC_INCREFS_DONE, &incref, 0))
        return 0;
    if (has_acquire && write_command(fd, BF_BC_ACQUIRE_DONE, &acquire, 0))
        return 0;
    return !release_reply(fd, &reply);
}
// The handle `client` is given for `name`, or 0 when nothing is registered.
static uint32_t lookup(int fd, const char *name) {
    struct bf_binder_transaction reply;
    struct bf_flat_binder flat;
    token();
    string16(name);
    if (!transact(fd, 2, 0, 0, &reply))
        return ~0u;
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    uint32_t handle = flat.type == BF_BINDER_TYPE_HANDLE ? flat.binder : 0;
    if (handle && write_command(fd, BF_BC_ACQUIRE, &handle, 0))
        return ~0u;
    return release_reply(fd, &reply) ? ~0u : handle;
}
int main(void) {
    int server = open("/dev/binder", O_RDWR | O_NONBLOCK),
        client = open("/dev/binder", O_RDWR | O_NONBLOCK);
    CHECK(server >= 0 && client >= 0);
    void *a = mmap(0, 65536, PROT_READ, MAP_PRIVATE, server, 0);
    void *b = mmap(0, 65536, PROT_READ, MAP_PRIVATE, client, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED);
    CHECK(ioctl(server, BF_BINDER_SET_CONTEXT_MGR, 0) < 0);
    struct bf_binder_transaction reply;
    length = 0;
    CHECK(transact(client, 0x5f504e47, 0, 0, &reply));
    CHECK(reply.flags == 8 && *(int32_t *)(uintptr_t)reply.buffer == 0);
    CHECK(!release_reply(client, &reply));
    token();string16("processinfo");
    CHECK(transact(client, 2, 0, 0, &reply));
    struct bf_flat_binder process_info;
    memcpy(&process_info,(void*)(uintptr_t)(reply.buffer+4),sizeof(process_info));
    CHECK(process_info.type==BF_BINDER_TYPE_HANDLE&&process_info.binder);
    uint32_t process_handle=process_info.binder;
    CHECK(!write_command(client,BF_BC_ACQUIRE,&process_handle,0));
    CHECK(!release_reply(client,&reply));
    target=process_handle;
    for (uint32_t method=1;method<=2;++method) {
        length=0;u32(0);u32(-1);u32(0x53595354);string16("android.os.IProcessInfoService");
        u32(2);u32(-1);u32(0x7fffffff);u32(2);if(method==2)u32(2);
        CHECK(transact(client,method,0,0,&reply));
        const int32_t* values=(void*)(uintptr_t)reply.buffer;
        uid_t uid = getuid();
        if (method == 2 && uid != 0 && uid != 1000 && uid != 1013 && uid != 1047) {
            // The same fixture is included in a real APK under its app UID.
            // platform.xml grants score access to media/cameraserver, not apps.
            CHECK(!reply.flags && reply.data_size >= 12 && values[0] == -1);
            CHECK(values[1] == (int32_t)strlen("Requires GET_PROCESS_STATE_AND_OOM_SCORE"));
        } else {
            CHECK(!reply.flags&&values[0]==0&&values[1]==2&&values[2]==20&&values[3]==20);
            if(method==2)CHECK(reply.data_size==32&&values[4]==2&&values[5]==-10000&&values[6]==-10000&&values[7]==0);
            else CHECK(reply.data_size==20&&values[4]==0);
        }
        CHECK(!release_reply(client,&reply));
    }
    length=0;u32(0);u32(-1);u32(0x53595354);string16("android.os.IProcessInfoService");
    u32(1);u32(-1);u32(2);
    CHECK(transact(client,1,0,0,&reply)&&reply.flags==8);
    CHECK(!release_reply(client,&reply));
    target=0;
    token();
    string16("android.hardware.graphics.allocator.IAllocator/default");
    CHECK(transact(client, 7, 0, 0, &reply));
    CHECK(!reply.flags && reply.data_size == 8 && *(int32_t *)(uintptr_t)(reply.buffer + 4) == 1);
    CHECK(!release_reply(client, &reply));
    token();
    string16("android.hardware.graphics.allocator.IAllocator/missing");
    CHECK(transact(client, 7, 0, 0, &reply));
    CHECK(!reply.flags && *(int32_t *)(uintptr_t)(reply.buffer + 4) == 0);
    CHECK(!release_reply(client, &reply));
    token();
    string16("android.hardware.graphics.allocator.IAllocator");
    CHECK(transact(client, 8, 0, 0, &reply));
    CHECK(!reply.flags && *(int32_t *)(uintptr_t)(reply.buffer + 4) == 1
            && *(int32_t *)(uintptr_t)(reply.buffer + 8) == 7);
    CHECK(!release_reply(client, &reply));
    length = 0;
    CHECK(transact(client, 0x5f4e5446, 0, 0, &reply));
    CHECK(!reply.flags && *(int32_t *)(uintptr_t)reply.buffer == 26);
    CHECK(!release_reply(client, &reply));
    token();
    string16("missing");
    CHECK(transact(client, 2, 0, 0, &reply));
    CHECK(!reply.flags && !*(int32_t *)(uintptr_t)reply.buffer);
    struct bf_flat_binder flat;
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    CHECK(flat.type == BF_BINDER_TYPE_BINDER && !flat.binder);
    CHECK(reply.data_size == 32 && *(int32_t *)(uintptr_t)(reply.buffer + 28) == 0);
    CHECK(!release_reply(client, &reply));
    token();
    string16("bifrost.echo");
    uint64_t offset = length;
    flat = (struct bf_flat_binder){BF_BINDER_TYPE_BINDER, 0, 0x11110000, 0x1234};
    memcpy(parcel + length, &flat, sizeof(flat));
    length += sizeof(flat);
    u32(12); // Parcel::finishFlattenBinder: SYSTEM stability
    u32(0);
    u32(8);
    // Reject malformed/missing representations without registering a service.
    uint32_t bad_stability = 42, system_stability = 12;
    memcpy(parcel + offset + sizeof(flat), &bad_stability, 4);
    CHECK(transact(server, 3, &offset, 1, &reply));
    CHECK(reply.flags == 8 && *(int32_t *)(uintptr_t)reply.buffer == -22);
    CHECK(!release_reply(server, &reply));
    memcpy(parcel + offset + sizeof(flat), &system_stability, 4);
    length -= 4;
    CHECK(transact(server, 3, &offset, 1, &reply));
    CHECK(reply.flags == 8 && *(int32_t *)(uintptr_t)reply.buffer == -22);
    CHECK(!release_reply(server, &reply));
    length += 4;
    CHECK(transact(server, 3, &offset, 1, &reply));
    CHECK(!reply.flags && !*(int32_t *)(uintptr_t)reply.buffer);
    // The publishing thread must acquire its Binder before the reply permits
    // libbinder to recycle the registration Parcel. It need not be a looper.
    uint32_t command;
    memcpy(&command, output + 4, 4);
    CHECK(command == BF_BR_INCREFS);
    struct bf_binder_ptr_cookie ack;
    memcpy(&ack, output + 8, sizeof(ack));
    CHECK(ack.ptr == flat.binder && ack.cookie == flat.cookie);
    memcpy(&command, output + 8 + sizeof(ack), 4);
    CHECK(command == BF_BR_ACQUIRE);
    struct bf_binder_ptr_cookie acquired;
    memcpy(&acquired, output + 12 + sizeof(ack), sizeof(acquired));
    CHECK(acquired.ptr == ack.ptr && acquired.cookie == ack.cookie);
    CHECK(!write_command(server, BF_BC_INCREFS_DONE, &ack, 0));
    CHECK(!write_command(server, BF_BC_ACQUIRE_DONE, &acquired, 0));
    CHECK(!release_reply(server, &reply));
    token();
    string16("bifrost.echo");
    CHECK(transact(client, 2, 0, 0, &reply));
    CHECK(!reply.flags && reply.offsets_size == 8 && *(uint64_t *)(uintptr_t)reply.offsets == 4);
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    CHECK(flat.type == BF_BINDER_TYPE_HANDLE && flat.binder);
    CHECK(reply.data_size == 32 && *(int32_t *)(uintptr_t)(reply.buffer + 28) == 12);
    uint32_t handle = flat.binder;
    CHECK(!write_command(client, BF_BC_ACQUIRE, &handle, 0));
    CHECK(!release_reply(client, &reply));
    // Dispatch through the discovered handle, then reply through the raw driver.
    uint32_t request = 7;
    struct bf_binder_transaction echo = {0};
    echo.target = handle;
    echo.code = 99;
    echo.data_size = 4;
    echo.buffer = (uintptr_t)&request;
    CHECK(!write_command(client, BF_BC_TRANSACTION, &echo, 1));
    CHECK(find(BF_BR_TRANSACTION_COMPLETE, 0));
    CHECK(!write_command(server, BF_BC_ENTER_LOOPER, 0, 1));
    size_t received = wr.read_consumed;
    if (find(BF_BR_INCREFS, &ack))
        CHECK(!write_command(server, BF_BC_INCREFS_DONE, &ack, 0));
    wr.read_consumed = received;
    if (find(BF_BR_ACQUIRE, &ack))
        CHECK(!write_command(server, BF_BC_ACQUIRE_DONE, &ack, 0));
    // Save the received transaction before command writes replace the read buffer.
    // Notifications and transactions are returned together; read-only writes do
    // not overwrite output, although they reset wr.read_consumed.
    wr.read_consumed = received;
    CHECK(find(BF_BR_TRANSACTION, &echo));
    CHECK(echo.target == 0x11110000 && echo.cookie == 0x1234);
    CHECK(*(uint32_t *)(uintptr_t)echo.buffer == 7);
    CHECK(!release_reply(server, &echo));
    uint32_t answer = 14;
    echo = (struct bf_binder_transaction){0};
    echo.data_size = 4;
    echo.buffer = (uintptr_t)&answer;
    CHECK(!write_command(server, BF_BC_REPLY, &echo, 1));
    CHECK(!write_command(client, 0, 0, 1));
    CHECK(find(BF_BR_REPLY, &echo) && *(uint32_t *)(uintptr_t)echo.buffer == 14);
    CHECK(!release_reply(client, &echo));
    token();
    u32(8);
    CHECK(transact(client, 4, 0, 0, &reply));
    CHECK(!reply.flags && !*(int32_t *)(uintptr_t)reply.buffer &&
          *(int32_t *)(uintptr_t)(reply.buffer + 4) == 3);
    CHECK(!release_reply(client, &reply));
    token();
    u32(1);
    CHECK(transact(client, 4, 0, 0, &reply));
    CHECK(*(int32_t *)(uintptr_t)(reply.buffer + 4) == 0);
    CHECK(!release_reply(client, &reply));
    token();
    string16("bad name");
    CHECK(transact(client, 2, 0, 0, &reply));
    CHECK(reply.flags == 8 && *(int32_t *)(uintptr_t)reply.buffer == -22);
    CHECK(!release_reply(client, &reply));
    token();
    CHECK(transact(client, 5, 0, 0, &reply));
    CHECK(reply.flags == 8 && *(int32_t *)(uintptr_t)reply.buffer == -74);
    CHECK(!release_reply(client, &reply));
    token();
    string16("bifrost.echo");
    u32(42);
    CHECK(transact(client, 2, 0, 0, &reply));
    CHECK(reply.flags == 8);
    CHECK(!release_reply(client, &reply));
    // Split initial ownership and the reply across reads. Even a registered
    // looper must receive its thread's ownership work before the reply.
    token();
    string16("bifrost.echo");
    offset = length;
    flat = (struct bf_flat_binder){BF_BINDER_TYPE_BINDER, 0, 0x22220000, 0x5678};
    memcpy(parcel + length, &flat, sizeof(flat));
    length += sizeof(flat);
    u32(12);
    u32(0);
    u32(8);
    read_capacity = 24; // BR_NOOP + BR_INCREFS only
    CHECK(!transact(server, 3, &offset, 1, &reply));
    CHECK(wr.read_consumed == 24 && find(BF_BR_INCREFS, &ack));
    CHECK(ack.ptr == flat.binder && ack.cookie == flat.cookie);
    CHECK(!find(BF_BR_ACQUIRE, 0) && !find(BF_BR_REPLY, 0));
    CHECK(!write_command(server, BF_BC_INCREFS_DONE, &ack, 0));
    read_capacity = sizeof(output);
    CHECK(!write_command(server, 0, 0, 1));
    memcpy(&command, output + 4, 4);
    CHECK(command == BF_BR_ACQUIRE && find(BF_BR_ACQUIRE, &acquired));
    CHECK(acquired.ptr == flat.binder && acquired.cookie == flat.cookie);
    CHECK(find(BF_BR_REPLY, &reply) && !reply.flags);
    CHECK(!write_command(server, BF_BC_ACQUIRE_DONE, &acquired, 0));
    CHECK(!release_reply(server, &reply));
    // The registry must not keep a dead owner callable or advertise its service.
    CHECK(!close(server));
    token();
    string16("bifrost.echo");
    CHECK(transact(client, 2, 0, 0, &reply));
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    CHECK(!flat.binder);
    CHECK(!release_reply(client, &reply));
    CHECK(!write_command(client, BF_BC_RELEASE, &handle, 0));
    token();
    u32(8);
    CHECK(transact(client, 4, 0, 0, &reply));
    CHECK(!reply.flags && *(int32_t *)(uintptr_t)(reply.buffer + 4) == 2);
    CHECK(!release_reply(client, &reply));
    // A second publisher replaces a name while it lives; when it goes away the
    // name returns to the registration it replaced (Play services' window
    // session replaced the game's SurfaceFlinger, then removed it on exit).
    int first = open("/dev/binder", O_RDWR | O_NONBLOCK), second = open("/dev/binder", O_RDWR | O_NONBLOCK);
    void *c = mmap(0, 65536, PROT_READ, MAP_PRIVATE, first, 0);
    void *d = mmap(0, 65536, PROT_READ, MAP_PRIVATE, second, 0);
    CHECK(first >= 0 && second >= 0 && c != MAP_FAILED && d != MAP_FAILED);
    CHECK(publish(first, "bifrost.shared", 0x33330000, 0x1));
    uint32_t original = lookup(client, "bifrost.shared");
    CHECK(original && original != ~0u);
    CHECK(publish(second, "bifrost.shared", 0x44440000, 0x2));
    uint32_t newest = lookup(client, "bifrost.shared");
    CHECK(newest && newest != ~0u && newest != original);
    // The same publisher registering again replaces its own entry for good.
    CHECK(publish(second, "bifrost.shared", 0x44440008, 0x3));
    uint32_t again = lookup(client, "bifrost.shared");
    CHECK(again && again != ~0u && again != original);
    CHECK(!munmap(d, 65536) && !close(second));
    CHECK(lookup(client, "bifrost.shared") == original);
    CHECK(!munmap(c, 65536) && !close(first));
    CHECK(lookup(client, "bifrost.shared") == 0);
    CHECK(!munmap(a, 65536) && !munmap(b, 65536) && !close(client));
    printf("android_service_manager: ALL PASS (%d checks)\n", checks);
    return 0;
}
