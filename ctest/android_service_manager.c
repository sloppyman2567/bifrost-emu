#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../include/bifrost/binder_abi.h"
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
        n, 0, (uintptr_t)input, readback ? sizeof(output) : 0, 0, (uintptr_t)output};
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
    CHECK(!release_reply(client, &reply));
    token();
    string16("bifrost.echo");
    uint64_t offset = length;
    flat = (struct bf_flat_binder){BF_BINDER_TYPE_BINDER, 0, 0x11110000, 0x1234};
    memcpy(parcel + length, &flat, sizeof(flat));
    length += sizeof(flat);
    u32(0);
    u32(8);
    CHECK(transact(server, 3, &offset, 1, &reply));
    CHECK(!reply.flags && !*(int32_t *)(uintptr_t)reply.buffer);
    CHECK(!release_reply(server, &reply));
    token();
    string16("bifrost.echo");
    CHECK(transact(client, 2, 0, 0, &reply));
    CHECK(!reply.flags && reply.offsets_size == 8 && *(uint64_t *)(uintptr_t)reply.offsets == 4);
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    CHECK(flat.type == BF_BINDER_TYPE_HANDLE && flat.binder);
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
    struct bf_binder_ptr_cookie ack;
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
          *(int32_t *)(uintptr_t)(reply.buffer + 4) == 1);
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
    // The registry must not keep a dead owner callable or advertise its service.
    CHECK(!close(server));
    token();
    string16("bifrost.echo");
    CHECK(transact(client, 2, 0, 0, &reply));
    memcpy(&flat, (void *)(uintptr_t)(reply.buffer + 4), sizeof(flat));
    CHECK(!flat.binder);
    CHECK(!release_reply(client, &reply));
    CHECK(!write_command(client, BF_BC_RELEASE, &handle, 0));
    CHECK(!munmap(a, 65536) && !munmap(b, 65536) && !close(client));
    printf("android_service_manager: ALL PASS (%d checks)\n", checks);
    return 0;
}
