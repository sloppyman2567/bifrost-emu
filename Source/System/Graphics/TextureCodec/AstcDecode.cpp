// AstcDecode.cpp — see AstcDecode.hpp. Written from the Khronos Data Format Specification's ASTC chapter; section
// names in comments are that chapter's.
#include "System/Graphics/TextureCodec/AstcDecode.hpp"
#include <algorithm>
#include <cstring>

namespace arm64emu::texcodec {
namespace {

using u128 = unsigned __int128;

// "Integer Sequence Encoding": every range is bits, or a trit/quint plus bits. Ascending by the largest value.
struct Range { uint8_t bits, trits, quints, max; };
constexpr Range kRanges[] = {
    {1, 0, 0, 1},   {0, 1, 0, 2},   {2, 0, 0, 3},   {0, 0, 1, 4},   {1, 1, 0, 5},   {3, 0, 0, 7},   {1, 0, 1, 9},
    {2, 1, 0, 11},  {4, 0, 0, 15},  {2, 0, 1, 19},  {3, 1, 0, 23},  {5, 0, 0, 31},  {3, 0, 1, 39},  {4, 1, 0, 47},
    {6, 0, 0, 63},  {4, 0, 1, 79},  {5, 1, 0, 95},  {7, 0, 0, 127}, {5, 0, 1, 159}, {6, 1, 0, 191}, {8, 0, 0, 255},
};
constexpr int kRangeCount = sizeof(kRanges) / sizeof(kRanges[0]);
constexpr int kMinColorRange = 4;  // 0..5, the smallest range an endpoint may use

int ise_bits(int count, const Range& r) {
    return count * r.bits + (r.trits ? (8 * count + 4) / 5 : 0) + (r.quints ? (7 * count + 2) / 3 : 0);
}

// Bits past the end of a sequence read as zero ("Integer Sequence Encoding"); neighbouring fields must not leak in.
inline u128 keep_bits(const u128& v, int n) { return n >= 128 ? v : v & ((static_cast<u128>(1) << n) - 1); }

inline uint32_t get_bits(const u128& v, int pos, int n) {
    if (pos >= 128 || n == 0) return 0;
    return static_cast<uint32_t>((v >> pos) & ((1u << n) - 1u));
}

void decode_trits(uint32_t T, uint8_t t[5]) {
    uint32_t C, t4, t3;
    if (((T >> 2) & 7) == 7) {
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t4 = t3 = 2;
    } else {
        C = T & 0x1F;
        if (((T >> 5) & 3) == 3) { t4 = 2; t3 = (T >> 7) & 1; }
        else { t4 = (T >> 7) & 1; t3 = (T >> 5) & 3; }
    }
    uint32_t t2, t1, t0;
    if ((C & 3) == 3) {
        t2 = 2; t1 = (C >> 4) & 1;
        t0 = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~(C >> 3) & 1);
    } else if (((C >> 2) & 3) == 3) {
        t2 = 2; t1 = 2; t0 = C & 3;
    } else {
        t2 = (C >> 4) & 1; t1 = (C >> 2) & 3;
        t0 = (((C >> 1) & 1) << 1) | ((C & 1) & ~(C >> 1) & 1);
    }
    t[0] = t0; t[1] = t1; t[2] = t2; t[3] = t3; t[4] = t4;
}

void decode_quints(uint32_t Q, uint8_t q[3]) {
    uint32_t q2, q1, q0, C;
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        q2 = ((Q & 1) << 2) | ((((Q >> 4) & 1) & ~Q & 1) << 1) | (((Q >> 3) & 1) & ~Q & 1);
        q1 = q0 = 4;
    } else {
        if (((Q >> 1) & 3) == 3) {
            q2 = 4;
            C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
        } else {
            q2 = (Q >> 5) & 3;
            C = Q & 0x1F;
        }
        if ((C & 7) == 5) { q1 = 4; q0 = (C >> 3) & 3; }
        else { q1 = (C >> 3) & 3; q0 = C & 7; }
    }
    q[0] = q0; q[1] = q1; q[2] = q2;
}

