// Bc7Encode.cpp — see Bc7Encode.hpp. Bit layouts, partition tables and interpolation weights are from the Khronos
// Data Format Specification's BPTC chapter.
#include "System/Graphics/TextureCodec/Bc7Encode.hpp"
#include "System/Graphics/TextureCodec/Bc7Tables.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace arm64emu::texcodec {
namespace {

using namespace bc7;

// NS subsets, PB partition bits, CB colour bits, AB alpha bits, PK p-bit kind (0 none, 1 per endpoint, 2 one shared
// by both endpoints of a subset), IB index bits. Modes 4 and 5 (separate alpha indices, rotation) are not produced.
struct Mode { uint8_t ns, pb, cb, ab, pk, ib; };
constexpr Mode kModes[8] = {
    {3, 4, 4, 0, 1, 3}, {2, 6, 6, 0, 2, 3}, {3, 6, 5, 0, 0, 2}, {2, 6, 7, 0, 1, 2},
    {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}, {1, 0, 7, 7, 1, 4}, {2, 6, 5, 5, 1, 2},
};
constexpr uint8_t kWeight2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeight3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const uint8_t* weights(int ib) { return ib == 2 ? kWeight2 : ib == 3 ? kWeight3 : kWeight4; }

// An n-bit endpoint value expanded to 8 bits by replicating its top bits.
struct Dequant {
    uint8_t v[9][256];
    Dequant() {
        std::memset(v, 0, sizeof(v));
        for (int bits = 1; bits <= 8; bits++)
            for (int x = 0; x < (1 << bits); x++) {
                int r = 0;
                for (int shift = 8 - bits;; shift -= bits) {
                    r |= shift >= 0 ? x << shift : x >> -shift;
                    if (shift <= 0) break;
                }
                v[bits][x] = static_cast<uint8_t>(r);
            }
    }
};
const Dequant kDq;

struct Endpoints {
    uint8_t q0[4] = {}, q1[4] = {};  // quantized to CB / AB bits
    uint8_t p0 = 0, p1 = 0;          // p-bits (shared: both the same)
    uint8_t e0[4] = {255, 255, 255, 255}, e1[4] = {255, 255, 255, 255};  // as decoded, 8 bits
};

struct Quant { uint8_t q[4]; uint8_t e[4]; };

// Quantizes one endpoint with p-bit `p` (-1 when the mode has none). `channels` < 4 means alpha is the constant 255.
Quant quantize_endpoint(const float* v, int channels, const Mode& m, int p) {
    Quant out{};
    for (int c = 0; c < 4; c++) {
        const int nb = c < 3 ? m.cb : m.ab;
        if (nb == 0) { out.q[c] = 0; out.e[c] = 255; continue; }
        const int bits = nb + (p >= 0 ? 1 : 0), maxq = (1 << nb) - 1;
        if (c >= channels) {  // opaque block in a mode with alpha: the largest value, with the p-bit set
            out.q[c] = static_cast<uint8_t>(maxq);
            out.e[c] = kDq.v[bits][p >= 0 ? (maxq << 1) | p : maxq];
            continue;
        }
        const float scale = static_cast<float>((1 << bits) - 1) / 255.f;
        // lround, then clamp. Truncating x + 0.5 (exact in double) is floor(x + 0.5) = lround(x) for x >= 0; below
        // zero both clamp to 0. This avoided a libm call per channel.
        const float x = (v[c] * scale - (p >= 0 ? p : 0)) / (p >= 0 ? 2.f : 1.f);
        int guess = static_cast<int>(static_cast<double>(x) + 0.5);
        guess = std::clamp(guess, 0, maxq);
        int best_q = guess;
        float best_d = 1e30f;
        for (int q = std::max(guess - 1, 0); q <= std::min(guess + 1, maxq); q++) {
            const float d = std::fabs(static_cast<float>(kDq.v[bits][p >= 0 ? (q << 1) | p : q]) - v[c]);
            if (d < best_d) { best_d = d; best_q = q; }
        }
        out.q[c] = static_cast<uint8_t>(best_q);
        out.e[c] = kDq.v[bits][p >= 0 ? (best_q << 1) | p : best_q];
    }
    return out;
}

float quant_error(const Quant& q, const float* v, int channels) {
    float e = 0;
    for (int c = 0; c < channels; c++) { const float d = q.e[c] - v[c]; e += d * d; }
    return e;
}

Endpoints quantize_pair(const float* a, const float* b, int channels, const Mode& m) {
    Endpoints ep;
    Quant qa, qb;
    int pa = 0, pb = 0;
    if (m.pk == 0) {
        qa = quantize_endpoint(a, channels, m, -1);
        qb = quantize_endpoint(b, channels, m, -1);
    } else if (m.pk == 1) {
        // An opaque block in a mode with alpha needs the p-bit set to reach alpha 255.
        const bool force = channels < 4 && m.ab > 0;
        Quant ca[2] = {quantize_endpoint(a, channels, m, 0), quantize_endpoint(a, channels, m, 1)};
        Quant cb[2] = {quantize_endpoint(b, channels, m, 0), quantize_endpoint(b, channels, m, 1)};
        pa = force || quant_error(ca[1], a, channels) < quant_error(ca[0], a, channels);
        pb = force || quant_error(cb[1], b, channels) < quant_error(cb[0], b, channels);
        qa = ca[pa]; qb = cb[pb];
    } else {
        Quant ca[2] = {quantize_endpoint(a, channels, m, 0), quantize_endpoint(a, channels, m, 1)};
        Quant cb[2] = {quantize_endpoint(b, channels, m, 0), quantize_endpoint(b, channels, m, 1)};
        const float e0 = quant_error(ca[0], a, channels) + quant_error(cb[0], b, channels);
        const float e1 = quant_error(ca[1], a, channels) + quant_error(cb[1], b, channels);
        pa = pb = e1 < e0;
        qa = ca[pa]; qb = cb[pb];
    }
    std::memcpy(ep.q0, qa.q, 4); std::memcpy(ep.q1, qb.q, 4);
    std::memcpy(ep.e0, qa.e, 4); std::memcpy(ep.e1, qb.e, 4);
    ep.p0 = static_cast<uint8_t>(pa); ep.p1 = static_cast<uint8_t>(pb);
    return ep;
}

// Nearest palette entry for every pixel of a subset (the first of equal ones). Returns the squared error. The distances
// to four entries come from two PMADDWD: one register holds the entries' (R, G) pairs, the other (B, A), as int16
// differences; their squares fit int32 exactly. Alpha differences are zero when `channels` is 3.
template <int Groups>
int assign_indices_n(const int (*px)[4], const uint8_t* ids, int n, int channels, const uint8_t* w, const uint8_t* e0,
                     const uint8_t* e1, uint8_t* idx) {
    constexpr int count = Groups * 4;
    alignas(16) int16_t rg[count][2], ba[count][2];
    for (int k = 0; k < count; k++) {
        int pal[4] = {0, 0, 0, 0};
        for (int c = 0; c < channels; c++) pal[c] = ((64 - w[k]) * e0[c] + w[k] * e1[c] + 32) >> 6;
        rg[k][0] = static_cast<int16_t>(pal[0]); rg[k][1] = static_cast<int16_t>(pal[1]);
        ba[k][0] = static_cast<int16_t>(pal[2]); ba[k][1] = static_cast<int16_t>(pal[3]);
    }
    __m128i prg[Groups], pba[Groups];
    for (int g = 0; g < Groups; g++) {
        prg[g] = _mm_load_si128(reinterpret_cast<const __m128i*>(rg[4 * g]));
        pba[g] = _mm_load_si128(reinterpret_cast<const __m128i*>(ba[4 * g]));
    }
    int total = 0;
    for (int i = 0; i < n; i++) {
        const int* p = px[ids[i]];
        const int a = channels > 3 ? p[3] : 0;
        const __m128i xrg = _mm_set1_epi32(static_cast<int>((static_cast<uint32_t>(p[1]) << 16) | static_cast<uint32_t>(p[0])));
        const __m128i xba = _mm_set1_epi32(static_cast<int>((static_cast<uint32_t>(a) << 16) | static_cast<uint32_t>(p[2])));
        alignas(16) int d[count];
        for (int g = 0; g < Groups; g++) {
            const __m128i drg = _mm_sub_epi16(xrg, prg[g]), dba = _mm_sub_epi16(xba, pba[g]);
            _mm_store_si128(reinterpret_cast<__m128i*>(d + 4 * g),
                            _mm_add_epi32(_mm_madd_epi16(drg, drg), _mm_madd_epi16(dba, dba)));
        }
        int best = d[0], best_k = 0;
        for (int k = 1; k < count; k++)
            if (d[k] < best) { best = d[k]; best_k = k; }
        idx[i] = static_cast<uint8_t>(best_k);
        total += best;
    }
    return total;
}

int assign_indices(const int (*px)[4], const uint8_t* ids, int n, int channels, int ib, const uint8_t* e0,
                   const uint8_t* e1, uint8_t* idx) {
    const uint8_t* w = weights(ib);
    if (ib == 2) return assign_indices_n<1>(px, ids, n, channels, w, e0, e1, idx);
    if (ib == 3) return assign_indices_n<2>(px, ids, n, channels, w, e0, e1, idx);
    return assign_indices_n<4>(px, ids, n, channels, w, e0, e1, idx);
}

// Least-squares endpoints for fixed indices; false when the indices do not span a line.
bool refit(const int (*px)[4], const uint8_t* ids, const uint8_t* idx, int n, int channels, int ib, float* a, float* b) {
    const uint8_t* w = weights(ib);
    double aa = 0, ab = 0, bb = 0, ax[4] = {}, bx[4] = {};
    for (int i = 0; i < n; i++) {
        const double wi = w[idx[i]] / 64.0, ia = 1.0 - wi;
        aa += ia * ia; ab += ia * wi; bb += wi * wi;
        for (int c = 0; c < channels; c++) { ax[c] += ia * px[ids[i]][c]; bx[c] += wi * px[ids[i]][c]; }
    }
    const double det = aa * bb - ab * ab;
    if (det < 1e-6) return false;
    for (int c = 0; c < channels; c++) {
        a[c] = static_cast<float>(std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0, 255.0));
        b[c] = static_cast<float>(std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0, 255.0));
    }
    return true;
}

