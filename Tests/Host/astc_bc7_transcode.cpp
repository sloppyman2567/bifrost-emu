// ASTC to BC7 texture transcoding (Source/System/Graphics/TextureCodec).
//
// The ASTC decoder is checked against blocks built by hand from the Khronos specification (bit-only ranges, weight
// infill, dual plane, void extent), against properties the integer sequence encoding must have, and on random blocks
// for crashes and determinism. The BC7 encoder is checked by decoding its output with a decoder written separately
// from the BPTC section of the specification. Not checked against ARM's reference decoder or a GPU's ASTC path.
#include "System/Graphics/TextureCodec/AstcDecode.cpp"  // white box: tests the anonymous-namespace helpers too
#include "System/Graphics/TextureCodec/Bc7Encode.cpp"
#include "System/Graphics/TextureCodec/AstcToBc7.cpp"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace arm64emu::texcodec;

static int g_fail = 0;
#define CHECK(cond, what) do { if (!(cond)) { std::fprintf(stderr, "astc_bc7_transcode: FAIL %s\n", what); g_fail++; } } while (0)

namespace {

struct BlockBits {
    uint8_t b[16] = {};
    void set(int pos, int n, uint32_t v) {
        for (int i = 0; i < n; i++)
            if ((v >> i) & 1) b[(pos + i) >> 3] |= static_cast<uint8_t>(1u << ((pos + i) & 7));
    }
    // Weight stream: bit n of the stream is block bit 127 - n.
    void set_weight_bits(int stream_pos, int n, uint32_t v) {
        for (int i = 0; i < n; i++)
            if ((v >> i) & 1) { int bit = 127 - (stream_pos + i); b[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7)); }
    }
};

// Spec formula for the final step, written out again here: endpoints are expanded to 16 bits, interpolated with a
// weight in 0..64 and the top 8 bits kept.
int lerp8(int e0, int e1, int w, bool srgb) {
    const int c0 = (e0 << 8) | (srgb ? 0x80 : e0), c1 = (e1 << 8) | (srgb ? 0x80 : e1);
    return ((c0 * (64 - w) + c1 * w + 32) >> 6) >> 8;
}
int weight2(int v) { static const int t[4] = {0, 21, 43, 64}; return t[v]; }  // 0, 21, 42, 63 then +1 above 32

