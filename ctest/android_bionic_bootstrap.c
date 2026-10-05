// Deliberately freestanding declarations: link the actual pinned Bionic DSOs,
// without accidentally using host glibc headers or its startup objects.
typedef unsigned long pthread_t;
extern char* getenv(const char*);
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
static __thread int tls_value __attribute__((aligned(64))) = 17;
static int constructed;
__attribute__((constructor)) static void initialize(void) { constructed = 41; }
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
    if (getauxval(6) != 4096 || getpagesize() != 4096 || !getauxval(7)) return 2;
    printf("Bionic: constructors and auxv passed (page=%d)\n", getpagesize());
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