// Principal axis of a subset's colours by power iteration. False for a single colour.
bool principal_axis(const int (*px)[4], const uint8_t* ids, int n, int channels, float* mean, float* axis) {
    double m[4] = {}, s[4][4] = {};
    for (int i = 0; i < n; i++)
        for (int c = 0; c < channels; c++) m[c] += px[ids[i]][c];
    for (int c = 0; c < channels; c++) m[c] /= n;
    for (int i = 0; i < n; i++)
        for (int c = 0; c < channels; c++)
            for (int d = c; d < channels; d++) s[c][d] += (px[ids[i]][c] - m[c]) * (px[ids[i]][d] - m[d]);
    for (int c = 0; c < channels; c++)
        for (int d = 0; d < c; d++) s[c][d] = s[d][c];
    double v[4] = {};
    int best = 0;
    for (int c = 1; c < channels; c++) if (s[c][c] > s[best][best]) best = c;
    if (s[best][best] < 1e-9) { for (int c = 0; c < channels; c++) mean[c] = static_cast<float>(m[c]); return false; }
    for (int c = 0; c < channels; c++) v[c] = s[c][best];
    for (int it = 0; it < 4; it++) {
        double t[4] = {}, len = 0;
        for (int c = 0; c < channels; c++) for (int d = 0; d < channels; d++) t[c] += s[c][d] * v[d];
        for (int c = 0; c < channels; c++) len += t[c] * t[c];
        len = std::sqrt(len);
        if (len < 1e-12) break;
        for (int c = 0; c < channels; c++) v[c] = t[c] / len;
    }
    for (int c = 0; c < channels; c++) { mean[c] = static_cast<float>(m[c]); axis[c] = static_cast<float>(v[c]); }
    return true;
}