// A 4x4 block, one partition, CEM 12 (RGBA direct) with 8-bit endpoints, 4x4 grid of 2-bit weights.
void test_hand_built_blocks() {
    const int e0[4] = {10, 20, 30, 40}, e1[4] = {200, 210, 220, 230};
    for (int srgb = 0; srgb < 2; srgb++) {
        BlockBits b;
        b.set(0, 11, 0x42);   // 4x4 weights, range 0..3, single plane
        b.set(11, 2, 0);      // one partition
        b.set(13, 4, 12);     // CEM 12
        for (int c = 0; c < 4; c++) { b.set(17 + 16 * c, 8, e0[c]); b.set(17 + 16 * c + 8, 8, e1[c]); }
        int w[16];
        for (int i = 0; i < 16; i++) { w[i] = (i * 7 + 1) & 3; b.set_weight_bits(2 * i, 2, w[i]); }
        uint8_t out[16 * 4];
        CHECK(astc_decode_block(b.b, 4, 4, srgb, out), "hand-built 4x4 block is valid");
        bool same = true;
        for (int i = 0; i < 16; i++)
            for (int c = 0; c < 4; c++) same &= out[i * 4 + c] == lerp8(e0[c], e1[c], weight2(w[i]), srgb);
        CHECK(same, srgb ? "4x4 sRGB texels match the spec formula" : "4x4 linear texels match the spec formula");
    }

    // 6x6 block from a 3x3 grid of 3-bit weights: bilinear infill, written out from "Weight Infill".
    {
        BlockBits b;
        b.set(0, 11, 0x1BF);  // 3x3 weights, range 0..7
        b.set(13, 4, 12);
        for (int c = 0; c < 4; c++) { b.set(17 + 16 * c, 8, e0[c]); b.set(17 + 16 * c + 8, 8, e1[c]); }
        int g[9];
        for (int i = 0; i < 9; i++) { g[i] = (i * 5 + 2) & 7; b.set_weight_bits(3 * i, 3, g[i]); }
        auto unq3 = [](int v) { int u = (v << 3) | v; return u > 32 ? u + 1 : u; };
        uint8_t out[36 * 4];
        CHECK(astc_decode_block(b.b, 6, 6, false, out), "hand-built 6x6 block is valid");
        bool same = true;
        const int Ds = (1024 + 3) / 5;
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 6; x++) {
                const int gs = (Ds * x * 2 + 32) >> 6, gt = (Ds * y * 2 + 32) >> 6;
                const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
                auto at = [&](int cx, int cy) { return cx < 3 && cy < 3 ? unq3(g[cy * 3 + cx]) : 0; };
                const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
                const int i = (at(js, jt) * w00 + at(js + 1, jt) * w01 + at(js, jt + 1) * w10 + at(js + 1, jt + 1) * w11 + 8) >> 4;
                for (int c = 0; c < 4; c++) same &= out[(y * 6 + x) * 4 + c] == lerp8(e0[c], e1[c], i, false);
            }
        CHECK(same, "6x6 texels match the spec's weight infill");
    }

    // Dual plane: 4x2 grid of weight pairs in a 4x4 block; the colour component selector picks the second weight.
    for (int ccs = 0; ccs < 4; ccs++) {
        BlockBits b;
        b.set(0, 11, 0x402);  // 4x2 weights, range 0..3, dual plane
        b.set(13, 4, 12);
        for (int c = 0; c < 4; c++) { b.set(17 + 16 * c, 8, e0[c]); b.set(17 + 16 * c + 8, 8, e1[c]); }
        b.set(94, 2, ccs);    // 128 - weight bits (32) - 2
        int p[8][2];
        for (int i = 0; i < 8; i++)
            for (int k = 0; k < 2; k++) { p[i][k] = (i * 3 + k * 2 + 1) & 3; b.set_weight_bits(2 * (2 * i + k), 2, p[i][k]); }
        uint8_t out[16 * 4];
        CHECK(astc_decode_block(b.b, 4, 4, false, out), "hand-built dual-plane block is valid");
        bool same = true;
        const int Dt = (1024 + 2) / 3, Ds = Dt;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                const int gs = (Ds * x * 3 + 32) >> 6, gt = (Dt * y * 1 + 32) >> 6;
                const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
                int idx[2];
                for (int k = 0; k < 2; k++) {
                    auto at = [&](int cx, int cy) { return cx < 4 && cy < 2 ? weight2(p[cy * 4 + cx][k]) : 0; };
                    const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
                    idx[k] = (at(js, jt) * w00 + at(js + 1, jt) * w01 + at(js, jt + 1) * w10 + at(js + 1, jt + 1) * w11 + 8) >> 4;
                }
                for (int c = 0; c < 4; c++) same &= out[(y * 4 + x) * 4 + c] == lerp8(e0[c], e1[c], idx[c == ccs ? 1 : 0], false);
            }
        CHECK(same, "dual-plane texels match the spec");
    }
}

void test_void_extent() {
    for (int srgb = 0; srgb < 2; srgb++) {
        BlockBits b;
        b.set(0, 9, 0x1FC);
        b.set(10, 2, 3);  // reserved bits are ones
        for (int c = 0; c < 4; c++) b.set(64 + 16 * c, 16, 0x1234 * (c + 1) + 0x0F0F);
        b.set(12, 13, 0x1FFF); b.set(25, 13, 0x1FFF); b.set(38, 13, 0x1FFF); b.set(51, 13, 0x1FFF);
        uint8_t out[12 * 12 * 4];
        CHECK(astc_decode_block(b.b, 12, 12, srgb, out), "void-extent block is valid");
        bool same = true;
        for (int i = 0; i < 144; i++)
            for (int c = 0; c < 4; c++) same &= out[i * 4 + c] == ((0x1234 * (c + 1) + 0x0F0F) >> 8);
        CHECK(same, "void-extent colour is the top 8 bits of each UNORM16 component");
    }
    BlockBits hdr;
    hdr.set(0, 9, 0x1FC); hdr.set(9, 1, 1); hdr.set(10, 2, 3);
    hdr.set(12, 13, 0x1FFF); hdr.set(25, 13, 0x1FFF); hdr.set(38, 13, 0x1FFF); hdr.set(51, 13, 0x1FFF);
    uint8_t out[16 * 4];
    CHECK(!astc_decode_block(hdr.b, 4, 4, false, out) && out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255,
          "an HDR void-extent block decodes to the magenta error colour");
    BlockBits bad;
    bad.set(0, 9, 0x1FC); bad.set(10, 2, 3);
    bad.set(12, 13, 5); bad.set(25, 13, 4);  // min s >= max s
    CHECK(!astc_decode_block(bad.b, 4, 4, false, out), "a void extent with min >= max is illegal");
}

