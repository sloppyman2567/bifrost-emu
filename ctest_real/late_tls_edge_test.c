#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

// Make the frozen TP anchor differ from the high alignment requested by
// startup TLS. The late module must still align its TLS on every thread.
__thread volatile char startup_tls[256] __attribute__((aligned(256))) = {7};
static void *(*pointer_addr[2])(void);
static void *(*expected_addr[2])(void);
static void *(*local_pointer_addr[2])(void);
static void *(*local_expected_addr[2])(void);
static void *(*aligned_addr)(void);
static void *(*optional_addr)(void);
static _Atomic int ready;

static void *load(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/bifrost_late_tls_pthread/%s.so", name);
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) fprintf(stderr, "%s: %s\n", path, dlerror());
    return handle;
}

static int check(void) {
    if (startup_tls[0] != 7) return 1;
    for (int i = 0; i < 2; ++i) {
        if (pointer_addr[i] && (pointer_addr[i]() != expected_addr[i]() ||
            local_pointer_addr[i]() != local_expected_addr[i]())) {
            fprintf(stderr, "relocated TLS pointers failed (module %d)\n", i);
            return 1;
        }
    }
    void *address = aligned_addr ? aligned_addr() : NULL;
    if (aligned_addr && ((uintptr_t)address % 64 != 0 || *(char *)address != 9)) {
        fprintf(stderr, "late TLS alignment/value failed: %p\n", address);
        return 1;
    }
    if (optional_addr && optional_addr() != NULL) {
        fprintf(stderr, "undefined weak TLS did not resolve to NULL\n");
        return 1;
    }
    return 0;
}

static void *lookup(void *handle, const char *module, const char *symbol) {
    char name[256];
    snprintf(name, sizeof(name), "%s_%s", module, symbol);
    void *address = dlsym(handle, name);
    if (!address) fprintf(stderr, "missing regression symbol: %s\n", name);
    return address;
}

static void *worker(void *unused) {
    (void)unused;
    while (!atomic_load_explicit(&ready, memory_order_acquire))
        __asm__ volatile("yield");
    return (void *)(uintptr_t)check();
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    int pointers = strcmp(argv[1], "pointers") == 0;
    int alignment = strcmp(argv[1], "alignment") == 0;
    int weak = strcmp(argv[1], "weak") == 0;
    if (!pointers && !alignment && !weak) return 2;
    // Keep a real pthread alive before any of the DSOs is loaded.
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, NULL)) return 2;
    const char *names[] = {"edge_pointer_static", "edge_pointer_dynamic"};
    for (int i = 0; pointers && i < 2; ++i) {
        void *handle = load(names[i]);
        if (!handle) return 2;
        pointer_addr[i] = lookup(handle, names[i], "pointer_addr");
        expected_addr[i] = lookup(handle, names[i], "expected_addr");
        local_pointer_addr[i] = lookup(handle, names[i], "local_pointer_addr");
        local_expected_addr[i] = lookup(handle, names[i], "local_expected_addr");
        if (!pointer_addr[i] || !expected_addr[i] ||
            !local_pointer_addr[i] || !local_expected_addr[i]) return 2;
    }
    if (alignment) {
        void *handle = load("edge_aligned");
        if (!handle || !(aligned_addr = dlsym(handle, "aligned_addr"))) return 2;
    }
    if (weak) {
        void *handle = load("edge_weak");
        if (!handle || !(optional_addr = dlsym(handle, "optional_addr"))) return 2;
    }
    atomic_store_explicit(&ready, 1, memory_order_release);
    void *result;
    int failed = check();
    if (pthread_join(thread, &result) || result != NULL) failed = 1;
    // Exercise fresh and recycled stacks after late TLS registration.
    for (int wave = 0; wave < 4; ++wave) {
        if (pthread_create(&thread, NULL, worker, NULL)) return 2;
        if (pthread_join(thread, &result) || result != NULL) failed = 1;
    }
    printf("late TLS %s: %s\n", argv[1], failed ? "FAIL" : "PASS");
    return failed;
}