struct SubsetFit {
    Endpoints ep;
    int err = 1 << 30;
};

// Fits one subset: endpoints and indices (`idx`, one per pixel of `ids`).
SubsetFit fit_subset(const int (*px)[4], const uint8_t* ids, int n, int channels, const Mode& m, int rounds, uint8_t* idx) {
    SubsetFit best;
    if (n <= 0) { best.err = 0; return best; }
    float mean[4] = {}, axis[4] = {};
    const bool spans = principal_axis(px, ids, n, channels, mean, axis);
    float start[2][2][4];  // [which][endpoint][channel]
    int starts = 1;
    if (!spans) {
        for (int e = 0; e < 2; e++) for (int c = 0; c < 4; c++) start[0][e][c] = mean[c];
    } else {
        float tmin = 1e30f, tmax = -1e30f;
        int imin = 0, imax = 0;
        for (int i = 0; i < n; i++) {
            float t = 0;
            for (int c = 0; c < channels; c++) t += (px[ids[i]][c] - mean[c]) * axis[c];
            if (t < tmin) { tmin = t; imin = i; }
            if (t > tmax) { tmax = t; imax = i; }
        }
        for (int c = 0; c < channels; c++) {
            start[0][0][c] = std::clamp(mean[c] + axis[c] * tmin, 0.f, 255.f);
            start[0][1][c] = std::clamp(mean[c] + axis[c] * tmax, 0.f, 255.f);
            start[1][0][c] = static_cast<float>(px[ids[imin]][c]);
            start[1][1][c] = static_cast<float>(px[ids[imax]][c]);
        }
        starts = 2;
    }
    uint8_t cur[16];
    for (int s = 0; s < starts; s++) {
        float a[4], b[4];
        std::memcpy(a, start[s][0], sizeof(a)); std::memcpy(b, start[s][1], sizeof(b));
        for (int r = 0; r < rounds; r++) {
            const Endpoints ep = quantize_pair(a, b, channels, m);
            const int err = assign_indices(px, ids, n, channels, m.ib, ep.e0, ep.e1, cur);
            if (err < best.err) { best.err = err; best.ep = ep; std::memcpy(idx, cur, static_cast<size_t>(n)); }
            if (err == 0 || r + 1 == rounds) break;
            if (!refit(px, ids, cur, n, channels, m.ib, a, b)) break;
        }
        if (best.err == 0) break;
    }
    return best;
}