void test_ise_properties() {
    // Every trit combination must have an 8-bit encoding, and every quint combination a 7-bit one.
    bool seen_t[243] = {}, seen_q[125] = {};
    for (uint32_t T = 0; T < 256; T++) {
        uint8_t t[5]; decode_trits(T, t);
        int v = 0;
        for (int i = 4; i >= 0; i--) { CHECK(t[i] <= 2, "trit is 0..2"); v = v * 3 + t[i]; }
        seen_t[v] = true;
    }
    for (uint32_t Q = 0; Q < 128; Q++) {
        uint8_t q[3]; decode_quints(Q, q);
        int v = 0;
        for (int i = 2; i >= 0; i--) { CHECK(q[i] <= 4, "quint is 0..4"); v = v * 5 + q[i]; }
        seen_q[v] = true;
    }
    bool all_t = true, all_q = true;
    for (bool s : seen_t) all_t &= s;
    for (bool s : seen_q) all_q &= s;
    CHECK(all_t, "the 256 trit codes cover all 243 trit combinations");
    CHECK(all_q, "the 128 quint codes cover all 125 quint combinations");

    // Unquantization reaches both ends of the range and never repeats a value. For trit and quint ranges the order is
    // scrambled on purpose ("Endpoint Unquantization"), so only the set is checked.
    const Tables& tb = tables();
    for (int r = 0; r < kRangeCount; r++) {
        const int levels = kRanges[r].max + 1;
        if (r >= kMinColorRange) {
            bool seen[256] = {};
            int lo = 255, hi = 0;
            for (int v = 0; v < levels; v++) { seen[tb.color[r][v]] = true; lo = std::min<int>(lo, tb.color[r][v]); hi = std::max<int>(hi, tb.color[r][v]); }
            int distinct = 0;
            for (bool b : seen) distinct += b;
            CHECK(lo == 0 && hi == 255 && distinct == levels, "endpoint unquantization spans 0..255 without repeats");
        }
        if (levels <= 32) {
            bool seen[65] = {};
            int lo = 64, hi = 0;
            for (int v = 0; v < levels; v++) { seen[tb.weight[r][v]] = true; lo = std::min<int>(lo, tb.weight[r][v]); hi = std::max<int>(hi, tb.weight[r][v]); }
            int distinct = 0;
            for (bool b : seen) distinct += b;
            CHECK(lo == 0 && hi == 64 && distinct == levels, "weight unquantization spans 0..64 without repeats");
        }
    }
    // The 6-level trit tables worked out by hand from the specification's formulas.
    static const uint8_t kColor6[6] = {0, 255, 51, 204, 102, 153}, kWeight6[6] = {0, 64, 12, 52, 25, 39};
    for (int v = 0; v < 6; v++) {
        CHECK(tb.color[4][v] == kColor6[v], "0..5 endpoint unquantization matches the hand-worked table");
        CHECK(tb.weight[4][v] == kWeight6[v], "0..5 weight unquantization matches the hand-worked table");
    }
    // Bit-only endpoints replicate the top bits (the 3-bit value 5 = 101 becomes 10110110).
    CHECK(tb.color[5][5] == 0xB6, "a 3-bit endpoint is bit-replicated");
    CHECK(tb.color[11][21] == ((21 << 3) | (21 >> 2)), "a 5-bit endpoint is bit-replicated");
    // The integer sequence encoding sizes in the specification.
    CHECK(ise_bits(16, kRanges[2]) == 32 && ise_bits(5, kRanges[1]) == 8 && ise_bits(3, kRanges[3]) == 7 &&
          ise_bits(8, kRanges[20]) == 64 && ise_bits(12, kRanges[12]) == 12 * 3 + 28,
          "sequence sizes match 'Data Size Determination'");
}

