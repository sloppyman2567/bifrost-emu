// Deliberately freestanding declarations: link the actual pinned Bionic DSOs,
// without accidentally using host glibc headers or its startup objects.
#include "../../Include/Bifrost/BinderABI.h"
typedef unsigned long pthread_t;
extern char* getenv(const char*);
extern int strcmp(const char*, const char*);
extern int __system_property_get(const char*, char*);
extern int open(const char*, int, ...);
extern int close(int);
extern int access(const char*, int);
extern int ioctl(int, unsigned long, ...);
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int prctl(int, ...);
extern int printf(const char*, ...);
extern void* memcpy(void*, const void*, unsigned long);
extern unsigned long getauxval(unsigned long);
extern int getpagesize(void);
extern unsigned getuid(void);
extern unsigned geteuid(void);
extern unsigned getgid(void);
extern unsigned getegid(void);
extern long syscall(long, ...);
extern int* __errno(void);
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int dlclose(void*);
extern char* dlerror(void);
typedef struct FILE FILE;
extern FILE* fopen(const char*, const char*);
extern char* fgets(char*, int, FILE*);
extern int fclose(FILE*);
extern int sscanf(const char*, const char*, ...);
extern pthread_t pthread_self(void);
extern int pthread_getattr_np(pthread_t, void*);
extern int pthread_attr_getstack(const void*, void**, unsigned long*);
extern int pthread_attr_destroy(void*);
static __thread int tls_value __attribute__((aligned(64))) = 17;
static int constructed;
__attribute__((constructor)) static void initialize(void) { constructed = 41; }
static int check_identity(void) {
    unsigned ids[3], group = 0;
    if (getuid() != 10000 || geteuid() != 10000 || getgid() != 10000 || getegid() != 10000) return 1;
    for (unsigned long key = 11; key <= 14; ++key) if (getauxval(key) != 10000) return 2;
    for (long number = 148; number <= 150; number += 2) {
        if (syscall(number, &ids[0], &ids[1], &ids[2]) ||
            ids[0] != 10000 || ids[1] != 10000 || ids[2] != 10000) return 3;
    }
    if (syscall(158, 0, 0) != 1 || syscall(158, 1, &group) != 1 || group != 10000) return 4;
    FILE* status = fopen("/proc/self/status", "re");
    if (!status) return 5;
    char line[512]; unsigned real, effective, saved, fs; int found = 0;
    while (fgets(line, sizeof(line), status)) {
        if (sscanf(line, "Uid: %u %u %u %u", &real, &effective, &saved, &fs) == 4 ||
            sscanf(line, "Gid: %u %u %u %u", &real, &effective, &saved, &fs) == 4) {
            if (real != 10000 || effective != 10000 || saved != 10000 || fs != 10000) {
                fclose(status); return 6;
            }
            found++;
        }
    }
    fclose(status);
    if (found != 2) return 7;
    for (long number = 143; number <= 149; ++number) {
        if (number == 148) continue;
        if (syscall(number, 0, 0, 0) != -1 || *__errno() != 1) return 8; // EPERM
        if (syscall(number, 10000, 10000, 10000)) return 9;
    }
    if (syscall(147, -1, -1, -1) || syscall(149, -1, -1, -1)) return 10;
    if (syscall(151, 0) != 10000 || syscall(152, 0) != 10000 ||
        syscall(159, 0, 0) != -1 || *__errno() != 1 || getuid() != 10000) return 11;
    unsigned header[2] = {0x20080522, 0}, caps[6] = {1, 1, 1, 1, 1, 1};
    if (syscall(90, header, caps)) return 12;
    for (int i = 0; i < 6; ++i) if (caps[i]) return 13;
    if (syscall(91, header, caps)) return 14;
    caps[0] = 1;
    if (syscall(91, header, caps) != -1 || *__errno() != 1) return 15;
    header[0] = 0;
    if (syscall(90, header, 0) || header[0] != 0x20080522) return 16;
    return 0;
}
// Exercise the driver's real sender metadata, not Binder's local-call shortcut.
static int check_binder_identity(void) {
    int server = open("/dev/hwbinder", 2 | 0x800), client = open("/dev/hwbinder", 2 | 0x800);
    if (server < 0 || client < 0) return 1;
    void* sm = mmap(0, 65536, 1, 2, server, 0);
    void* cm = mmap(0, 65536, 1, 2, client, 0);
    if (sm == (void*)-1 || cm == (void*)-1) return 2;
    struct bf_flat_binder manager = {BF_BINDER_TYPE_BINDER, 0, 0x12345678, 0xabcdef};
    if (ioctl(server, BF_BINDER_SET_CONTEXT_MGR_EXT, &manager)) return 3;
    unsigned char input[4 + sizeof(struct bf_binder_transaction)], output[256];
    uint32_t cmd = BF_BC_ENTER_LOOPER;
    memcpy(input, &cmd, 4);
    struct bf_binder_write_read wr = {4, 0, (uintptr_t)input, sizeof(output), 0, (uintptr_t)output};
    if (ioctl(server, BF_BINDER_WRITE_READ, &wr) != -1 || *__errno() != 11) return 4;
    struct bf_binder_transaction tx = {0};
    tx.code = 42;
    tx.sender_euid = 0xffffffff; // sender cannot choose its credentials
    cmd = BF_BC_TRANSACTION;
    memcpy(input, &cmd, 4); memcpy(input + 4, &tx, sizeof(tx));
    wr = (struct bf_binder_write_read){sizeof(input), 0, (uintptr_t)input,
                                     sizeof(output), 0, (uintptr_t)output};
    if (ioctl(client, BF_BINDER_WRITE_READ, &wr)) return 5;
    wr = (struct bf_binder_write_read){0, 0, 0, sizeof(output), 0, (uintptr_t)output};
    if (ioctl(server, BF_BINDER_WRITE_READ, &wr)) return 6;
    int found = 0;
    for (unsigned long pos = 0; pos + 4 <= wr.read_consumed;) {
        memcpy(&cmd, output + pos, 4); pos += 4;
        unsigned long size = (cmd >> 16) & 0x3fff;
        if (size > wr.read_consumed - pos) return 7;
        if (cmd == BF_BR_TRANSACTION && size == sizeof(tx)) {
            memcpy(&tx, output + pos, sizeof(tx)); found = 1; break;
        }
        pos += size;
    }
    if (!found) return 7;
    if (tx.sender_euid != 10000 || tx.sender_pid != 1 || tx.code != 42) return 8;
    if (munmap(sm, 65536) || munmap(cm, 65536) || close(server) || close(client)) return 9;
    return 0;
}
// Bionic and ART find the main stack via the first /proc/self/maps line
// containing the initial SP, so lines must not overlap and the reported
// stack must contain this frame (ART madvises below it).
static int check_main_stack(void) {
    FILE* maps = fopen("/proc/self/maps", "re");
    if (!maps) return 1;
    char line[512];
    unsigned long previous_end = 0, low, high;
    int overlap = 0;
    while (fgets(line, sizeof(line), maps))
        if (sscanf(line, "%lx-%lx", &low, &high) == 2) {
            if (low < previous_end || high <= low) {
                if (!overlap) printf("overlapping maps line after %lx: %s", previous_end, line);
                overlap = 1;
            }
            previous_end = high;
        }
    fclose(maps);
    if (overlap) return 2;
    unsigned long attr[8] __attribute__((aligned(16))) = {0};
    void* base = 0;
    unsigned long size = 0;
    if (pthread_getattr_np(pthread_self(), attr) ||
        pthread_attr_getstack(attr, &base, &size) || pthread_attr_destroy(attr)) return 3;
    unsigned long frame = (unsigned long)__builtin_frame_address(0);
    return frame > (unsigned long)base && frame < (unsigned long)base + size ? 0 : 4;
}
static void* worker(void* unused) {
    (void)unused;
    // Linux gives every worker a TID different from the process leader.
    // Shared-Binder runs can have a low host PID inside a PID namespace.
    if (syscall(178) == syscall(172)) return (void*)3;
    if (tls_value != 17) return (void*)1;
    tls_value = 99;
    *__errno() = 22;
    return (void*)(unsigned long)(tls_value == 99 && *__errno() == 22 ? 42 : 2);
}
int main(int argc, char** argv, char** envp) {
    (void)argv; (void)envp;
    if (argc != 1 || constructed != 41 || tls_value != 17) return 1;
    int identity = check_identity();
    if (identity) { printf("Android app identity check failed: %d\n", identity); return 11; }
    printf("Bionic: Android app identity uid=10000 gid=10000 passed\n");
    char ready[92] = {0};
    const char* expected_ready = getenv("BIFROST_ANDROID_SERVICES_TEST") ? "true" : "false";
    if (__system_property_get("servicemanager.ready", ready) <= 0 ||
        strcmp(ready, expected_ready)) {
        printf("service-manager readiness: got '%s', expected '%s'\n", ready, expected_ready);
        return 10;
    }
    printf("Bionic: service-manager readiness %s passed\n", ready);
    if (getauxval(6) != 4096 || getpagesize() != 4096 || !getauxval(7)) return 2;
    printf("Bionic: constructors and auxv passed (page=%d)\n", getpagesize());
    int stack = check_main_stack();
    if (stack) { printf("main stack check failed: %d\n", stack); return 9; }
    printf("Bionic: /proc/self/maps and main-thread stack bounds passed\n");
    tls_value = 31;
    *__errno() = 11;
    pthread_t thread;
    void* result = 0;
    for (int i = 0; i < 16; ++i)
        if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, &result) ||
            result != (void*)42 || tls_value != 31 || *__errno() != 11) return 3;
    printf("Bionic: pthread identity and isolated TLS passed\n");
    void* library = dlopen("libm.so", 2); // RTLD_NOW
    if (!library) { printf("dlopen: %s\n", dlerror()); return 4; }
    double (*square_root)(double) = (double (*)(double))dlsym(library, "sqrt");
    if (!square_root || square_root(81.0) != 9.0 || dlclose(library)) return 5;
    if (getenv("BIFROST_ANDROID_DEVICE_TEST")) {
        int binder_identity = check_binder_identity();
        if (binder_identity) { printf("Binder sender identity check failed: %d\n", binder_identity); return 12; }
        printf("Bionic: Binder sender uid=10000 passed\n");
        int fd = open("/dev/binder", 2 | 0x800); // O_RDWR | O_NONBLOCK
        int version = 0;
        if (fd < 0 || access("/dev/binder", 6) || ioctl(fd, 0xc0046209UL, &version) || version != 8) return 6;
        void* arena = mmap(0, 65536, 1, 2, fd, 0); // PROT_READ, MAP_PRIVATE
        if (arena == (void*)-1 || munmap(arena, 65536) || close(fd)) return 7;
        if (prctl(38, 1L, 0L, 0L, 0L) || prctl(39, 0L, 0L, 0L, 0L) != 1) return 8;
        printf("Bionic: Binder device and prctl passed\n");
    }
    printf("android_bionic_bootstrap: ALL PASS\n");
    return 0;
}