struct Candidate {
    int err = 1 << 30;
    int mode = -1, partition = 0;
    Endpoints ep[3];
    uint8_t idx[16];
};

void subset_ids(const Mode& m, int partition, uint8_t ids[3][16], int count[3]) {
    count[0] = count[1] = count[2] = 0;
    for (int i = 0; i < 16; i++) {
        const int s = m.ns == 1 ? 0 : m.ns == 2 ? kPartition2[partition][i] : kPartition3[partition][i];
        ids[s][count[s]++] = static_cast<uint8_t>(i);
    }
}

int anchor_of(const Mode& m, int partition, int subset) {
    if (subset == 0) return 0;
    if (m.ns == 2) return kAnchor2[partition];
    return subset == 1 ? kAnchor3A[partition] : kAnchor3B[partition];
}

void try_mode(int mode, int partition, const int (*px)[4], int channels, int rounds, Candidate& best) {
    const Mode& m = kModes[mode];
    uint8_t ids[3][16];
    int count[3];
    subset_ids(m, partition, ids, count);
    Candidate c;
    c.mode = mode;
    c.partition = partition;
    c.err = 0;
    for (int s = 0; s < m.ns && c.err < best.err; s++) {
        uint8_t idx[16];
        SubsetFit f = fit_subset(px, ids[s], count[s], channels, m, rounds, idx);
        // The anchor index stores one bit less: its top bit must be zero, so swap the endpoints if it is not.
        const int anchor = anchor_of(m, partition, s);
        for (int i = 0; i < count[s]; i++) {
            if (ids[s][i] != anchor) continue;
            if (idx[i] >= (1 << (m.ib - 1))) {
                for (int ch = 0; ch < 4; ch++) { std::swap(f.ep.q0[ch], f.ep.q1[ch]); std::swap(f.ep.e0[ch], f.ep.e1[ch]); }
                std::swap(f.ep.p0, f.ep.p1);
                for (int j = 0; j < count[s]; j++) idx[j] = static_cast<uint8_t>((1 << m.ib) - 1 - idx[j]);
            }
            break;
        }
        for (int i = 0; i < count[s]; i++) c.idx[ids[s][i]] = idx[i];
        c.ep[s] = f.ep;
        c.err += f.err;
    }
    if (c.err < best.err) best = c;
}

// Colour sums of a set of pixels, as 16 int32: the four channel sums, the ten channel products (upper triangle, see
// kSq), the pixel count, and a zero. Pixels are summed four lanes at a time under a mask, with no branch on the subset.
struct alignas(16) Moments { int v[16]; };
constexpr int kSq[4][4] = {{4, 5, 6, 7}, {5, 8, 9, 10}, {6, 9, 11, 12}, {7, 10, 12, 13}};
constexpr int kCount = 14;