// Decodes `count` values of range `r` from bit 0 of `bits`; a value is trit-or-quint * 2^bits + the low bits.
void ise_decode(const u128& bits, int count, const Range& r, uint8_t* out) {
    const int n = r.bits;
    if (!r.trits && !r.quints) {
        for (int i = 0; i < count; i++) out[i] = static_cast<uint8_t>(get_bits(bits, i * n, n));
        return;
    }
    if (r.trits) {
        for (int base = 0, block = 0; base < count; base += 5, block++) {
            const int p = block * (8 + 5 * n);
            const uint32_t m[5] = {get_bits(bits, p, n), get_bits(bits, p + n + 2, n), get_bits(bits, p + 2 * n + 4, n),
                                   get_bits(bits, p + 3 * n + 5, n), get_bits(bits, p + 4 * n + 7, n)};
            const uint32_t T = get_bits(bits, p + n, 2) | (get_bits(bits, p + 2 * n + 2, 2) << 2) |
                               (get_bits(bits, p + 3 * n + 4, 1) << 4) | (get_bits(bits, p + 4 * n + 5, 2) << 5) |
                               (get_bits(bits, p + 5 * n + 7, 1) << 7);
            uint8_t t[5];
            decode_trits(T, t);
            for (int j = 0; j < 5 && base + j < count; j++) out[base + j] = static_cast<uint8_t>((t[j] << n) | m[j]);
        }
    } else {
        for (int base = 0, block = 0; base < count; base += 3, block++) {
            const int p = block * (7 + 3 * n);
            const uint32_t m[3] = {get_bits(bits, p, n), get_bits(bits, p + n + 3, n), get_bits(bits, p + 2 * n + 5, n)};
            const uint32_t Q = get_bits(bits, p + n, 3) | (get_bits(bits, p + 2 * n + 3, 2) << 3) |
                               (get_bits(bits, p + 3 * n + 5, 2) << 5);
            uint8_t q[3];
            decode_quints(Q, q);
            for (int j = 0; j < 3 && base + j < count; j++) out[base + j] = static_cast<uint8_t>((q[j] << n) | m[j]);
        }
    }
}

// The B pattern of an unquantization table is written MSB first with letters for bits of the low part
// ('a' = bit 0) and '0' for zero, as the specification's tables print it.
int pattern_value(const char* pattern, uint32_t m) {
    int v = 0;
    for (const char* c = pattern; *c; c++) v = (v << 1) | (*c == '0' ? 0 : static_cast<int>((m >> (*c - 'a')) & 1));
    return v;
}