void test_decoder_fuzz() {
    std::mt19937_64 rng(12345);
    int valid = 0;
    for (int n = 0; n < 200000; n++) {
        uint8_t blk[16];
        for (int i = 0; i < 16; i += 8) { uint64_t r = rng(); std::memcpy(blk + i, &r, 8); }
        static const int kDims[][2] = {{4, 4}, {5, 4}, {6, 6}, {8, 5}, {10, 8}, {12, 12}};
        const int* d = kDims[n % 6];
        uint8_t a[12 * 12 * 4], b[12 * 12 * 4];
        const bool va = astc_decode_block(blk, d[0], d[1], n & 1, a);
        const bool vb = astc_decode_block(blk, d[0], d[1], n & 1, b);
        if (std::memcmp(a, b, d[0] * d[1] * 4) != 0 || va != vb) { CHECK(false, "decoding is deterministic"); break; }
        valid += va;
    }
    CHECK(valid > 1000, "a useful share of random blocks decodes as valid");
}

// ── BC7 ────────────────────────────────────────────────────────────────────────────────────────────────────────────
// A decoder written from the BPTC chapter's field tables, separate from the encoder's packer.
struct DecMode { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
const DecMode kDec[8] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
                         {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
                         {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

bool bc7_decode(const uint8_t* blk, uint8_t out[16][4]) {
    auto bits = [&](int pos, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v |= ((blk[(pos + i) >> 3] >> ((pos + i) & 7)) & 1u) << i; return v; };
    if (blk[0] == 0) { std::memset(out, 0, 64); return false; }
    int mode = 0;
    while (!((blk[0] >> mode) & 1)) mode++;
    const DecMode& m = kDec[mode];
    int pos = mode + 1;
    const int part = bits(pos, m.pb); pos += m.pb;
    const int rot = bits(pos, m.rb); pos += m.rb;
    const int isel = bits(pos, m.isb); pos += m.isb;
    int v[6][4];
    for (int c = 0; c < 3; c++) for (int e = 0; e < 2 * m.ns; e++) { v[e][c] = bits(pos, m.cb); pos += m.cb; }
    for (int e = 0; e < 2 * m.ns; e++) v[e][3] = 255;
    if (m.ab) for (int e = 0; e < 2 * m.ns; e++) { v[e][3] = bits(pos, m.ab); pos += m.ab; }
    int p[6] = {};
    bool pbit = m.epb || m.spb;
    if (m.epb) for (int e = 0; e < 2 * m.ns; e++) { p[e] = bits(pos, 1); pos++; }
    if (m.spb) for (int s = 0; s < m.ns; s++) { p[2 * s] = p[2 * s + 1] = bits(pos, 1); pos++; }
    auto expand = [](int value, int nbits) { return (value << (8 - nbits)) | (value >> (2 * nbits - 8)); };
    for (int e = 0; e < 2 * m.ns; e++)
        for (int c = 0; c < 4; c++) {
            const int nb = c < 3 ? m.cb : m.ab;
            if (nb == 0) continue;  // alpha stays 255
            int val = v[e][c], n = nb;
            if (pbit) { val = (val << 1) | p[e]; n++; }
            v[e][c] = expand(val, n);
        }
    auto subset_of = [&](int i) { return m.ns == 1 ? 0 : m.ns == 2 ? kPartition2[part][i] : kPartition3[part][i]; };
    auto is_anchor = [&](int i) {
        if (i == 0) return true;
        if (m.ns == 2) return i == kAnchor2[part];
        if (m.ns == 3) return i == kAnchor3A[part] || i == kAnchor3B[part];
        return false;
    };
    int idx[16], idx2[16] = {};
    for (int i = 0; i < 16; i++) { const int n = m.ib - (is_anchor(i) ? 1 : 0); idx[i] = bits(pos, n); pos += n; }
    if (m.ib2) for (int i = 0; i < 16; i++) { const int n = m.ib2 - (i == 0 ? 1 : 0); idx2[i] = bits(pos, n); pos += n; }
    auto weight = [](int ib, int i) {
        return ib == 2 ? (i * 21) + (i >> 1) : ib == 3 ? (i * 9) + (i >> 2) : (i << 2) + (i >> 2) + ((i >> 1) & 1);
    };
    for (int i = 0; i < 16; i++) {
        const int s = subset_of(i);
        int ci = idx[i], ai = idx[i], cib = m.ib, aib = m.ib;
        if (m.ib2) { if (isel) { ci = idx2[i]; cib = m.ib2; ai = idx[i]; aib = m.ib; } else { ai = idx2[i]; aib = m.ib2; } }
        int px[4];
        for (int c = 0; c < 4; c++) {
            const int w = c < 3 ? weight(cib, ci) : weight(aib, ai);
            px[c] = ((64 - w) * v[2 * s][c] + w * v[2 * s + 1][c] + 32) >> 6;
        }
        if (rot) std::swap(px[3], px[rot - 1]);
        for (int c = 0; c < 4; c++) out[i][c] = static_cast<uint8_t>(px[c]);
    }
    return true;
}

double block_error(const uint8_t* src, const uint8_t out[16][4], int channels) {
    double e = 0;
    for (int i = 0; i < 16; i++) for (int c = 0; c < channels; c++) { const double d = src[i * 4 + c] - out[i][c]; e += d * d; }
    return e;
}

void test_bc7_modes_pack_correctly() {
    // For every mode and several partitions, the error the encoder believes it has must equal the error an
    // independent decoder measures on the packed block: that covers p-bits, anchors, swaps and the index layout.
    std::mt19937 rng(7);
    int checked = 0;
    for (int mode : {0, 1, 2, 3, 6, 7}) {
        const Mode& m = kModes[mode];
        const int partitions = m.ns == 1 ? 1 : (1 << m.pb);
        for (int trial = 0; trial < 300; trial++) {
            uint8_t rgba[64];
            const int kind = trial % 4;
            const int base[4] = {static_cast<int>(rng() & 255), static_cast<int>(rng() & 255), static_cast<int>(rng() & 255), static_cast<int>(rng() & 255)};
            for (int i = 0; i < 16; i++)
                for (int c = 0; c < 4; c++) {
                    int v = kind == 0 ? static_cast<int>(rng() & 255) : base[c] + static_cast<int>(rng() % (kind == 1 ? 9 : 60)) - 4 * kind;
                    if (kind == 3) v = (i & 5) ? base[c] : 255 - base[c];
                    rgba[i * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0, 255));
                }
            const bool alpha_mode = m.ab > 0;
            if (!alpha_mode) for (int i = 0; i < 16; i++) rgba[i * 4 + 3] = 255;
            int px[16][4];
            for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) px[i][c] = rgba[i * 4 + c];
            const int partition = static_cast<int>(rng() % partitions);
            Candidate cand;
            try_mode(mode, partition, px, alpha_mode ? 4 : 3, 3, cand);
            uint8_t blk[16];
            pack(cand, blk);
            uint8_t out[16][4];
            CHECK(bc7_decode(blk, out), "an encoded block decodes");
            const double e = block_error(rgba, out, alpha_mode ? 4 : 3);
            if (static_cast<int>(e) != cand.err) {
                std::fprintf(stderr, "  mode %d partition %d trial %d: encoder %d, decoder %.0f\n", mode, partition, trial, cand.err, e);
                CHECK(false, "encoder error equals the independent decoder's error");
                break;
            }
            checked++;
            bool opaque_ok = true;
            if (!alpha_mode) for (int i = 0; i < 16; i++) opaque_ok &= out[i][3] == 255;
            CHECK(opaque_ok, "modes without alpha decode to alpha 255");
        }
    }
    CHECK(checked > 1000, "enough mode/partition combinations were checked");
}

void test_bc7_exact_cases() {
    Bc7Options o;
    // A flat colour is within one level per channel: an endpoint's p-bit is shared by all its channels, so channels
    // of different parity cannot all be exact; when the parities agree it is exact.
    std::mt19937 rng(3);
    for (int n = 0; n < 200; n++) {
        uint8_t rgba[64];
        uint8_t c[4] = {static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), static_cast<uint8_t>(rng()), static_cast<uint8_t>(n & 1 ? 255 : rng())};
        if (n % 4 == 0) for (int k = 1; k < 4; k++) c[k] = static_cast<uint8_t>((c[k] & ~1) | (c[0] & 1));  // equal parity
        for (int i = 0; i < 16; i++) std::memcpy(rgba + i * 4, c, 4);
        uint8_t blk[16], out[16][4];
        bc7_encode_block(rgba, o, blk);
        bc7_decode(blk, out);
        int worst = 0;
        for (int i = 0; i < 16; i++) for (int k = 0; k < 4; k++) worst = std::max(worst, std::abs(out[i][k] - rgba[i * 4 + k]));
        CHECK(worst <= 1, "a flat colour is within one level");
        if (n % 4 == 0 && c[3] != 255) CHECK(worst == 0, "a flat colour with matching channel parity is exact");
        if (c[3] == 255) CHECK(out[0][3] == 255, "a flat opaque colour keeps alpha 255");
    }
    // Two colours split by a vertical edge: the two endpoints are the colours themselves (opaque mode 6 has odd values only).
    uint8_t rgba[64];
    for (int i = 0; i < 16; i++) {
        const bool right = (i & 3) >= 2;
        const uint8_t px[4] = {static_cast<uint8_t>(right ? 10 : 250), static_cast<uint8_t>(right ? 60 : 30), static_cast<uint8_t>(right ? 200 : 20), 255};
        std::memcpy(rgba + i * 4, px, 4);
    }
    uint8_t blk[16], out[16][4];
    bc7_encode_block(rgba, o, blk);
    bc7_decode(blk, out);
    int worst = 0;
    for (int i = 0; i < 16; i++) for (int k = 0; k < 4; k++) worst = std::max(worst, std::abs(out[i][k] - rgba[i * 4 + k]));
    CHECK(worst <= 1, "a two-colour block is within one level");
    // An opaque block never gains transparency, whatever mode wins.
    bool opaque = true;
    for (int n = 0; n < 500; n++) {
        for (int i = 0; i < 64; i++) rgba[i] = static_cast<uint8_t>(rng());
        for (int i = 0; i < 16; i++) rgba[i * 4 + 3] = 255;
        bc7_encode_block(rgba, o, blk);
        bc7_decode(blk, out);
        for (int i = 0; i < 16; i++) opaque &= out[i][3] == 255;
    }
    CHECK(opaque, "opaque blocks stay opaque");
}