// A subset's colour covariance. The sums are integers, exactly the values the double accumulation they replaced
// produced, so the estimate below is unchanged. False below two pixels (the estimate is then 0).
struct Covariance { double c[4][4]; double trace = 0, frobenius = 0; };
bool subset_covariance(const Moments& m, int channels, Covariance& out) {
    const int n = m.v[kCount];
    if (n < 2) return false;
    double sq = 0;
    for (int c = 0; c < channels; c++)
        for (int d = c; d < channels; d++) {
            const double v = static_cast<double>(m.v[kSq[c][d]]) -
                             static_cast<double>(m.v[c]) * static_cast<double>(m.v[d]) / n;
            out.c[c][d] = out.c[d][c] = v;
            sq += c == d ? v * v : 2 * v * v;
        }
    for (int c = 0; c < channels; c++) out.trace += out.c[c][c];
    out.frobenius = std::sqrt(sq);
    return true;
}

// How well a subset fits one line: the part of its colour variance off the principal axis (power iteration).
double subset_estimate(const Covariance& k, int channels) {
    const auto& cov = k.c;
    int best = 0;
    for (int c = 1; c < channels; c++) if (cov[c][c] > cov[best][best]) best = c;
    double v[4] = {};
    for (int c = 0; c < channels; c++) v[c] = cov[c][best];
    double lambda = 0;
    for (int it = 0; it < 3; it++) {
        double t[4] = {}, len = 0;
        for (int c = 0; c < channels; c++) for (int d = 0; d < channels; d++) t[c] += cov[c][d] * v[d];
        for (int c = 0; c < channels; c++) len += t[c] * t[c];
        len = std::sqrt(len);
        if (len < 1e-9) break;
        for (int c = 0; c < channels; c++) v[c] = t[c] / len;
        lambda = len;
    }
    return std::max(k.trace - lambda, 0.0);
}

// The `keep` best partitions of `ns` subsets, best first. Each partition sums the pixels of all subsets but the last,
// which is the block's total minus the others.
int rank_partitions(const int (*px)[4], int channels, int ns, int keep, int* out) {
    struct Scored { double score; int partition; };
    Scored top[8];
    int have = 0;
    keep = std::min(keep, 8);
    __m128i pm[16][4];
    __m128i total[4] = {_mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128(), _mm_setzero_si128()};
    for (int i = 0; i < 16; i++) {
        Moments m{};
        for (int c = 0; c < channels; c++) {
            m.v[c] = px[i][c];
            for (int d = c; d < channels; d++) m.v[kSq[c][d]] = px[i][c] * px[i][d];
        }
        m.v[kCount] = 1;
        for (int q = 0; q < 4; q++) {
            pm[i][q] = _mm_load_si128(reinterpret_cast<const __m128i*>(m.v + 4 * q));
            total[q] = _mm_add_epi32(total[q], pm[i][q]);
        }
    }
    const int partitions = 1 << kModes[ns == 2 ? 1 : 2].pb;
    for (int p = 0; p < partitions; p++) {
        const uint8_t* subset_of = ns == 2 ? kPartition2[p] : kPartition3[p];
        __m128i acc[2][4] = {};
        for (int i = 0; i < 16; i++)
            for (int s = 0; s < ns - 1; s++) {
                const __m128i mask = _mm_set1_epi32(-static_cast<int>(subset_of[i] == s));
                for (int q = 0; q < 4; q++) acc[s][q] = _mm_add_epi32(acc[s][q], _mm_and_si128(mask, pm[i][q]));
            }
        Moments m[3];
        for (int q = 0; q < 4; q++) {
            __m128i last = total[q];
            for (int s = 0; s < ns - 1; s++) {
                _mm_store_si128(reinterpret_cast<__m128i*>(m[s].v + 4 * q), acc[s][q]);
                last = _mm_sub_epi32(last, acc[s][q]);
            }
            _mm_store_si128(reinterpret_cast<__m128i*>(m[ns - 1].v + 4 * q), last);
        }
        // The iteration's lambda is a Rayleigh quotient, at most the largest eigenvalue and so at most the Frobenius
        // norm: trace - norm bounds a subset's estimate from below. A partition whose bound already reaches the
        // current last place cannot enter the list, so its iterations are skipped; the margin covers rounding. (An
        // iteration that stops at its second step keeps |C v0| for an unnormalized v0, but |C v1| >= |C v0| / |v0| for
        // symmetric C, so that stop means lambda < 1e-9 |v0| <= 1e-9 norm.)
        Covariance cov[3];
        bool used[3];
        double bound = 0, scale = 1;
        for (int s = 0; s < ns; s++) {
            used[s] = subset_covariance(m[s], channels, cov[s]);
            if (used[s]) { bound += std::max(cov[s].trace - cov[s].frobenius, 0.0); scale += cov[s].trace; }
        }
        if (have == keep && bound - 1e-9 * scale >= top[have - 1].score) continue;
        double score = 0;
        for (int s = 0; s < ns; s++) if (used[s]) score += subset_estimate(cov[s], channels);
        if (have < keep || score < top[have - 1].score) {
            int at = have < keep ? have++ : have - 1;
            top[at] = {score, p};
            while (at > 0 && top[at].score < top[at - 1].score) { std::swap(top[at], top[at - 1]); at--; }
        }
    }
    for (int i = 0; i < have; i++) out[i] = top[i].partition;
    return have;
}