struct Tables {
    uint8_t color[kRangeCount][256];   // "Endpoint Unquantization": 0..255
    uint8_t weight[kRangeCount][32];   // "Weight Unquantization": 0..64
    Tables() {
        std::memset(color, 0, sizeof(color));
        std::memset(weight, 0, sizeof(weight));
        struct Q { const char* b; int c; };
        // Endpoint values: B pattern (9 bits) and C per trit/quint range.
        const Q colorQ[kRangeCount] = {
            {}, {}, {}, {},
            {"000000000", 204}, {}, {"000000000", 113}, {"b000b0bb0", 93}, {}, {"b0000bb00", 54}, {"cb000cbcb", 44}, {},
            {"cb0000cbc", 26}, {"dcb000dcb", 22}, {}, {"dcb0000dc", 13}, {"edcb000ed", 11}, {}, {"edcb0000e", 6},
            {"fedcb000f", 5}, {},
        };
        const Q weightQ[kRangeCount] = {
            {}, {}, {}, {}, {"0000000", 50}, {}, {"0000000", 28}, {"b000b0b", 23}, {}, {"b0000b0", 13}, {"cb000cb", 11},
            {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
        };
        for (int r = 0; r < kRangeCount; r++) {
            const Range& rg = kRanges[r];
            const int levels = rg.max + 1;
            for (int v = 0; v < levels && v < 256; v++) {
                const uint32_t m = v & ((1u << rg.bits) - 1u);
                const int tq = v >> rg.bits;  // trit or quint value
                if (r >= kMinColorRange) {
                    if (!rg.trits && !rg.quints) {
                        // Bit replication from the most significant bit.
                        int x = 0;
                        for (int shift = 8 - rg.bits;; shift -= rg.bits) {
                            x |= shift >= 0 ? static_cast<int>(m) << shift : static_cast<int>(m) >> -shift;
                            if (shift <= 0) break;
                        }
                        color[r][v] = static_cast<uint8_t>(x);
                    } else {
                        const int A = (m & 1) ? 0x1FF : 0;
                        int unq = tq * colorQ[r].c + pattern_value(colorQ[r].b, m);
                        unq ^= A;
                        color[r][v] = static_cast<uint8_t>((A & 0x80) | (unq >> 2));
                    }
                }
                if (levels <= 32) {
                    int unq;
                    static const int kTrit0[] = {0, 32, 63}, kQuint0[] = {0, 16, 32, 47, 63};
                    if (r == 1) unq = kTrit0[v];
                    else if (r == 3) unq = kQuint0[v];
                    else if (!rg.trits && !rg.quints) {
                        switch (rg.bits) {
                            case 1: unq = v ? 63 : 0; break;
                            case 2: unq = v * 21; break;
                            case 3: unq = (v << 3) | v; break;
                            case 4: unq = (v << 2) | (v >> 2); break;
                            default: unq = (v << 1) | (v >> 4); break;
                        }
                    } else {
                        const int A = (m & 1) ? 0x7F : 0;
                        unq = tq * weightQ[r].c + pattern_value(weightQ[r].b, m);
                        unq ^= A;
                        unq = (A & 0x20) | (unq >> 2);
                    }
                    if (unq > 32) unq++;
                    weight[r][v] = static_cast<uint8_t>(unq);
                }
            }
        }
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

// "Partition Pattern Generation".
uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6;  p ^= p >> 17;
    return p;
}

int select_partition(int seed, int x, int y, int partitions, bool small_block) {
    if (small_block) { x <<= 1; y <<= 1; }
    seed += (partitions - 1) * 1024;
    const uint32_t rnum = hash52(static_cast<uint32_t>(seed));
    uint8_t s[12] = {
        static_cast<uint8_t>(rnum & 0xF),         static_cast<uint8_t>((rnum >> 4) & 0xF),
        static_cast<uint8_t>((rnum >> 8) & 0xF),  static_cast<uint8_t>((rnum >> 12) & 0xF),
        static_cast<uint8_t>((rnum >> 16) & 0xF), static_cast<uint8_t>((rnum >> 20) & 0xF),
        static_cast<uint8_t>((rnum >> 24) & 0xF), static_cast<uint8_t>((rnum >> 28) & 0xF),
        static_cast<uint8_t>((rnum >> 18) & 0xF), static_cast<uint8_t>((rnum >> 22) & 0xF),
        static_cast<uint8_t>((rnum >> 26) & 0xF), static_cast<uint8_t>(((rnum >> 30) | (rnum << 2)) & 0xF)};
    for (uint8_t& v : s) v = static_cast<uint8_t>(v * v);
    int sh1, sh2;
    if (seed & 1) {
        sh1 = (seed & 2) ? 4 : 5;
        sh2 = partitions == 3 ? 6 : 5;
    } else {
        sh1 = partitions == 3 ? 6 : 5;
        sh2 = (seed & 2) ? 4 : 5;
    }
    const int sh3 = (seed & 0x10) ? sh1 : sh2;
    s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2;
    s[4] >>= sh1; s[5] >>= sh2; s[6] >>= sh1; s[7] >>= sh2;
    s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;
    int a = s[0] * x + s[1] * y + (rnum >> 14);
    int b = s[2] * x + s[3] * y + (rnum >> 10);
    int c = s[4] * x + s[5] * y + (rnum >> 6);
    int d = s[6] * x + s[7] * y + (rnum >> 2);
    a &= 0x3F; b &= 0x3F; c &= 0x3F; d &= 0x3F;
    if (partitions < 4) d = 0;
    if (partitions < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

u128 reverse_bits128(uint64_t lo, uint64_t hi) {
    auto rev64 = [](uint64_t v) {
        v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
        v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
        v = ((v >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((v & 0x0F0F0F0F0F0F0F0Full) << 4);
        return __builtin_bswap64(v);
    };
    return (static_cast<u128>(rev64(lo)) << 64) | rev64(hi);
}

struct BlockMode {
    bool valid = false;
    int width = 0, height = 0;  // weight grid
    bool dual = false;
    int range = 0;              // index into kRanges
};

// "Block Mode", 2D layout.
BlockMode decode_block_mode(uint32_t b) {
    BlockMode m;
    int layout_w, layout_h;
    uint32_t rho;
    bool dual = (b >> 10) & 1, high = (b >> 9) & 1;
    if ((b & 3) != 0) {
        rho = ((b >> 4) & 1) | ((b & 1) << 1) | (((b >> 1) & 1) << 2);
        const uint32_t a = (b >> 5) & 3, bb = (b >> 7) & 3;
        switch ((b >> 2) & 3) {
            case 0: layout_w = bb + 4; layout_h = a + 2; break;
            case 1: layout_w = bb + 8; layout_h = a + 2; break;
            case 2: layout_w = a + 2;  layout_h = bb + 8; break;
            default:
                if (b & 0x100) { layout_w = ((b >> 7) & 1) + 2; layout_h = a + 2; }
                else           { layout_w = a + 2;              layout_h = ((b >> 7) & 1) + 6; }
                break;
        }
    } else {
        rho = ((b >> 4) & 1) | (((b >> 2) & 1) << 1) | (((b >> 3) & 1) << 2);
        if (((b >> 2) & 3) == 0) return m;  // bits 3..0 all zero: reserved
        const uint32_t sel = (b >> 7) & 3, a = (b >> 5) & 3;
        switch (sel) {
            case 0: layout_w = 12; layout_h = a + 2; break;
            case 1: layout_w = a + 2; layout_h = 12; break;
            case 2:
                layout_w = a + 6; layout_h = ((b >> 9) & 3) + 6;
                dual = false; high = false;
                break;
            default:
                if (a == 0) { layout_w = 6; layout_h = 10; }
                else if (a == 1) { layout_w = 10; layout_h = 6; }
                else return m;  // void extent or reserved: the caller has handled the former
                break;
        }
    }
    if (rho < 2) return m;
    m.valid = true;
    m.width = layout_w;
    m.height = layout_h;
    m.dual = dual;
    // Low precision: 0..1, 0..2, 0..3, 0..4, 0..5, 0..7; high precision: 0..9, 0..11, 0..15, 0..19, 0..23, 0..31.
    static const int kLow[] = {0, 1, 2, 3, 4, 5};
    static const int kHigh[] = {6, 7, 8, 9, 10, 11};
    m.range = high ? kHigh[rho - 2] : kLow[rho - 2];
    return m;
}

inline int clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void bit_transfer_signed(int& a, int& b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}

struct Rgba { int r, g, b, a; };
Rgba blue_contract(int r, int g, int b, int a) { return {(r + b) >> 1, (g + b) >> 1, b, a}; }

// "LDR Endpoint Decoding"; false for the HDR modes.
bool decode_endpoints(int cem, const int* v, Rgba& e0, Rgba& e1) {
    switch (cem) {
        case 0: e0 = {v[0], v[0], v[0], 255}; e1 = {v[1], v[1], v[1], 255}; return true;
        case 1: {
            const int L0 = (v[0] >> 2) | (v[1] & 0xC0);
            const int L1 = std::min(L0 + (v[1] & 0x3F), 255);
            e0 = {L0, L0, L0, 255}; e1 = {L1, L1, L1, 255};
            return true;
        }
        case 4: e0 = {v[0], v[0], v[0], v[2]}; e1 = {v[1], v[1], v[1], v[3]}; return true;
        case 5: {
            int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            e0 = {v0, v0, v0, v2};
            e1 = {clamp8(v0 + v1), clamp8(v0 + v1), clamp8(v0 + v1), clamp8(v2 + v3)};
            e0 = {clamp8(e0.r), clamp8(e0.g), clamp8(e0.b), clamp8(e0.a)};
            return true;
        }
        case 6:
            e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255};
            e1 = {v[0], v[1], v[2], 255};
            return true;
        case 8: {
            const int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) { e0 = {v[0], v[2], v[4], 255}; e1 = {v[1], v[3], v[5], 255}; }
            else { e0 = blue_contract(v[1], v[3], v[5], 255); e1 = blue_contract(v[0], v[2], v[4], 255); }
            return true;
        }
        case 9: {
            int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3], v4 = v[4], v5 = v[5];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            bit_transfer_signed(v5, v4);
            if (v1 + v3 + v5 >= 0) {
                e0 = {v0, v2, v4, 255};
                e1 = {v0 + v1, v2 + v3, v4 + v5, 255};
            } else {
                e0 = blue_contract(v0 + v1, v2 + v3, v4 + v5, 255);
                e1 = blue_contract(v0, v2, v4, 255);
            }
            e0 = {clamp8(e0.r), clamp8(e0.g), clamp8(e0.b), clamp8(e0.a)};
            e1 = {clamp8(e1.r), clamp8(e1.g), clamp8(e1.b), clamp8(e1.a)};
            return true;
        }
        case 10:
            e0 = {(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]};
            e1 = {v[0], v[1], v[2], v[5]};
            return true;
        case 12: {
            const int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) { e0 = {v[0], v[2], v[4], v[6]}; e1 = {v[1], v[3], v[5], v[7]}; }
            else { e0 = blue_contract(v[1], v[3], v[5], v[7]); e1 = blue_contract(v[0], v[2], v[4], v[6]); }
            return true;
        }
        case 13: {
            int v0 = v[0], v1 = v[1], v2 = v[2], v3 = v[3], v4 = v[4], v5 = v[5], v6 = v[6], v7 = v[7];
            bit_transfer_signed(v1, v0);
            bit_transfer_signed(v3, v2);
            bit_transfer_signed(v5, v4);
            bit_transfer_signed(v7, v6);
            if (v1 + v3 + v5 >= 0) {
                e0 = {v0, v2, v4, v6};
                e1 = {v0 + v1, v2 + v3, v4 + v5, v6 + v7};
            } else {
                e0 = blue_contract(v0 + v1, v2 + v3, v4 + v5, v6 + v7);
                e1 = blue_contract(v0, v2, v4, v6);
            }
            e0 = {clamp8(e0.r), clamp8(e0.g), clamp8(e0.b), clamp8(e0.a)};
            e1 = {clamp8(e1.r), clamp8(e1.g), clamp8(e1.b), clamp8(e1.a)};
            return true;
        }
        default: return false;  // 2, 3, 7, 11, 14, 15: HDR
    }
}

void fill_error(uint8_t* rgba, int count) {
    for (int i = 0; i < count; i++) { rgba[i * 4] = 255; rgba[i * 4 + 1] = 0; rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = 255; }
}

}  // namespace

bool astc_footprint_valid(int w, int h) {
    static const int kFootprints[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                         {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (const auto& f : kFootprints)
        if (f[0] == w && f[1] == h) return true;
    return false;
}

bool astc_decode_block(const uint8_t* block, int bw, int bh, bool srgb, uint8_t* rgba) {
    const int texels = bw * bh;
    uint64_t lo, hi;
    std::memcpy(&lo, block, 8);
    std::memcpy(&hi, block + 8, 8);
    const u128 blk = (static_cast<u128>(hi) << 64) | lo;
    const uint32_t mode_bits = static_cast<uint32_t>(lo & 0x7FF);

    // "Void-Extent Blocks": one colour, stored as UNORM16.
    if ((lo & 0x1FF) == 0x1FC) {
        const bool hdr = (lo >> 9) & 1;
        const bool reserved_ok = ((lo >> 10) & 3) == 3;
        const uint32_t smin = (lo >> 12) & 0x1FFF, smax = (lo >> 25) & 0x1FFF;
        const uint32_t tmin = (lo >> 38) & 0x1FFF, tmax = ((lo >> 51) & 0x1FFF);
        const bool unbounded = smin == 0x1FFF && smax == 0x1FFF && tmin == 0x1FFF && tmax == 0x1FFF;
        if (hdr || !reserved_ok || (!unbounded && (smin >= smax || tmin >= tmax))) {
            fill_error(rgba, texels);
            return false;
        }
        const uint8_t px[4] = {static_cast<uint8_t>(hi >> 8), static_cast<uint8_t>(hi >> 24),
                               static_cast<uint8_t>(hi >> 40), static_cast<uint8_t>(hi >> 56)};
        for (int i = 0; i < texels; i++) std::memcpy(rgba + i * 4, px, 4);
        return true;
    }

    const BlockMode bm = decode_block_mode(mode_bits);
    const int partitions = static_cast<int>((lo >> 11) & 3) + 1;
    const int num_weights = bm.width * bm.height * (bm.dual ? 2 : 1);
    if (!bm.valid || bm.width > bw || bm.height > bh || (bm.dual && partitions == 4) || num_weights > 64) {
        fill_error(rgba, texels);
        return false;
    }
    const Range& wr = kRanges[bm.range];
    const int weight_bits = ise_bits(num_weights, wr);
    if (weight_bits < 24 || weight_bits > 96) { fill_error(rgba, texels); return false; }

    // Colour endpoint modes.
    int cem[4] = {0, 0, 0, 0};
    int extra_cem_bits = 0;
    int config_bits = 17;
    if (partitions == 1) {
        cem[0] = static_cast<int>((lo >> 13) & 0xF);
    } else {
        const uint32_t selector = static_cast<uint32_t>((lo >> 23) & 3);
        if (selector == 0) {
            const int c = static_cast<int>((lo >> 25) & 0xF);
            for (int i = 0; i < partitions; i++) cem[i] = c;
            config_bits = 29;
        } else {
            extra_cem_bits = 3 * partitions - 4;
            config_bits = 25 + 3 * partitions;
            // The CEM field: selector (2), a class bit per partition, then two mode bits per partition. The first 6
            // bits sit under the partition index; the rest sit just under the weights.
            const uint32_t field = static_cast<uint32_t>((lo >> 23) & 0x3F) |
                                   (get_bits(blk, 128 - weight_bits - extra_cem_bits, extra_cem_bits) << 6);
            for (int i = 0; i < partitions; i++) {
                const int cls = static_cast<int>(selector) - 1 + static_cast<int>((field >> (2 + i)) & 1);
                const int mbits = static_cast<int>((field >> (2 + partitions + 2 * i)) & 3);
                cem[i] = (cls << 2) | mbits;
            }
        }
    }
    int ccs = 0;
    if (bm.dual) {
        config_bits += 2;
        ccs = static_cast<int>(get_bits(blk, 128 - weight_bits - extra_cem_bits - 2, 2));
    }
    int values = 0;
    for (int i = 0; i < partitions; i++) values += 2 * ((cem[i] >> 2) + 1);
    const int remaining = 128 - config_bits - weight_bits;
    if (values > 18 || remaining <= 0) { fill_error(rgba, texels); return false; }
    int cr = -1;
    for (int r = kRangeCount - 1; r >= kMinColorRange; r--)
        if (ise_bits(values, kRanges[r]) <= remaining) { cr = r; break; }
    if (cr < 0) { fill_error(rgba, texels); return false; }

    const Tables& tb = tables();
    uint8_t raw[18];
    ise_decode(keep_bits(blk >> (partitions == 1 ? 17 : 29), ise_bits(values, kRanges[cr])), values, kRanges[cr], raw);
    int vals[18];
    for (int i = 0; i < values; i++) vals[i] = tb.color[cr][raw[i]];

    Rgba e0[4], e1[4];
    bool hdr_part[4] = {false, false, false, false};
    for (int i = 0, at = 0; i < partitions; i++) {
        const int n = 2 * ((cem[i] >> 2) + 1);
        hdr_part[i] = !decode_endpoints(cem[i], vals + at, e0[i], e1[i]);
        at += n;
    }

    // Weights: a bit stream growing down from bit 127.
    uint8_t wraw[128];
    ise_decode(keep_bits(reverse_bits128(lo, hi), weight_bits), num_weights, wr, wraw);
    uint8_t w[2][96] = {};
    const int grid = bm.width * bm.height;
    for (int i = 0; i < grid; i++) {
        w[0][i] = tb.weight[bm.range][wraw[bm.dual ? 2 * i : i]];
        if (bm.dual) w[1][i] = tb.weight[bm.range][wraw[2 * i + 1]];
    }

    const int seed = static_cast<int>((lo >> 13) & 0x3FF);
    const bool small_block = texels < 31;
    const int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            uint8_t* out = rgba + (y * bw + x) * 4;
            const int part = partitions > 1 ? select_partition(seed, x, y, partitions, small_block) : 0;
            if (hdr_part[part]) { fill_error(out, 1); continue; }
            const int gs = ((Ds * x) * (bm.width - 1) + 32) >> 6;
            const int gt = ((Dt * y) * (bm.height - 1) + 32) >> 6;
            const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            const int v0 = js + jt * bm.width;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            int idx[2];
            for (int p = 0; p < (bm.dual ? 2 : 1); p++)
                idx[p] = (w[p][v0] * w00 + w[p][v0 + 1] * w01 + w[p][v0 + bm.width] * w10 +
                          w[p][v0 + bm.width + 1] * w11 + 8) >> 4;
            if (!bm.dual) idx[1] = idx[0];
            const int ch0[4] = {e0[part].r, e0[part].g, e0[part].b, e0[part].a};
            const int ch1[4] = {e1[part].r, e1[part].g, e1[part].b, e1[part].a};
            for (int c = 0; c < 4; c++) {
                const int i = (bm.dual && c == ccs) ? idx[1] : idx[0];
                const int c0 = (ch0[c] << 8) | (srgb ? 0x80 : ch0[c]);
                const int c1 = (ch1[c] << 8) | (srgb ? 0x80 : ch1[c]);
                out[c] = static_cast<uint8_t>(((c0 * (64 - i) + c1 * i + 32) >> 6) >> 8);
            }
        }
    }
    return true;
}

}  // namespace arm64emu::texcodec