void test_bc7_quality() {
    // Smooth content must come out close, and the partitioned modes must help on a block of three flat colours.
    Bc7Options o;
    std::mt19937 rng(11);
    double total = 0;
    for (int n = 0; n < 500; n++) {
        uint8_t rgba[64];
        const double gx = static_cast<int>(rng() % 41) - 20, gy = static_cast<int>(rng() % 41) - 20;
        const int base = 60 + rng() % 120;
        for (int i = 0; i < 16; i++) {
            const int v = static_cast<int>(base + gx * (i & 3) + gy * (i >> 2));
            for (int c = 0; c < 3; c++) rgba[i * 4 + c] = static_cast<uint8_t>(std::clamp(v + c * 9, 0, 255));
            rgba[i * 4 + 3] = 255;
        }
        uint8_t blk[16], out[16][4];
        bc7_encode_block(rgba, o, blk);
        bc7_decode(blk, out);
        total += block_error(rgba, out, 3) / 48;
    }
    CHECK(total / 500 < o.accept_error, "smooth gradients stay under the accept error (mean squared error per channel)");

    uint8_t rgba[64];
    const uint8_t colors[3][3] = {{250, 20, 20}, {20, 250, 20}, {20, 20, 250}};
    for (int i = 0; i < 16; i++) {
        const int s = (i & 3) < 2 ? 0 : (i < 8 ? 1 : 2);
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = colors[s][c];
        rgba[i * 4 + 3] = 255;
    }
    uint8_t blk[16], out[16][4];
    bc7_encode_block(rgba, o, blk);
    bc7_decode(blk, out);
    CHECK(block_error(rgba, out, 3) / 48 < 8.0, "three flat colours are close with the partitioned modes");
}

