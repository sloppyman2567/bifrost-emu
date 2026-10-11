// Fixed schedule-instruction vectors checked with qemu-aarch64.
// Expectations are literals, independent of the emulator's helpers.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } \
    else { printf("OK:   %s\n", msg); } \
} while(0)

// Test SHA256SU0.
static void test_sha256su0(void) {
    printf("\n--- test_sha256su0 ---\n");
    uint32_t vd_in[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t vn_in[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
    uint32_t vd_ref[4], vd_out[4];

    const uint32_t expected[4] = {0xd9999999, 0xceeeeeee, 0xc4444444, 0x39999999};
    memcpy(vd_ref, expected, 16);

    // Load vd into v0, vn into v1, run sha256su0, store back.
    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "sha256su0 v0.4s, v1.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vd_out]"r"(vd_out)
        : "v0", "v1", "memory"
    );

    char msg[128];
    snprintf(msg, sizeof(msg), "SHA256SU0 word 0: out=0x%x ref=0x%x",
             vd_out[0], vd_ref[0]);
    CHECK(vd_out[0] == vd_ref[0], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 1: out=0x%x ref=0x%x",
             vd_out[1], vd_ref[1]);
    CHECK(vd_out[1] == vd_ref[1], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 2: out=0x%x ref=0x%x",
             vd_out[2], vd_ref[2]);
    CHECK(vd_out[2] == vd_ref[2], msg);
    snprintf(msg, sizeof(msg), "SHA256SU0 word 3: out=0x%x ref=0x%x",
             vd_out[3], vd_ref[3]);
    CHECK(vd_out[3] == vd_ref[3], msg);
}

// Test SHA256SU1.
static void test_sha256su1(void) {
    printf("\n--- test_sha256su1 ---\n");
    uint32_t vd_in[4] = {0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10};
    uint32_t vn_in[4] = {0x11121314, 0x15161718, 0x191a1b1c, 0x1d1e1f20};
    uint32_t vm_in[4] = {0x21222324, 0x25262728, 0x292a2b2c, 0x2d2e2f30};
    uint32_t vd_ref[4], vd_out[4];

    const uint32_t expected[4] = {0x6711f556, 0xf0957add, 0xea406543, 0x4039953a};
    memcpy(vd_ref, expected, 16);

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha256su1 v0.4s, v1.4s, v2.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vm]"r"(vm_in),
           [vd_out]"r"(vd_out)
        : "v0", "v1", "v2", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA256SU1 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

// Test SHA1SU0.
static void test_sha1su0(void) {
    printf("\n--- test_sha1su0 ---\n");
    uint32_t vd_in[4] = {0xAABBCCDD, 0x11223344, 0x55667788, 0x99AABBCD};
    uint32_t vn_in[4] = {0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10};
    uint32_t vm_in[4] = {0xCAFEBABE, 0xDEADBEEF, 0xFEEDFACE, 0xBAADF00D};
    uint32_t vd_ref[4], vd_out[4];

    const uint32_t expected[4] = {0x352301eb, 0x56253666, 0xaa898e42, 0x26014cc8};
    memcpy(vd_ref, expected, 16);

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "ldr q2, [%[vm]]\n"
        "sha1su0 v0.4s, v1.4s, v2.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vm]"r"(vm_in),
           [vd_out]"r"(vd_out)
        : "v0", "v1", "v2", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA1SU0 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

// Test SHA1SU1 (the corrected full 4-word version).
static void test_sha1su1(void) {
    printf("\n--- test_sha1su1 ---\n");
    uint32_t vd_in[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
    uint32_t vn_in[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
    uint32_t vd_ref[4], vd_out[4];

    const uint32_t expected[4] = {0xeeeeeeee, 0xaaaaaaaa, 0x77777777, 0x55555555};
    memcpy(vd_ref, expected, 16);

    __asm__ volatile (
        "ldr q0, [%[vd]]\n"
        "ldr q1, [%[vn]]\n"
        "sha1su1 v0.4s, v1.4s\n"
        "str q0, [%[vd_out]]\n"
        :: [vd]"r"(vd_in), [vn]"r"(vn_in), [vd_out]"r"(vd_out)
        : "v0", "v1", "memory"
    );

    char msg[128];
    for (int i = 0; i < 4; i++) {
        snprintf(msg, sizeof(msg), "SHA1SU1 word %d: out=0x%x ref=0x%x",
                 i, vd_out[i], vd_ref[i]);
        CHECK(vd_out[i] == vd_ref[i], msg);
    }
}

int main(void) {
    test_sha1su0();
    test_sha1su1();
    test_sha256su0();
    test_sha256su1();

    printf("\n=== Results: %d/%d checks passed, %d failures ===\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
