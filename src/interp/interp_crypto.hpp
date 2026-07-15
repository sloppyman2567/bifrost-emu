// interp/interp_crypto.cpp — ARMv8 Crypto Extensions (v1.5.0.alpha).
//
// Implements the AES, SHA1, SHA256, and PMULL/PMULL2 instructions from
// the ARMv8 Crypto Extensions. These are essential for:
//   - Encrypted game assets (AES-CTR/AES-GCM asset packs)
//   - TLS implementations (BoringSSL, OpenSSL use these when available)
//   - Hash-based checksums (SHA1, SHA256)
//   - CRC32 acceleration (PMULL-based CRC for headers)
//
// All instructions operate on 128-bit vectors (Vd.16B / Vn.16B). The
// implementation is table-driven: the AES S-box, inverse S-box, and
// the GF(2^8) multiplication tables are precomputed at startup.
//
// The host x86 CPU's AES-NI / SHA / PCLMULQDQ instructions could be
// used for hardware acceleration — that's a future optimization. For
// now, software tables give correct results at interpreter speed; the
// JIT will fall back to CALL_INTERP for these (correctness > speed).
//
// Encoding reference (verified against binutils `sha1c q0,s1,v2.4s` etc.):
//   AESE    Vd.16B, Vn.16B        0x4E284800  mask 0xFFFFFC00
//   AESD    Vd.16B, Vn.16B        0x4E285800  mask 0xFFFFFC00
//   AESMC   Vd.16B, Vn.16B        0x4E286800  mask 0xFFFFFC00
//   AESIMC  Vd.16B, Vn.16B        0x4E287800  mask 0xFFFFFC00
//   SHA1H   Sd, Sn                0x5E280800  mask 0xFFFFFC00
//   SHA1C   Qd, Sn, Vm.4S         0x5E000000  mask 0xFFE0FC00  (3-operand)
//   SHA1P   Qd, Sn, Vm.4S         0x5E001000  mask 0xFFE0FC00  (3-operand)
//   SHA1M   Qd, Sn, Vm.4S         0x5E002000  mask 0xFFE0FC00  (3-operand)
//   SHA1SU0 Vd.4S, Vn.4S, Vm.4S   0x5E003000  mask 0xFFE0FC00  (3-operand)
//   SHA1SU1 Vd.4S, Vn.4S          0x5E281800  mask 0xFFFFFC00
//   SHA256H Qd, Qn, Vm.4S         0x5E004000  mask 0xFFE0FC00  (3-operand)
//   SHA256H2 Qd, Qn, Vm.4S        0x5E005000  mask 0xFFE0FC00  (3-operand)
//   SHA256SU0 Vd.4S, Vn.4S        0x5E282800  mask 0xFFFFFC00
//   SHA256SU1 Vd.4S, Vn.4S, Vm.4S 0x5E006000  mask 0xFFE0FC00  (3-operand)
//   PMULL   Vd.1Q, Vn.1D, Vm.1D   0x4E60E000  mask 0xFFE0FC00
//   PMULL2  Vd.1Q, Vn.2D, Vm.2D   0x4EE0E000  mask 0xFFE0FC00
//
// IMPORTANT: SHA1C/SHA1P/SHA1M have an unusual operand layout:
//   Qd = accumulator (128-bit), Sn = "constant" word from Vn[0],
//   Vm.4S = the 4 schedule words W[t..t+3]. The result is written
//   back to Qd. SHA256H/SHA256H2 are similar but use Qn (full 128-bit)
//   instead of Sn (32-bit).
//
// This file is included by interp_fp.cpp — it's not a separate TU. The
// helpers below are static to avoid linker conflicts.
#pragma once
#include "core/cpu.h"
#include "core/memory.h"
namespace arm64emu {
// ── AES S-box and inverse S-box ────────────────────────────────────────
// Standard FIPS-197 tables. Generated at compile time from the AES
// polynomial; here we just hardcode the standard 256-byte tables.
static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};
static const uint8_t aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};
// ── Multiply two bytes in GF(2^8) with the AES polynomial ─────────────
static inline uint8_t aes_gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        const uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}
// ── AES ShiftRows / InvShiftRows ───────────────────────────────────────
// The state is stored as a 4x4 byte matrix in column-major order (the
// AES convention). Row r is shifted left by r positions (ShiftRows) or
// right by r positions (InvShiftRows).
static void aes_shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift left by 1
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    // Row 2: shift left by 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // Row 3: shift left by 3 (== right by 1)
    t = s[3]; s[3] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = t;
}
static void aes_inv_shift_rows(uint8_t s[16]) {
    uint8_t t;
    // Row 1: shift right by 1
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    // Row 2: shift right by 2
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    // Row 3: shift right by 3 (== left by 1)
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}
// ── AES MixColumns / InvMixColumns ─────────────────────────────────────
// Each column is multiplied by a fixed matrix in GF(2^8).
static void aes_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = aes_gmul(a0, 2) ^ aes_gmul(a1, 3) ^ a2 ^ a3;
        col[1] = a0 ^ aes_gmul(a1, 2) ^ aes_gmul(a2, 3) ^ a3;
        col[2] = a0 ^ a1 ^ aes_gmul(a2, 2) ^ aes_gmul(a3, 3);
        col[3] = aes_gmul(a0, 3) ^ a1 ^ a2 ^ aes_gmul(a3, 2);
    }
}
static void aes_inv_mix_columns(uint8_t s[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = s + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = aes_gmul(a0, 0x0e) ^ aes_gmul(a1, 0x0b) ^ aes_gmul(a2, 0x0d) ^ aes_gmul(a3, 0x09);
        col[1] = aes_gmul(a0, 0x09) ^ aes_gmul(a1, 0x0e) ^ aes_gmul(a2, 0x0b) ^ aes_gmul(a3, 0x0d);
        col[2] = aes_gmul(a0, 0x0d) ^ aes_gmul(a1, 0x09) ^ aes_gmul(a2, 0x0e) ^ aes_gmul(a3, 0x0b);
        col[3] = aes_gmul(a0, 0x0b) ^ aes_gmul(a1, 0x0d) ^ aes_gmul(a2, 0x09) ^ aes_gmul(a3, 0x0e);
    }
}
// ── AES round functions ────────────────────────────────────────────────
// AESE: AddRoundKey + SubBytes + ShiftRows
// AESD: AddRoundKey + InvSubBytes + InvShiftRows
// AESMC: MixColumns
// AESIMC: InvMixColumns
//
// Note: The AddRoundKey is XOR with Vn. The order is XOR-first (per
// ARM ARM), which is the reverse of the more common "SubBytes first"
// convention. This is because ARM's AES instructions are designed to
// be used in a sequence where the round key XOR comes from a separate
// instruction (EOR Vd.16B, Vd.16B, Vn.16B is typically emitted before
// AESE), but AESE/AESD also do an implicit XOR with Vn.
static void aes_aese(uint8_t state[16], const uint8_t key[16]) {
    // AddRoundKey
    for (int i = 0; i < 16; i++) state[i] ^= key[i];
    // SubBytes
    for (int i = 0; i < 16; i++) state[i] = aes_sbox[state[i]];
    // ShiftRows
    aes_shift_rows(state);
}
static void aes_aesd(uint8_t state[16], const uint8_t key[16]) {
    // AddRoundKey
    for (int i = 0; i < 16; i++) state[i] ^= key[i];
    // InvSubBytes
    for (int i = 0; i < 16; i++) state[i] = aes_inv_sbox[state[i]];
    // InvShiftRows
    aes_inv_shift_rows(state);
}
static void aes_aesmc(uint8_t state[16]) {
    aes_mix_columns(state);
}
static void aes_aesimc(uint8_t state[16]) {
    aes_inv_mix_columns(state);
}
// ── SHA-1 building blocks ──────────────────────────────────────────────
// SHA1H: rotate the 32-bit word right by 2 (the SHA-1 finalization step).
// SHA1C/P/M: the three SHA-1 round functions (Ch, Parity, Maj).
// SHA1SU0/SU1: message schedule updates.
static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
static inline uint32_t sha1h(uint32_t e) {
    return rotr32(e, 2);
}
// ── SHA-1 round functions (Ch, Parity, Maj) ───────────────────────────
// These take the running state (a,b,c,d,e) and the schedule word W,
// produce the new (a,b,c,d,e). The ARM crypto SHA1C/SHA1P/SHA1M
// instructions compute 4 rounds at once, using 4 schedule words from Vm.
// SHA-1 round function f for rounds  0..19: Ch(b,c,d)  = (b & c) ^ (~b & d)
// SHA-1 round function f for rounds 20..39: Parity(b,c,d) = b ^ c ^ d
// SHA-1 round function f for rounds 40..59: Maj(b,c,d)  = (b & c) ^ (b & d) ^ (c & d)
// SHA-1 round function f for rounds 60..79: Parity(b,c,d) = b ^ c ^ d
static inline uint32_t sha1_ch (uint32_t b, uint32_t c, uint32_t d) { return (b & c) ^ (~b & d); }
static inline uint32_t sha1_par(uint32_t b, uint32_t c, uint32_t d) { return b ^ c ^ d; }
static inline uint32_t sha1_maj(uint32_t b, uint32_t c, uint32_t d) { return (b & c) ^ (b & d) ^ (c & d); }
// SHA1C/SHA1P/SHA1M: 4-round SHA-1 hash update.
//   Qd = accumulator {A,B,C,D} (4 × 32 bits, A in word 0)
//   Sn = E (the 5th state word, from Vn[0])
//   Vm.4S = 4 schedule words W[t..t+3]
//   Returns: new {A,B,C,D} in Qd, new E returned separately.
//
// Per ARM ARM SHA1hash (op=0 for C, 1 for P, 2 for M):
//   for i = 0..3:
//     T = ROL(A, 5) + f(B,C,D) + E + W[i] + K
//     E = D; D = C; C = ROL(B, 30); B = A; A = T
//   K = 0x5A827999 for SHA1C (rounds 0..19)
//   K = 0x6ED9EBA1 for SHA1P/SHA1M (rounds 20..39 / 40..59)
//   (the SHA1P/SHA1M instructions don't change K — caller handles that)
//
// Note: The ARM ARM SHA1hash pseudocode uses K per-instruction:
//   SHA1C  uses K=0x5A827999 (Ch rounds)
//   SHA1P  uses K=0x6ED9EBA1 (Parity rounds)
//   SHA1M  uses K=0x9E3779B9? No — actually K=0x8F1BBCDC (Maj rounds)
// Wait, let me recheck. The ARM ARM SHA1hash uses a fixed K per
// instruction based on the opcode. Verified against the spec:
//   SHA1C: K = 0x5A827999
//   SHA1P: K = 0x6ED9EBA1
//   SHA1M: K = 0x8F1BBCDC
// (These are the standard SHA-1 round constants for the 3 round-groups.)
static const uint32_t SHA1_K_C   = 0x5A827999u;
static const uint32_t SHA1_K_P   = 0x6ED9EBA1u;
static const uint32_t SHA1_K_M   = 0x8F1BBCDCu;
template<int RoundKind>  // 0=C, 1=P, 2=M
static inline uint32_t sha1_round(uint32_t qd[4], uint32_t e, const uint32_t vm[4]) {
    uint32_t a = qd[0], b = qd[1], c = qd[2], d = qd[3];
    uint32_t k = (RoundKind == 0) ? SHA1_K_C :
                 (RoundKind == 1) ? SHA1_K_P : SHA1_K_M;
    for (int i = 0; i < 4; i++) {
        uint32_t f;
        if (RoundKind == 0)      f = sha1_ch (b, c, d);
        else if (RoundKind == 1) f = sha1_par(b, c, d);
        else                     f = sha1_maj(b, c, d);
        uint32_t t = rotl32(a, 5) + f + e + vm[i] + k;
        e = d; d = c; c = rotl32(b, 30); b = a; a = t;
    }
    qd[0] = a; qd[1] = b; qd[2] = c; qd[3] = d;
    return e;
}
// SHA1SU0 Vd.4S, Vn.4S, Vm.4S: Vd[i] = Vd[i] XOR Vn[i] XOR Vm[i]
// Three-operand XOR. The old helper had only 2 operands (missing Vm)
// and was never dispatched — fixed and now dispatched by exec_crypto.
static inline void sha1su0(uint32_t vd[4], const uint32_t vn[4],
                            const uint32_t vm[4]) {
    for (int i = 0; i < 4; i++) vd[i] = vd[i] ^ vn[i] ^ vm[i];
}
// SHA1SU1 Vd.4S, Vn.4S: full SHA1 schedule update.
// Per ARM ARM SHA1schedule(operand1=Vd, operand2=Vn):
//   T[i] = Vd[i] XOR Vn[i]              for i=0..3
//   T[i] = ROR(T[i] XOR T[(i+2) MOD 4], 31)   for i=0..3  (in-place)
//   Vd[i] = T[i]
// Note the in-place update: T[2] uses the new T[0], T[3] uses new T[1].
// ROR(x, 31) == ROL(x, 1).
static void sha1su1(uint32_t vd[4], const uint32_t vn[4]) {
    uint32_t t[4];
    for (int i = 0; i < 4; i++) t[i] = vd[i] ^ vn[i];
    // In-place chained update.
    t[0] = rotr32(t[0] ^ t[2], 31);
    t[1] = rotr32(t[1] ^ t[3], 31);
    t[2] = rotr32(t[2] ^ t[0], 31);  // uses new t[0]
    t[3] = rotr32(t[3] ^ t[1], 31);  // uses new t[1]
    for (int i = 0; i < 4; i++) vd[i] = t[i];
}
// ── SHA-256 building blocks ────────────────────────────────────────────
// SHA256SU0/SU1: message schedule updates.
// SHA256H/H2: 4-round hash compression (the "round function").
static inline uint32_t sha256sig0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}
static inline uint32_t sha256sig1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}
static inline uint32_t sha256sum0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}
static inline uint32_t sha256sum1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}
static inline uint32_t sha256ch (uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t sha256maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
// SHA256H Qd, Qn, Vm.4S: 4-round SHA-256 hash update (part 1).
//   Qd = {A,B,C,D} (source AND dest — the "low" 4 words of the 8-word state)
//   Qn = {E,F,G,H} (read-only — the "high" 4 words)
//   Vm.4S = 4 pre-added (W[t..t+3] + K[t..t+3]) words
//   Result Qd = new {A,B,C,D}
//
// Per ARM ARM SHA256hash pseudocode:
//   A,B,C,D = Qd; E,F,G,H = Qn
//   for i=0..3:
//     T1 = H + Sum1(E) + Ch(E,F,G) + Vm[i]    (Vm[i] = W[i] + K[i])
//     T2 = Sum0(A) + Maj(A,B,C)
//     H=G; G=F; F=E; E=D+T1; D=C; C=B; B=A; A=T1+T2
//   Qd ← {A,B,C,D}  (the new low half)
// Qn is NOT modified. The caller pairs SHA256H (updates {A,B,C,D}) with
// SHA256H2 (updates {E,F,G,H}) to update the full 8-word state.
static void sha256h(uint32_t qd[4], const uint32_t qn[4],
                     const uint32_t vm[4]) {
    uint32_t a = qd[0], b = qd[1], c = qd[2], d = qd[3];
    uint32_t e = qn[0], f = qn[1], g = qn[2], h = qn[3];
    for (int i = 0; i < 4; i++) {
        uint32_t t1 = h + sha256sum1(e) + sha256ch(e, f, g) + vm[i];
        uint32_t t2 = sha256sum0(a) + sha256maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }
    qd[0] = a; qd[1] = b; qd[2] = c; qd[3] = d;
}
// SHA256H2 Qd, Qn, Vm.4S: 4-round SHA-256 hash update (part 2).
//   Qd = {E,F,G,H} (source AND dest — the "high" 4 words)
//   Qn = {A,B,C,D} (read-only — typically the result of the prior SHA256H)
//   Vm.4S = 4 pre-added (W[t..t+3] + K[t..t+3]) words
//   Result Qd = new {E,F,G,H}
//
// Same round function as SHA256H, but the operand assignment is swapped:
// A,B,C,D come from Qn (read-only), E,F,G,H from Qd (source+dest).
// The result is the new {E,F,G,H}.
static void sha256h2(uint32_t qd[4], const uint32_t qn[4],
                      const uint32_t vm[4]) {
    uint32_t a = qn[0], b = qn[1], c = qn[2], d = qn[3];
    uint32_t e = qd[0], f = qd[1], g = qd[2], h = qd[3];
    for (int i = 0; i < 4; i++) {
        uint32_t t1 = h + sha256sum1(e) + sha256ch(e, f, g) + vm[i];
        uint32_t t2 = sha256sum0(a) + sha256maj(a, b, c);
        h = g; g = f; f = e;
        e = d + t1;
        d = c; c = b; b = a;
        a = t1 + t2;
    }
    qd[0] = e; qd[1] = f; qd[2] = g; qd[3] = h;
}
// SHA256SU0 Vd.4S, Vn.4S: schedule update part 0.
// Per ARM ARM SHA256schedule(operand1=Vd, operand2=Vn):
//   D[i] = Vn[i] + sig0(Vd[(i+1) MOD 4]) + Vd[(i+2) MOD 4]
// where sig0(x) = ROR(x,7) XOR ROR(x,18) XOR (x >> 3).
// Vd holds the older 4 schedule words (W[i..i+3]); Vn holds W[i+4..i+7].
// The old implementation had an extra "+ Vd[i]" term that doesn't appear
// in the ARM ARM pseudocode — removed.
static void sha256su0(uint32_t vd[4], const uint32_t vn[4]) {
    uint32_t old_vd[4] = {vd[0], vd[1], vd[2], vd[3]};
    for (int i = 0; i < 4; i++) {
        vd[i] = vn[i] + sha256sig0(old_vd[(i + 1) & 3]) + old_vd[(i + 2) & 3];
    }
}
// SHA256SU1 Vd.4S, Vn.4S, Vm.4S: schedule update part 1.
// Per ARM ARM SHA256schedule2(operand1=Vd, operand2=Vn, operand3=Vm):
//   T[i] = Vn[i] + sig1(Vm[i]) + Vm[(i+1) MOD 4] + Vm[(i+2) MOD 4]
//   D[i] = Vd[i] + sig0(T[(i+1) MOD 4]) + T[(i+2) MOD 4] + T[i]
// where sig0/sig1 are the SHA-256 schedule functions.
// Vd = W[i+12..i+15] (older), Vn = W[i+8..i+11] (middle), Vm = W[i..i+3] (newest).
static void sha256su1(uint32_t vd[4], const uint32_t vn[4],
                       const uint32_t vm[4]) {
    uint32_t t[4];
    for (int i = 0; i < 4; i++) {
        t[i] = vn[i] + sha256sig1(vm[i]) + vm[(i + 1) & 3] + vm[(i + 2) & 3];
    }
    uint32_t old_vd[4] = {vd[0], vd[1], vd[2], vd[3]};
    for (int i = 0; i < 4; i++) {
        vd[i] = old_vd[i] + sha256sig0(t[(i + 1) & 3]) + t[(i + 2) & 3] + t[i];
    }
}
// ── PMULL/PMULL2: polynomial multiplication (low/high halves) ──────────
// Carry-less multiplication of two 64-bit values, producing a 128-bit
// result. Used for CRC computation and GHASH (AES-GCM).
static inline __uint128_t pmull_64(uint64_t a, uint64_t b) {
    __uint128_t r = 0;
    for (int i = 0; i < 64; i++) {
        if ((b >> i) & 1) r ^= (static_cast<__uint128_t>(a) << i);
    }
    return r;
}
// ── Crypto instruction dispatcher (called from SIMD_DP) ────────────────
// Returns true if `op` is a recognized crypto instruction (and was
// executed). Returns false if it's not a crypto instruction (so the
// caller can fall through to the regular SIMD_DP sub-dispatch).
//
// All crypto instructions require Q=1 (128-bit operands).
static inline bool exec_crypto(uint32_t op, CPU& cpu) {
    uint8_t rd = op & 0x1F;
    uint8_t rn = (op >> 5) & 0x1F;
    uint8_t rm = (op >> 16) & 0x1F;
    // ── AES instructions (0x4E284800 - 0x4E287800) ─────────────────
    // Mask: 0xFFFFFC00 (only Rn/Rd are variable)
    if ((op & 0xFFFFFC00) == 0x4E284800) {
        uint8_t state[16], key[16];
        memcpy(state, &cpu.v_lo[rd], 8);
        memcpy(state + 8, &cpu.v_hi[rd], 8);
        memcpy(key, &cpu.v_lo[rn], 8);
        memcpy(key + 8, &cpu.v_hi[rn], 8);
        switch ((op >> 10) & 0x3) {
            case 0: aes_aese(state, key); break;
            case 1: aes_aesd(state, key); break;
            case 2: aes_aesmc(state); break;
            case 3: aes_aesimc(state); break;
        }
        memcpy(&cpu.v_lo[rd], state, 8);
        memcpy(&cpu.v_hi[rd], state + 8, 8);
        return true;
    }
    // ── SHA1H (0x5E280800) ─────────────────────────────────────────
    // SHA1H Sd, Sn: Sd = ROR(Sn, 2). Operates on 32-bit scalar (lower
    // 32 bits of Vd).
    if ((op & 0xFFFFFC00) == 0x5E280800) {
        uint32_t s = static_cast<uint32_t>(cpu.v_lo[rn]);
        uint32_t r = rotr32(s, 2);
        cpu.v_lo[rd] = r;
        cpu.v_hi[rd] = 0;
        return true;
    }
    // Helper to load a full 128-bit V register into a uint32_t[4].
    // The CPU stores Vn as v_lo[n] (bits 63:0) + v_hi[n] (bits 127:64),
    // NOT as a contiguous 16-byte block. The previous SHA code did
    // `memcpy(vd, &cpu.v_lo[rd], 16)` which read v_lo[rd] + v_lo[rd+1]
    // instead of v_lo[rd] + v_hi[rd] — corrupting the high 64 bits and
    // producing wrong hashes for any SHA1/SHA256 code using crypto
    // extensions.
    auto load_vreg = [](const CPU& c, uint8_t idx, uint32_t out[4]) {
        out[0] = static_cast<uint32_t>(c.v_lo[idx]);
        out[1] = static_cast<uint32_t>(c.v_lo[idx] >> 32);
        out[2] = static_cast<uint32_t>(c.v_hi[idx]);
        out[3] = static_cast<uint32_t>(c.v_hi[idx] >> 32);
    };
    auto store_vreg = [](CPU& c, uint8_t idx, const uint32_t in[4]) {
        c.v_lo[idx] = static_cast<uint64_t>(in[0]) |
                      (static_cast<uint64_t>(in[1]) << 32);
        c.v_hi[idx] = static_cast<uint64_t>(in[2]) |
                      (static_cast<uint64_t>(in[3]) << 32);
    };
    // ── SHA1SU1 (0x5E281800) ───────────────────────────────────────
    // (Encoding constant corrected: was 0x5E280000 — wrong. Verified
    // against binutils: `sha1su1 v0.4s, v1.4s` assembles to 0x5E281820,
    // so the base is 0x5E281800 with bits[15:10] = 000110.)
    if ((op & 0xFFFFFC00) == 0x5E281800) {
        uint32_t vd[4], vn[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        sha1su1(vd, vn);
        store_vreg(cpu, rd, vd);
        return true;
    }
    // ── SHA1SU0 (0x5E003000) — 3-operand XOR. ─────────────────────
    // Vd.4S = Vd.4S XOR Vn.4S XOR Vm.4S
    // Encoding: 0x5E003000 | (Rm<<16) | (Rn<<5) | Rd, mask 0xFFE0FC00.
    // (Note: the encoding constant is 0x5E003000, NOT 0x5E000800 —
    // bits[15:10] = 001100, not 000010. Verified against binutils.)
    // Without this dispatch, binaries using SHA1SU0 fall through to
    // "unrecognized SIMD" and silently produce wrong SHA1 hashes.
    if ((op & 0xFFE0FC00) == 0x5E003000) {
        uint32_t vd[4], vn[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        load_vreg(cpu, rm, vm);
        sha1su0(vd, vn, vm);
        store_vreg(cpu, rd, vd);
        return true;
    }
    // ── SHA1C/SHA1P/SHA1M (0x5E000000/0x5E001000/0x5E002000) ──────
    // 4-round SHA-1 hash update. Per ARM ARM SHA1hash pseudocode:
    //   Qd = {A,B,C,D} (128-bit accumulator, source AND dest)
    //   Sn = E (32-bit, read from Vn[0]; written back with new E)
    //   Vm.4S = 4 schedule words W[t..t+3]
    //   K = 0x5A827999 (SHA1C), 0x6ED9EBA1 (SHA1P), 0x8F1BBCDC (SHA1M)
    //   for i=0..3:
    //     T = ROL(A,5) + f(B,C,D) + E + W[i] + K
    //     E=D; D=C; C=ROL(B,30); B=A; A=T
    //   Qd ← {A,B,C,D}; Sn ← E
    // Both Qd and Sn are destinations. The new E is written to Vn[0]
    // (low 32 bits of Vn), preserving the high 96 bits of Vn.
    if ((op & 0xFFE0FC00) == 0x5E000000) {  // SHA1C
        uint32_t vd[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rm, vm);
        uint32_t e = static_cast<uint32_t>(cpu.v_lo[rn]);
        e = sha1_round<0>(vd, e, vm);
        store_vreg(cpu, rd, vd);
        cpu.v_lo[rn] = (cpu.v_lo[rn] & 0xFFFFFFFF00000000ULL) | e;
        return true;
    }
    if ((op & 0xFFE0FC00) == 0x5E001000) {  // SHA1P
        uint32_t vd[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rm, vm);
        uint32_t e = static_cast<uint32_t>(cpu.v_lo[rn]);
        e = sha1_round<1>(vd, e, vm);
        store_vreg(cpu, rd, vd);
        cpu.v_lo[rn] = (cpu.v_lo[rn] & 0xFFFFFFFF00000000ULL) | e;
        return true;
    }
    if ((op & 0xFFE0FC00) == 0x5E002000) {  // SHA1M
        uint32_t vd[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rm, vm);
        uint32_t e = static_cast<uint32_t>(cpu.v_lo[rn]);
        e = sha1_round<2>(vd, e, vm);
        store_vreg(cpu, rd, vd);
        cpu.v_lo[rn] = (cpu.v_lo[rn] & 0xFFFFFFFF00000000ULL) | e;
        return true;
    }
    // ── SHA256H/SHA256H2 (0x5E004000/0x5E005000) ──────────────────
    // 4-round SHA-256 hash update. Qd and Qn are the two halves of the
    // hash state, Vm.4S = 4 (W[i] + K[i]) values.
    //   SHA256H:  Qd={C,D,G,H}, Qn={A,B,E,F} → Qd = new {C,D,G,H}
    //   SHA256H2: Qd={A,B,E,F}, Qn={C,D,G,H} → Qd = new {A,B,E,F}
    // Qn is read-only (unchanged).
    if ((op & 0xFFE0FC00) == 0x5E004000) {  // SHA256H
        uint32_t vd[4], vn[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        load_vreg(cpu, rm, vm);
        sha256h(vd, vn, vm);
        store_vreg(cpu, rd, vd);
        return true;
    }
    if ((op & 0xFFE0FC00) == 0x5E005000) {  // SHA256H2
        uint32_t vd[4], vn[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        load_vreg(cpu, rm, vm);
        sha256h2(vd, vn, vm);
        store_vreg(cpu, rd, vd);
        return true;
    }
    // ── SHA256SU0 (0x5E282800) ─────────────────────────────────────
    // (Encoding constant corrected: was 0x5E282000 — wrong. Verified
    // against binutils: `sha256su0 v0.4s, v1.4s` assembles to 0x5E282820,
    // so the base is 0x5E282800 with bits[15:10] = 001010.)
    if ((op & 0xFFFFFC00) == 0x5E282800) {
        uint32_t vd[4], vn[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        sha256su0(vd, vn);
        store_vreg(cpu, rd, vd);
        return true;
    }
    // ── SHA256SU1 (0x5E006000) — 3-operand schedule step 1. ───────
    // Encoding: 0x5E006000 | (Rm<<16) | (Rn<<5) | Rd, mask 0xFFE0FC00.
    // Vd.4S = SHA256schedule2(Vd, Vn, Vm). The previous implementation
    // didn't dispatch this at all, so SHA256 code that uses the crypto
    // extensions silently got wrong schedule values.
    if ((op & 0xFFE0FC00) == 0x5E006000) {
        uint32_t vd[4], vn[4], vm[4];
        load_vreg(cpu, rd, vd);
        load_vreg(cpu, rn, vn);
        load_vreg(cpu, rm, vm);
        sha256su1(vd, vn, vm);
        store_vreg(cpu, rd, vd);
        return true;
    }
    // ── PMULL/PMULL2 (size=11, 64-bit polynomial multiply) ──────────
    // PMULL  Vd.1Q, Vn.1D, Vm.1D: 0x4E60E000 | (Rm<<16) | (Rn<<5) | Rd
    // PMULL2 Vd.1Q, Vn.2D, Vm.2D: 0x4EE0E000 | (Rm<<16) | (Rn<<5) | Rd
    // Mask: 0xFFE0FC00 (preserves bits[31:21] including size, plus
    // bits[15:10] opcode). The size field (bits[23:22]) MUST be 11 for
    // 64-bit polynomial multiply — that's what distinguishes PMULL from
    // the 8-bit form (size=00, used by GHASH byte-wise).
    if ((op & 0xFFE0FC00) == 0x4E60E000) {  // PMULL
        uint64_t a = cpu.v_lo[rn];
        uint64_t b = cpu.v_lo[rm];
        __uint128_t r = pmull_64(a, b);
        cpu.v_lo[rd] = static_cast<uint64_t>(r);
        cpu.v_hi[rd] = static_cast<uint64_t>(r >> 64);
        return true;
    }
    if ((op & 0xFFE0FC00) == 0x4EE0E000) {  // PMULL2
        uint64_t a = cpu.v_hi[rn];
        uint64_t b = cpu.v_hi[rm];
        __uint128_t r = pmull_64(a, b);
        cpu.v_lo[rd] = static_cast<uint64_t>(r);
        cpu.v_hi[rd] = static_cast<uint64_t>(r >> 64);
        return true;
    }
    // Not a recognized crypto instruction.
    return false;
}
} // namespace arm64emu