void test_transcode_image() {
    static const int kFoot[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    static const int kSizes[][2] = {{37, 29}, {64, 64}, {1, 1}, {130, 41}};
    for (const auto& f : kFoot) {
        for (const auto& sz : kSizes) {
            const int bw = f[0], bh = f[1], W = sz[0], H = sz[1];
            const int bx_n = (W + bw - 1) / bw, by_n = (H + bh - 1) / bh;
            auto color = [](int bx, int by, int c) { return (c == 3 ? 255 : (40 + 37 * bx + 61 * by + 90 * c) % 128 * 2); };
            const size_t pitch = static_cast<size_t>(bx_n) * 16 + 16;  // a row pitch larger than the row
            std::vector<uint8_t> src(pitch * by_n, 0);
            for (int by = 0; by < by_n; by++)
                for (int bx = 0; bx < bx_n; bx++) {
                    BlockBits b;
                    b.set(0, 9, 0x1FC); b.set(10, 2, 3);
                    b.set(12, 13, 0x1FFF); b.set(25, 13, 0x1FFF); b.set(38, 13, 0x1FFF); b.set(51, 13, 0x1FFF);
                    for (int c = 0; c < 4; c++) b.set(64 + 16 * c, 16, color(bx, by, c) << 8);
                    std::memcpy(&src[by * pitch + bx * 16], b.b, 16);
                }
            const AstcRegion region{src.data(), pitch, bw, bh, false, W, H};
            std::vector<uint8_t> one(bc7_image_bytes(W, H)), many(bc7_image_bytes(W, H));
            Bc7Options o;
            CHECK(transcode_astc_to_bc7(region, o, 1, one.data()), "transcode runs");
            CHECK(transcode_astc_to_bc7(region, o, 8, many.data()), "transcode runs on threads");
            CHECK(one == many, "threads do not change the output");
            const int b4x = (W + 3) / 4, b4y = (H + 3) / 4;
            bool good = true;
            for (int ry = 0; ry < b4y; ry++)
                for (int rx = 0; rx < b4x; rx++) {
                    uint8_t out[16][4];
                    bc7_decode(&one[(static_cast<size_t>(ry) * b4x + rx) * 16], out);
                    int distinct = 0, seen[4] = {-1, -1, -1, -1};
                    for (int j = 0; j < 16; j++) {
                        const int x = rx * 4 + (j & 3), y = ry * 4 + (j >> 2);
                        if (x >= W || y >= H) continue;
                        const int id = (y / bh) * 64 + (x / bw);
                        bool known = false;
                        for (int k = 0; k < distinct; k++) known |= seen[k] == id;
                        if (!known && distinct < 4) seen[distinct++] = id;
                    }
                    if (distinct > 2) continue;
                    for (int j = 0; j < 16; j++) {
                        const int x = rx * 4 + (j & 3), y = ry * 4 + (j >> 2);
                        if (x >= W || y >= H) continue;
                        for (int c = 0; c < 4; c++) good &= std::abs(out[j][c] - color(x / bw, y / bh, c)) <= 2;
                    }
                }
            if (!good) std::fprintf(stderr, "  footprint %dx%d image %dx%d\n", bw, bh, W, H);
            CHECK(good, "texels of blocks with at most two source colours match");
        }
    }
    uint8_t dummy[16] = {};
    uint8_t out[16];
    CHECK(!transcode_astc_to_bc7(AstcRegion{dummy, 16, 7, 7, false, 4, 4}, Bc7Options{}, 1, out), "an invalid footprint is refused");
    CHECK(!transcode_astc_to_bc7(AstcRegion{dummy, 8, 4, 4, false, 4, 4}, Bc7Options{}, 1, out), "a row pitch shorter than the row is refused");
}

}  // namespace

int main() {
    test_hand_built_blocks();
    test_void_extent();
    test_ise_properties();
    test_decoder_fuzz();
    test_bc7_modes_pack_correctly();
    test_bc7_exact_cases();
    test_bc7_quality();
    test_transcode_image();
    std::printf(g_fail ? "astc_bc7_transcode: FAILED (%d)\n" : "astc_bc7_transcode: ALL PASS\n", g_fail);
    return g_fail != 0;
}