// Fields are written LSB first into a little-endian 128-bit block; no field is wider than 8 bits.
struct BitWriter {
    uint64_t w[2] = {};
    int pos = 0;
    void put(uint32_t v, int n) {
        const uint64_t x = v & ((1u << n) - 1);
        if (pos >= 64) w[1] |= x << (pos - 64);
        else {
            w[0] |= x << pos;
            if (pos + n > 64) w[1] |= x >> (64 - pos);
        }
        pos += n;
    }
};

void pack(const Candidate& c, uint8_t* out) {
    const Mode& m = kModes[c.mode];
    BitWriter w;
    w.put(1u << c.mode, c.mode + 1);
    w.put(static_cast<uint32_t>(c.partition), m.pb);
    for (int ch = 0; ch < 3; ch++)
        for (int s = 0; s < m.ns; s++) { w.put(c.ep[s].q0[ch], m.cb); w.put(c.ep[s].q1[ch], m.cb); }
    if (m.ab)
        for (int s = 0; s < m.ns; s++) { w.put(c.ep[s].q0[3], m.ab); w.put(c.ep[s].q1[3], m.ab); }
    if (m.pk == 1)
        for (int s = 0; s < m.ns; s++) { w.put(c.ep[s].p0, 1); w.put(c.ep[s].p1, 1); }
    else if (m.pk == 2)
        for (int s = 0; s < m.ns; s++) w.put(c.ep[s].p0, 1);
    for (int i = 0; i < 16; i++) {
        bool anchor = i == 0;
        for (int s = 1; s < m.ns; s++) anchor |= i == anchor_of(m, c.partition, s);
        w.put(c.idx[i], m.ib - (anchor ? 1 : 0));
    }
    static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "BitWriter stores its words little-endian");
    std::memcpy(out, w.w, 16);
}

}  // namespace

void bc7_encode_block(const uint8_t* rgba, const Bc7Options& o, uint8_t* block) {
    int px[16][4];
    bool opaque = true;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 4; c++) px[i][c] = rgba[i * 4 + c];
        opaque &= px[i][3] == 255;
    }
    const int channels = opaque ? 3 : 4;
    Candidate best;
    try_mode(6, 0, px, channels, o.refine_rounds, best);
    if (best.err > o.accept_error * 16 * channels) {
        int ranked[8];
        const int n2 = o.partitions2 > 0 ? rank_partitions(px, channels, 2, o.partitions2, ranked) : 0;
        for (int i = 0; i < n2; i++) {
            if (opaque) {
                try_mode(1, ranked[i], px, 3, o.refine_rounds, best);
                try_mode(3, ranked[i], px, 3, o.refine_rounds, best);
            } else {
                try_mode(7, ranked[i], px, 4, o.refine_rounds, best);
            }
        }
        if (opaque && o.partitions3 > 0) {
            const int n3 = rank_partitions(px, 3, 3, o.partitions3, ranked);
            for (int i = 0; i < n3; i++) {
                try_mode(0, ranked[i] & 15, px, 3, o.refine_rounds, best);  // mode 0 has 16 partitions
                try_mode(2, ranked[i], px, 3, o.refine_rounds, best);
            }
        }
    }
    pack(best, block);
}

}  // namespace arm64emu::texcodec
