// Deliberately freestanding declarations: link the actual pinned Bionic DSOs,
// without accidentally using host glibc headers or its startup objects.
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
extern unsigned long getauxval(unsigned long);
extern int getpagesize(void);
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
    if (tls_value != 17) return (void*)1;
    tls_value = 99;
    *__errno() = 22;
    return (void*)(unsigned long)(tls_value == 99 && *__errno() == 22 ? 42 : 2);
}
int main(int argc, char** argv, char** envp) {
    (void)argv; (void)envp;
    if (argc != 1 || constructed != 41 || tls_value != 17) return 1;
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
    if (pthread_create(&thread, 0, worker, 0) || pthread_join(thread, &result) ||
        result != (void*)42 || tls_value != 31 || *__errno() != 11) return 3;
    printf("Bionic: pthread and isolated TLS passed\n");
    void* library = dlopen("libm.so", 2); // RTLD_NOW
    if (!library) { printf("dlopen: %s\n", dlerror()); return 4; }
    double (*square_root)(double) = (double (*)(double))dlsym(library, "sqrt");
    if (!square_root || square_root(81.0) != 9.0 || dlclose(library)) return 5;
    if (getenv("BIFROST_ANDROID_DEVICE_TEST")) {
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
