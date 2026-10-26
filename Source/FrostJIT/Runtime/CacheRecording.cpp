// CacheRecording.cpp — Tier-1 translations that later processes reuse
// (BIFROST_JIT_DISK_CACHE; storage in PersistentCache.cpp).
//
// A recording compile (recording_) is an ordinary public Tier-1 compile of
// code mapped from a file. Its 64-bit immediates are logged, the sites that
// embed host addresses name them (note_host_reloc) and every constant a
// 32-bit form could hold takes its imm64 form (wide_guest_pc). Before publication the block is compiled a second
// time with every guest PC shifted by a page-aligned bias (record_oracle_:
// fetches subtract the bias, nothing is published, everything it appended
// is removed again). Outside the logged fields both compiles must emit the
// same bytes, and every field must be a constant, a declared host address,
// a guest PC (it moved by the bias) or a code address (it moved by the
// distance between the two copies). Edges, fault sites, instruction words
// and dead-flag dependencies must agree up to the bias as well. Anything
// else leaves the block unrecorded: a missed dependency costs a cache
// entry, never a wrong translation in a later process.
//
// A later process finds the record by file and offset, checks the words
// (and dependency words) now at that address, relocates it to the address
// the file is mapped at now and publishes it like a fresh compile.
#include "FrostJIT/Compiler/FrostJIT.hpp"
#include "FrostJIT/Runtime/PersistentCache.hpp"
#include "System/Core/DebugFlags.hpp"
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <link.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
namespace arm64emu {
extern "C" void jit_interp_step(Emulator* emu, CPU* cpu);
namespace {
// The guest PC bias of the proof compile: page aligned, moves values across
// the 4 GiB boundary and mixes bits so a non-linear use of a PC shows.
constexpr uint64_t kOracleBias = 0x1234567000ULL;

// Why a block stays unrecorded (PersistentCache::unrecordable_by).
enum RecordFailure : int {
    kRecordOk = -1, kFailCapture = 0, kFailSpan, kFailBias, kFailShape, kFailField, kFailHostAddress,
    kFailBytes, kFailEdges, kFailFaults, kFailDeps, kFailEntry, kFailOther,
};
enum class Reloc : uint8_t { GuestPc = 1, CodeAbs, Image, JitObject, Window, PageFlags, Edge };

struct NativeBody {
    uint32_t code_bytes, reloc_count, edge_count, fault_count, word_count, dep_count, dep_words;
    uint32_t chain_entry_off, chain_patch_off, taken_chain_patch_off, selfloop_patch_off;
    int32_t instr_count, call_interp_count;
    uint32_t flags;
    int64_t chain_target_rel, taken_target_rel;
};
struct RelocRecord { uint32_t offset; uint8_t kind, pad[3]; uint32_t edge, pad2; int64_t value; };
struct EdgeRecord { uint32_t slot_off, cold_off; int64_t target_rel; uint8_t cold_entry, pad[7]; };
struct FaultRecord { uint32_t site_off, stub_off; };
struct DepRecord { int64_t pc_rel; uint32_t count, pad; };
static_assert(sizeof(RelocRecord) == 24 && sizeof(EdgeRecord) == 24 && sizeof(DepRecord) == 16);
constexpr uint32_t kEndsWithBranch = 1, kHasSvc = 2, kHasCall = 4, kHasSelfloop = 8, kHasTakenSlot = 16,
    kVerifiedOnce = 32, kReadsPstate = 64, kSkipsFlagStore = 128, kHasChainTarget = 256, kHasTakenTarget = 512;

// The object holding the JIT: base, extent and GNU build-id.
struct Image { uintptr_t base = 0, lo = 0, hi = 0; std::string build_id; };
const Image& own_image() {
    static const Image image = [] {
        Image out;
        const uintptr_t probe = reinterpret_cast<uintptr_t>(&jit_interp_step);
        struct Ctx { uintptr_t probe; Image* out; } ctx{probe, &out};
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
            auto* c = static_cast<Ctx*>(data);
            uintptr_t lo = UINTPTR_MAX, hi = 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD) continue;
                lo = std::min<uintptr_t>(lo, info->dlpi_addr + ph.p_vaddr);
                hi = std::max<uintptr_t>(hi, info->dlpi_addr + ph.p_vaddr + ph.p_memsz);
            }
            if (c->probe < lo || c->probe >= hi) return 0;
            c->out->base = info->dlpi_addr; c->out->lo = lo; c->out->hi = hi;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
                const auto* end = p + ph.p_memsz;
                while (p + 12 <= end) {
                    uint32_t namesz, descsz, type;
                    memcpy(&namesz, p, 4); memcpy(&descsz, p + 4, 4); memcpy(&type, p + 8, 4);
                    const uint8_t* name = p + 12;
                    const uint8_t* desc = name + ((namesz + 3) & ~3u);
                    if (desc + descsz > end) break;
                    if (type == 3 /* NT_GNU_BUILD_ID */ && namesz == 4 && !memcmp(name, "GNU", 4))
                        c->out->build_id.assign(reinterpret_cast<const char*>(desc), descsz);
                    p = desc + ((descsz + 3) & ~3u);
                }
            }
            return 1;
        }, &ctx);
        return out;
    }();
    return image;
}

// Every BIFROST_* switch the JIT or DebugFlags reads, listed by the build
// from the sources (the Makefile's JIT_SWITCHES rule). Their values are part
// of the codegen fingerprint: a switch that changes generated code then
// changes the cache directory; one that does not merely costs a separate one.
constexpr const char* kCodegenSwitches[] = {
#include "JitCodegenSwitches.inc"
};

// Whether a value addresses a page this process has mapped. Both compiles of
// a block run in one process, so a heap or shared-library address that no
// emitter declared (note_host_reloc) looks like a constant to build_record;
// stored, it would point at something else in a later process.
bool host_mapped(uint64_t v) {
    static const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    if (v < 0x10000 || v >= (1ull << 57)) return false;  // below mmap_min_addr, above any user address
    unsigned char resident;
    if (mincore(reinterpret_cast<void*>(v & ~(page - 1)), 1, &resident) == 0) return true;
    return errno != ENOMEM;  // anything but "not mapped" counts as mapped
}

struct Hash128 {
    uint64_t a = 0xcbf29ce484222325ULL, b = 0x84222325cbf29ce4ULL;
    void bytes(const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { a = (a ^ p[i]) * 0x100000001b3ULL; b = (b ^ p[i]) * 0x1000193ULL + 0x9e37; }
    }
    void str(const std::string& s) { const uint64_t n = s.size(); bytes(&n, 8); bytes(s.data(), s.size()); }
    void u64(uint64_t v) { bytes(&v, 8); }
};

template <class T> void put(std::vector<uint8_t>& out, const T& v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}
} // namespace

// ── Emitter hooks ──────────────────────────────────────────────────────
void FrostJIT::note_host_reloc(HostReloc kind, uint32_t edge) {
    if (!recording_) return;
    if (imm64_fields_.empty() || imm64_fields_.back().offset + 8 != code_buf_used_) {
        record_unrecordable_ = true;  // not the field just emitted (overflow, misuse)
        return;
    }
    imm64_fields_.back().kind = kind;
    imm64_fields_.back().edge = edge;
}
bool FrostJIT::wide_guest_pc(uint64_t value) const {
    // Every value a 32-bit form could hold may be a guest PC of a block mapped
    // below 4 GiB, so all of them take the imm64 form; larger values have no
    // shorter form. The choice depends on the value alone: the same code
    // records the same bytes wherever a process maps it (verify compares
    // records across processes), and a constant costs only a longer encoding.
    return recording_ && value >= 0x10000 && value <= 0xFFFFFFFFull;
}

// ── Setup ──────────────────────────────────────────────────────────────
bool FrostJIT::disk_cache_ready(Emulator& emu) {
    if (disk_cache_checked_) return disk_cache_ != nullptr;
    disk_cache_checked_ = true;
    if (PersistentCache::configured_mode() == PersistentCache::Mode::Off) return false;
    // Translations these switches shape carry state no record describes.
    static const bool leaf_inline = getenv("BIFROST_ENABLE_LEAF_INLINE") != nullptr;
    static const bool malloc_interp = getenv("BIFROST_MALLOC_INTERP") != nullptr;
    const char* why = tier2_enabled() ? "Tier-2 is enabled" : dbg().jit_verify ? "BIFROST_JIT_VERIFY is set"
        : leaf_inline ? "leaf inlining is enabled" : malloc_interp ? "BIFROST_MALLOC_INTERP is set"
        : !dbg().block_trace.empty() ? "BIFROST_BLOCK_TRACE is set" : !guest_memory_ || !window_base_
        ? "no guest window" : own_image().hi == 0 ? "the JIT's own image is unknown" : nullptr;
    if (why) {
        fprintf(stderr, "[JIT] disk cache: off (%s)\n", why);
        return false;
    }
    Hash128 h;
    const Image& image = own_image();
    if (!image.build_id.empty()) h.str(image.build_id);
    else {  // no build-id note: the image's own bytes identify the build
        h.u64(image.hi - image.lo);
        h.bytes(reinterpret_cast<const void*>(image.lo), std::min<uintptr_t>(image.hi - image.lo, 64u << 20));
    }
    h.bytes(&cpu_features_, sizeof cpu_features_);
    h.u64(guest_memory_->window_size());
    h.u64((chain_skip_enabled() ? 1 : 0) | (direct_call_enabled() ? 2 : 0) | (wex_enabled_ ? 4 : 0) |
          (no_fma3_ ? 8 : 0) | (no_avx2_ ? 16 : 0));
    h.u64(CODE_BUF_SIZE);
    for (const char* name : kCodegenSwitches) {
        const char* v = getenv(name);
        h.str(name); h.str(v ? std::string("=") + v : std::string());
    }
    disk_cache_ = PersistentCache::open(h.a, h.b, emu.elf_path());
    return disk_cache_ != nullptr;
}

// ── Recording ──────────────────────────────────────────────────────────
void FrostJIT::start_record_compile() {
    imm64_fields_.clear();
    record_edge_mark_ = mt_edges_.size();
    record_fault_mark_ = fault_site_count_.load(std::memory_order_relaxed);
    record_flagdep_mark_ = flag_dep_table_.size();
    record_call_end_ = call_site_end_;
    record_unrecordable_ = false;
}

// The proof compile's guest PC shift; 0 when the shifted block would leave the window.
static uint64_t oracle_bias(uint64_t start_pc, uint64_t window) {
    if (start_pc + kOracleBias + (1ull << 32) < window) return kOracleBias;
    return start_pc >= kOracleBias + (1ull << 32) ? 0 - kOracleBias : 0;
}

bool FrostJIT::begin_recording(uint64_t start_pc) {
    Memory::CodeOrigin origin;
    if (!guest_memory_->code_origin(start_pc, origin)) return false;
    record_bias_ = oracle_bias(start_pc, guest_memory_->window_size());
    record_origin_.file_key = origin.file_key;
    record_origin_.file_offset = origin.file_offset;
    recording_ = true;
    start_record_compile();
    return true;
}

void FrostJIT::capture_compile(CompileCapture& c, uint64_t start_pc, size_t block_start,
                               const BlockEntry& entry, bool inlined_leaf) {
    c.ok = !record_unrecordable_ && !inlined_leaf && !code_buf_overflow_ && call_site_end_ == record_call_end_ &&
           entry.fn && !entry.interp_only && entry.inlined_ranges_.empty() && entry.tier2_counter_len == 0;
    c.start_pc = start_pc;
    c.block_start = block_start;
    c.code.assign(code_buf_ + block_start, code_buf_ + code_buf_used_);
    c.fields.clear();
    for (const Imm64Field& f : imm64_fields_) {
        if (f.offset < block_start || f.offset + 8 > code_buf_used_) continue;
        // An edge field must hold the cell of the edge it names.
        if (f.kind == HostReloc::Edge && (record_edge_mark_ + f.edge >= mt_edges_.size() ||
            f.value != reinterpret_cast<uint64_t>(&mt_edges_[record_edge_mark_ + f.edge]->target)))
            c.ok = false;
        c.fields.push_back({static_cast<uint32_t>(f.offset - block_start), f.value, f.kind, f.edge});
    }
    c.edges.clear();
    for (size_t i = record_edge_mark_; i < mt_edges_.size(); ++i) {
        const MtEdge& e = *mt_edges_[i];
        const uint64_t cold = e.target.cold;
        const uint64_t base = reinterpret_cast<uint64_t>(code_buf_) + block_start;
        // Only the block's own chain slots: self-loop and call cells keep
        // state (a body address, a callee) that a record does not carry.
        if (e.internal_loop || e.sampling_private || (e.slot != entry.chain_patch_off &&
            !(entry.has_taken_chain_slot && e.slot == entry.taken_chain_patch_off)) ||
            cold < base || cold - base > c.code.size())
            c.ok = false;
        c.edges.push_back({static_cast<uint32_t>(e.slot - block_start), static_cast<uint32_t>(cold - base),
                           e.target_pc, e.cold_entry});
    }
    c.faults.clear();
    const size_t faults = fault_site_count_.load(std::memory_order_relaxed);
    for (size_t i = record_fault_mark_; i < faults; ++i) {
        const uint64_t v = fault_sites_[i].load(std::memory_order_relaxed);
        const uint64_t site = v >> 32, stub = v & 0xffffffffu;
        if (site < block_start || stub < block_start) c.ok = false;
        c.faults.push_back({static_cast<uint32_t>(site - block_start), static_cast<uint32_t>(stub - block_start)});
    }
    c.words = block_words_;
    c.deps.clear();
    for (const auto& d : flag_deps_scratch_) c.deps.push_back({d.pc, d.words});
    if (c.words.size() != static_cast<size_t>(entry.instr_count)) c.ok = false;
    c.entry = entry;
}

// Removes what the proof compile appended; its code is rewound by the caller.
void FrostJIT::discard_oracle_effects(size_t edge_mark, size_t flagdep_mark) {
    for (size_t i = edge_mark; i < mt_edges_.size(); ++i) {
        MtEdge* edge = mt_edges_[i].get();
        for (auto* refs : {&mt_edge_refs_, &mt_source_refs_}) {
            auto it = refs->find(refs == &mt_edge_refs_ ? edge->target_pc : edge->source_pc);
            if (it == refs->end()) continue;
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), edge), v.end());
            if (v.empty()) refs->erase(it);
        }
    }
    mt_edges_.resize(edge_mark);
    flag_dep_table_.resize(flagdep_mark);
}

// Compares the two compiles and, if every difference is explained, builds the record.
int FrostJIT::build_record(const CompileCapture& a, const CompileCapture& b, std::vector<uint8_t>& out) {
    if (!a.ok || !b.ok) return kFailCapture;
    if (a.code.size() != b.code.size() || a.fields.size() != b.fields.size() || a.edges.size() != b.edges.size() ||
        a.faults.size() != b.faults.size() || a.words != b.words || a.deps.size() != b.deps.size())
        return kFailShape;
    const uint64_t bias = b.start_pc - a.start_pc;
    const uint64_t code_delta = b.block_start - a.block_start;
    const Image& image = own_image();
    const uint64_t self = reinterpret_cast<uint64_t>(this);
    const uint64_t window = reinterpret_cast<uint64_t>(window_base_);
    const uint64_t window_size = guest_memory_->window_size();
    const uint64_t flags = reinterpret_cast<uint64_t>(guest_memory_->direct_page_flags());
    const uint64_t code = reinterpret_cast<uint64_t>(code_buf_);
    auto host_address = [&](uint64_t v) {
        return (v >= image.lo && v < image.hi) || (v >= self && v < self + sizeof(*this)) ||
               // The JIT embeds no address inside guest memory but the base; window
               // limits (size - width) are constants that may fall in its range.
               (v + (64u << 10) >= window && v - window + (64u << 10) < (128u << 10)) ||
               (v >= flags && v - flags < window_size / Memory::PAGE_SIZE) ||
               (v >= code && v - code < code_buf_limit_);
    };
    std::vector<RelocRecord> relocs;
    std::vector<bool> masked(a.code.size(), false);
    for (size_t i = 0; i < a.fields.size(); ++i) {
        const auto& fa = a.fields[i];
        const auto& fb = b.fields[i];
        if (fa.offset != fb.offset || fa.kind != fb.kind) return kFailField;
        for (unsigned k = 0; k < 8; ++k) masked[fa.offset + k] = true;
        RelocRecord r{};
        r.offset = fa.offset;
        switch (fa.kind) {
            case HostReloc::None:
                if (fa.value == fb.value) {
                    // The guest window's reservation is skipped: the JIT embeds no
                    // address inside it but its base (host_address), and window limits
                    // (size - width) are constants that can fall into its range.
                    const bool in_window = fa.value >= window && fa.value - window < window_size;
                    if (host_address(fa.value) || (!in_window && host_mapped(fa.value)))
                        return kFailHostAddress;  // an undeclared host address
                    for (unsigned k = 0; k < 8; ++k) masked[fa.offset + k] = false;
                    continue;  // a constant: compared as code bytes
                }
                if (fb.value - fa.value == bias) { r.kind = uint8_t(Reloc::GuestPc); r.value = int64_t(fa.value - a.start_pc); }
                else if (fb.value - fa.value == code_delta) {
                    r.kind = uint8_t(Reloc::CodeAbs);
                    r.value = int64_t(fa.value - (code + a.block_start));
                } else return kFailField;
                break;
            case HostReloc::Edge:
                if (fa.edge != fb.edge || fa.edge >= a.edges.size()) return kFailEdges;
                r.kind = uint8_t(Reloc::Edge); r.edge = fa.edge;
                break;
            default: {
                if (fa.value != fb.value) return kFailField;
                // A declared address must lie in what it names: an image
                // relocation to another library would move with the wrong base.
                const uint64_t v = fa.value;
                uint64_t base = 0;
                bool inside = false;
                switch (fa.kind) {
                    case HostReloc::Image:
                        r.kind = uint8_t(Reloc::Image); base = image.base; inside = v >= image.lo && v < image.hi; break;
                    case HostReloc::JitObject:
                        r.kind = uint8_t(Reloc::JitObject); base = self; inside = v >= self && v - self < sizeof(*this); break;
                    case HostReloc::Window:
                        r.kind = uint8_t(Reloc::Window); base = window; inside = v == window; break;
                    case HostReloc::PageFlags:
                        r.kind = uint8_t(Reloc::PageFlags); base = flags;
                        inside = v >= flags && v - flags < window_size / Memory::PAGE_SIZE; break;
                    default: return kFailOther;
                }
                if (!inside) return kFailHostAddress;
                r.value = int64_t(v - base);
            }
        }
        relocs.push_back(r);
    }
    for (size_t i = 0; i < a.code.size(); ++i)
        if (!masked[i] && a.code[i] != b.code[i]) return kFailBytes;
    std::vector<EdgeRecord> edges;
    for (size_t i = 0; i < a.edges.size(); ++i) {
        const auto& ea = a.edges[i];
        const auto& eb = b.edges[i];
        if (ea.slot_off != eb.slot_off || ea.cold_off != eb.cold_off || ea.cold_entry != eb.cold_entry ||
            eb.target_pc - ea.target_pc != bias) return kFailEdges;
        edges.push_back({ea.slot_off, ea.cold_off, int64_t(ea.target_pc - a.start_pc), uint8_t(ea.cold_entry), {}});
    }
    for (size_t i = 0; i < a.faults.size(); ++i)
        if (a.faults[i].first != b.faults[i].first || a.faults[i].second != b.faults[i].second) return kFailFaults;
    for (size_t i = 0; i < a.deps.size(); ++i)
        if (b.deps[i].first - a.deps[i].first != bias || a.deps[i].second != b.deps[i].second) return kFailDeps;
    const BlockEntry& ea = a.entry;
    const BlockEntry& eb = b.entry;
    const uint64_t ba = code + a.block_start, bb = code + b.block_start;
    auto rel = [](const void* p, uint64_t base) { return p ? uint32_t(reinterpret_cast<uint64_t>(p) - base) : UINT32_MAX; };
    if (rel(reinterpret_cast<const void*>(ea.chain_entry), ba) != rel(reinterpret_cast<const void*>(eb.chain_entry), bb) ||
        ea.chain_patch_off - a.block_start != eb.chain_patch_off - b.block_start ||
        ea.has_taken_chain_slot != eb.has_taken_chain_slot || ea.has_selfloop_slot != eb.has_selfloop_slot ||
        (ea.has_taken_chain_slot && ea.taken_chain_patch_off - a.block_start != eb.taken_chain_patch_off - b.block_start) ||
        (ea.has_selfloop_slot && ea.selfloop_patch_off - a.block_start != eb.selfloop_patch_off - b.block_start) ||
        ea.instr_count != eb.instr_count || ea.call_interp_count != eb.call_interp_count ||
        ea.emitted_bytes != eb.emitted_bytes || ea.ends_with_branch != eb.ends_with_branch ||
        ea.has_svc != eb.has_svc || ea.has_call != eb.has_call || ea.verified_once != eb.verified_once ||
        ea.reads_pstate_before_set != eb.reads_pstate_before_set || ea.skips_dead_flag_store != eb.skips_dead_flag_store ||
        (ea.chain_target_pc == 0) != (eb.chain_target_pc == 0) ||
        (ea.chain_target_pc && eb.chain_target_pc - ea.chain_target_pc != bias) ||
        (ea.taken_chain_target_pc == 0) != (eb.taken_chain_target_pc == 0) ||
        (ea.taken_chain_target_pc && eb.taken_chain_target_pc - ea.taken_chain_target_pc != bias))
        return kFailEntry;
    NativeBody body{};
    body.code_bytes = uint32_t(a.code.size());
    body.reloc_count = uint32_t(relocs.size());
    body.edge_count = uint32_t(edges.size());
    body.fault_count = uint32_t(a.faults.size());
    body.word_count = uint32_t(a.words.size());
    body.dep_count = uint32_t(a.deps.size());
    for (const auto& d : a.deps) body.dep_words += uint32_t(d.second.size());
    body.chain_entry_off = rel(reinterpret_cast<const void*>(ea.chain_entry), ba);
    body.chain_patch_off = uint32_t(ea.chain_patch_off - a.block_start);
    body.taken_chain_patch_off = ea.has_taken_chain_slot ? uint32_t(ea.taken_chain_patch_off - a.block_start) : 0;
    body.selfloop_patch_off = ea.has_selfloop_slot ? uint32_t(ea.selfloop_patch_off - a.block_start) : 0;
    body.instr_count = ea.instr_count;
    body.call_interp_count = ea.call_interp_count;
    body.flags = (ea.ends_with_branch ? kEndsWithBranch : 0) | (ea.has_svc ? kHasSvc : 0) | (ea.has_call ? kHasCall : 0) |
                 (ea.has_selfloop_slot ? kHasSelfloop : 0) | (ea.has_taken_chain_slot ? kHasTakenSlot : 0) |
                 (ea.verified_once ? kVerifiedOnce : 0) | (ea.reads_pstate_before_set ? kReadsPstate : 0) |
                 (ea.skips_dead_flag_store ? kSkipsFlagStore : 0) | (ea.chain_target_pc ? kHasChainTarget : 0) |
                 (ea.taken_chain_target_pc ? kHasTakenTarget : 0);
    body.chain_target_rel = ea.chain_target_pc ? int64_t(ea.chain_target_pc - a.start_pc) : 0;
    body.taken_target_rel = ea.taken_chain_target_pc ? int64_t(ea.taken_chain_target_pc - a.start_pc) : 0;
    out.clear();
    CacheRecordHeader h{};
    h.file_key = record_origin_.file_key;
    h.file_offset = record_origin_.file_offset;
    h.start_pc = a.start_pc;
    h.mt = mt_active() ? 1 : 0;
    h.kind = kRecordNative;
    put(out, h);
    put(out, body);
    for (const auto& r : relocs) put(out, r);
    for (const auto& e : edges) put(out, e);
    for (const auto& f : a.faults) put(out, FaultRecord{f.first, f.second});
    for (uint32_t w : a.words) put(out, w);
    while (out.size() % 8) out.push_back(0);
    for (const auto& d : a.deps) put(out, DepRecord{int64_t(d.first - a.start_pc), uint32_t(d.second.size()), 0});
    for (const auto& d : a.deps) for (uint32_t w : d.second) put(out, w);
    out.insert(out.end(), a.code.begin(), a.code.end());
    // Canonical code: relocated fields hold no process-specific value.
    uint8_t* code_out = out.data() + out.size() - a.code.size();
    for (const auto& r : relocs) memset(code_out + r.offset, 0, 8);
    while (out.size() % 8) out.push_back(0);
    return kRecordOk;
}

void FrostJIT::finish_recording(Emulator& emu, uint64_t start_pc, size_t block_start,
                                const BlockEntry& entry, bool inlined_leaf) {
    struct Clear { FrostJIT* j; ~Clear() { j->recording_ = false; j->imm64_fields_.clear(); } } clear{this};
    CompileCapture a;
    capture_compile(a, start_pc, block_start, entry, inlined_leaf);
    // The whole block must lie in one contiguous run of the same file.
    Memory::CodeOrigin last;
    const uint64_t span = static_cast<uint64_t>(std::max(entry.instr_count, 1) - 1) * 4;
    auto fail = [&](int why) {
        disk_cache_->unrecordable.fetch_add(1, std::memory_order_relaxed);
        disk_cache_->unrecordable_by[why].fetch_add(1, std::memory_order_relaxed);
    };
    if (!a.ok) return fail(kFailCapture);
    if (!guest_memory_->code_origin(start_pc + span, last) || last.file_key != record_origin_.file_key ||
        last.file_offset != record_origin_.file_offset + span)
        return fail(kFailSpan);
    // The proof compile at shifted guest PCs, which must stay inside the window.
    const uint64_t bias = record_bias_;
    if (bias == 0) return fail(kFailBias);
    CompileCapture b;
    const auto saved_profile = block_profile_snapshot();
    const bool saved_exhausted = code_budget_exhausted_;
    const size_t edge_mark = mt_edges_.size(), flagdep_mark = flag_dep_table_.size(), code_mark = code_buf_used_;
    const uint64_t saved_start = current_start_pc_;
    oracle_out_ = &b;
    record_oracle_ = true;
    compile_pc_bias_ = bias;
    start_record_compile();
    translate_block(emu, start_pc + bias);
    record_oracle_ = false;
    compile_pc_bias_ = 0;
    oracle_out_ = nullptr;
    rewind_codegen(code_mark);
    code_buf_overflow_ = false;
    code_budget_exhausted_ = saved_exhausted;
    discard_oracle_effects(edge_mark, flagdep_mark);
    block_profile_restore(saved_profile);
    current_start_pc_ = saved_start;
    std::vector<uint8_t> record;
    if (const int why = build_record(a, b, record); why != kRecordOk) return fail(why);
    // verify keeps a record that matches; it replaces one that does not.
    if (disk_cache_->mode() == PersistentCache::Mode::Verify && verify_against_store(start_pc, record)) return;
    disk_cache_->submit(std::move(record));
}

// verify: a stored record for this code must equal the fresh one (canonical
// form, so every relocated field is compared by what it refers to).
bool FrostJIT::verify_against_store(uint64_t start_pc, const std::vector<uint8_t>& fresh) {
    const CacheRecordHeader* stored = disk_cache_->find(record_origin_.file_key, record_origin_.file_offset, mt_active());
    if (!stored || stored->kind != kRecordNative || !PersistentCache::valid(stored)) return false;
    const size_t body = sizeof(CacheRecordHeader);
    if (stored->size == fresh.size() &&
        !memcmp(reinterpret_cast<const uint8_t*>(stored) + body, fresh.data() + body, fresh.size() - body)) {
        disk_cache_->hits.fetch_add(1, std::memory_order_relaxed);
        disk_cache_->note_use(stored);
        return true;
    }
    NativeBody a{}, b{};
    if (stored->size >= sizeof(CacheRecordHeader) + sizeof a)
        memcpy(&a, reinterpret_cast<const uint8_t*>(stored) + body, sizeof a);
    memcpy(&b, fresh.data() + body, sizeof b);
    // A block ends at the first already-known entry, so translations of the
    // same address can cover different instruction runs (compile order); both
    // are valid. Only translations of the same run must be identical.
    if (a.instr_count != b.instr_count) {
        disk_cache_->verify_other_shape.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (disk_cache_->verify_mismatches.fetch_add(1, std::memory_order_relaxed) < 20)
        fprintf(stderr, "[JIT] disk cache verify: record for pc=0x%llx (file offset 0x%llx) differs from a fresh compile "
                "(stored %d instrs/%u bytes, fresh %d instrs/%u bytes)\n",
                (unsigned long long)start_pc, (unsigned long long)record_origin_.file_offset,
                a.instr_count, a.code_bytes, b.instr_count, b.code_bytes);
    return false;
}

// ── Install ────────────────────────────────────────────────────────────
uint64_t (*FrostJIT::install_cached(Emulator& emu, uint64_t start_pc))(CPU*, Emulator*) {
    (void)emu;
    if (disk_cache_->mode() != PersistentCache::Mode::On) return nullptr;
    Memory::CodeOrigin origin;
    if (!guest_memory_->code_origin(start_pc, origin)) return nullptr;
    const CacheRecordHeader* h = disk_cache_->find(origin.file_key, origin.file_offset, mt_active());
    if (!h || h->kind != kRecordNative) {
        disk_cache_->misses.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (!PersistentCache::valid(h)) {  // damaged on disk
        disk_cache_->rejected.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(h) + sizeof *h;
    const uint8_t* end = reinterpret_cast<const uint8_t*>(h) + h->size;
    NativeBody body;
    if (p + sizeof body > end) { disk_cache_->rejected.fetch_add(1); return nullptr; }
    memcpy(&body, p, sizeof body); p += sizeof body;
    const uint8_t* relocs = p; p += size_t(body.reloc_count) * sizeof(RelocRecord);
    const uint8_t* edges = p; p += size_t(body.edge_count) * sizeof(EdgeRecord);
    const uint8_t* faults = p; p += size_t(body.fault_count) * sizeof(FaultRecord);
    const uint8_t* words = p; p += size_t(body.word_count) * 4;
    p = reinterpret_cast<const uint8_t*>(h) + ((p - reinterpret_cast<const uint8_t*>(h) + 7) & ~size_t{7});
    const uint8_t* deps = p; p += size_t(body.dep_count) * sizeof(DepRecord);
    const uint8_t* dep_words = p; p += size_t(body.dep_words) * 4;
    const uint8_t* code = p; p += body.code_bytes;
    uint64_t listed_dep_words = 0;  // the dependency streams must exactly fill their words
    for (uint32_t i = 0; p <= end && i < body.dep_count; ++i) {
        DepRecord d; memcpy(&d, deps + i * sizeof d, sizeof d);
        listed_dep_words += d.count;
    }
    if (p > end || body.word_count != uint32_t(body.instr_count) || body.instr_count <= 0 || body.code_bytes == 0 ||
        body.dep_count > UINT16_MAX || listed_dep_words != body.dep_words) {
        disk_cache_->rejected.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    // The code at this address must be what was translated, and executable.
    auto words_match = [&](uint64_t pc, const uint8_t* expect, uint32_t count) {
        const uint64_t bytes = uint64_t(count) * 4, window = guest_memory_->window_size();
        if (!count || (pc & 3) || pc > window || bytes > window - pc) return false;
        const auto* flags = guest_memory_->direct_page_flags();
        constexpr uint8_t need = Memory::GUEST_PAGE_MAPPED | Memory::GUEST_PROT_EXEC;
        for (uint64_t pg = pc / Memory::PAGE_SIZE; pg <= (pc + bytes - 1) / Memory::PAGE_SIZE; ++pg) {
            const uint8_t f = flags[pg].load(std::memory_order_acquire);
            if ((f & need) != need || (f & Memory::GUEST_PAGE_UFFD_MISSING)) return false;
        }
        return memcmp(window_base_ + pc, expect, bytes) == 0;
    };
    bool ok = words_match(start_pc, words, body.word_count);
    const uint8_t* dw = dep_words;
    for (uint32_t i = 0; ok && i < body.dep_count; ++i) {
        DepRecord d; memcpy(&d, deps + i * sizeof d, sizeof d);
        ok = words_match(start_pc + uint64_t(d.pc_rel), dw, d.count);
        dw += size_t(d.count) * 4;
    }
    if (!ok) {
        disk_cache_->rejected.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    if (!ensure_code_space(body.code_bytes) ||
        fault_site_count_.load(std::memory_order_relaxed) + body.fault_count >= code_buf_limit_ / 2)
        return nullptr;
    make_writable();
    const size_t block_start = code_buf_used_;
    uint8_t* dst = code_buf_ + block_start;
    memcpy(dst, code, body.code_bytes);
    code_buf_used_ += body.code_bytes;
    // Chain edges first: their cells are relocation targets.
    current_start_pc_ = start_pc;
    std::vector<MtEdge*> created;
    for (uint32_t i = 0; i < body.edge_count; ++i) {
        EdgeRecord e; memcpy(&e, edges + i * sizeof e, sizeof e);
        MtEdge* edge = create_chain_edge(start_pc + uint64_t(e.target_rel), e.cold_entry != 0, block_start + e.slot_off);
        edge->target.cold = reinterpret_cast<uint64_t>(dst + e.cold_off);
        edge->target.word.store(edge->target.cold, std::memory_order_relaxed);
        created.push_back(edge);
    }
    const Image& image = own_image();
    for (uint32_t i = 0; i < body.reloc_count; ++i) {
        RelocRecord r; memcpy(&r, relocs + i * sizeof r, sizeof r);
        uint64_t v = 0;
        switch (Reloc(r.kind)) {
            case Reloc::GuestPc: v = start_pc + uint64_t(r.value); break;
            case Reloc::CodeAbs: v = reinterpret_cast<uint64_t>(dst) + uint64_t(r.value); break;
            case Reloc::Image: v = image.base + uint64_t(r.value); break;
            case Reloc::JitObject: v = reinterpret_cast<uint64_t>(this) + uint64_t(r.value); break;
            case Reloc::Window: v = reinterpret_cast<uint64_t>(window_base_) + uint64_t(r.value); break;
            case Reloc::PageFlags: v = reinterpret_cast<uint64_t>(guest_memory_->direct_page_flags()) + uint64_t(r.value); break;
            case Reloc::Edge:
                if (r.edge >= created.size()) { ok = false; break; }
                v = reinterpret_cast<uint64_t>(&created[r.edge]->target);
                break;
            default: ok = false;
        }
        if (!ok || r.offset + 8 > body.code_bytes) { ok = false; break; }
        memcpy(dst + r.offset, &v, 8);
    }
    for (uint32_t i = 0; ok && i < body.fault_count; ++i) {
        FaultRecord f; memcpy(&f, faults + i * sizeof f, sizeof f);
        if (f.site_off >= body.code_bytes || f.stub_off >= body.code_bytes) { ok = false; break; }
        append_fault_site(block_start + f.site_off, block_start + f.stub_off);
    }
    if (!ok) {  // a damaged record that passed its checksum: drop what was appended
        rewind_codegen(block_start);
        discard_oracle_effects(mt_edges_.size() - created.size(), flag_dep_table_.size());
        make_executable();
        disk_cache_->rejected.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    BlockEntry entry;
    entry.fn = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(dst);
    if (body.chain_entry_off != UINT32_MAX)
        entry.chain_entry = reinterpret_cast<uint64_t (*)(CPU*, Emulator*)>(dst + body.chain_entry_off);
    entry.emitted_bytes = body.code_bytes;
    entry.chain_patch_off = uint32_t(block_start + body.chain_patch_off);
    entry.chain_target_pc = (body.flags & kHasChainTarget) ? start_pc + uint64_t(body.chain_target_rel) : 0;
    entry.has_taken_chain_slot = body.flags & kHasTakenSlot;
    entry.taken_chain_patch_off = entry.has_taken_chain_slot ? uint32_t(block_start + body.taken_chain_patch_off) : 0;
    entry.taken_chain_target_pc = (body.flags & kHasTakenTarget) ? start_pc + uint64_t(body.taken_target_rel) : 0;
    entry.has_selfloop_slot = body.flags & kHasSelfloop;
    entry.selfloop_patch_off = entry.has_selfloop_slot ? uint32_t(block_start + body.selfloop_patch_off) : 0;
    entry.instr_count = body.instr_count;
    entry.call_interp_count = body.call_interp_count;
    entry.ends_with_branch = body.flags & kEndsWithBranch;
    entry.has_svc = body.flags & kHasSvc;
    entry.has_call = body.flags & kHasCall;
    entry.verified_once = body.flags & kVerifiedOnce;
    entry.reads_pstate_before_set = body.flags & kReadsPstate;
    entry.skips_dead_flag_store = body.flags & kSkipsFlagStore;
    // Instruction words and dead-flag dependencies, laid out as attach_code_words does.
    if (code_words_.size() <= UINT32_MAX - body.word_count - body.dep_words) {
        entry.code_words_off = uint32_t(code_words_.size());
        entry.code_words_count = body.word_count;
        const auto* w = reinterpret_cast<const uint32_t*>(words);
        for (uint32_t i = 0; i < body.word_count; ++i) { uint32_t v; memcpy(&v, w + i, 4); code_words_.push_back(v); }
        if (body.dep_count) {
            entry.flag_deps_off = uint32_t(flag_dep_table_.size());
            entry.flag_deps_count = uint16_t(body.dep_count);
            const uint8_t* dwp = dep_words;
            for (uint32_t i = 0; i < body.dep_count; ++i) {
                DepRecord d; memcpy(&d, deps + i * sizeof d, sizeof d);
                flag_dep_table_.push_back({start_pc + uint64_t(d.pc_rel), d.count, uint32_t(code_words_.size())});
                for (uint32_t k = 0; k < d.count; ++k) { uint32_t v; memcpy(&v, dwp + k * 4, 4); code_words_.push_back(v); }
                dwp += size_t(d.count) * 4;
            }
        }
    } else if (body.dep_count) {
        // Without recorded dependencies the dead-flag exits would go unchecked.
        rewind_codegen(block_start);
        discard_oracle_effects(mt_edges_.size() - created.size(), flag_dep_table_.size());
        make_executable();
        return nullptr;
    }
    disk_cache_->hits.fetch_add(1, std::memory_order_relaxed);
    disk_cache_->note_use(h);
    if (staging_) {
        staged_entry_ = entry;
        staged_ = staged_linked_ = true;
    } else {
        publish_translation(start_pc, entry);
    }
    make_executable();
    return entry.fn;
}

void FrostJIT::append_fault_site(size_t site, size_t stub) {
    const size_t n = fault_site_count_.load(std::memory_order_relaxed);
    const uint64_t entry = (uint64_t(site) << 32) | stub;
    if (n == fault_site_constructed_) {
        new (fault_sites_ + n) std::atomic<uint64_t>(entry);
        ++fault_site_constructed_;
    } else {
        fault_sites_[n].store(entry, std::memory_order_relaxed);
    }
    fault_site_count_.store(n + 1, std::memory_order_release);
}

FrostJIT::BlockProfileCounts FrostJIT::block_profile_snapshot() const {
    return {block_profile.natural_branch.load(), block_profile.entry_point.load(), block_profile.call_interp_cap.load(),
            block_profile.bl_call_cap.load(), block_profile.max_size.load(), block_profile.decode_fail.load(),
            block_profile.interp_only.load(), block_profile.leaf_inlined.load()};
}
void FrostJIT::block_profile_restore(const BlockProfileCounts& c) {
    block_profile.natural_branch = c.v[0]; block_profile.entry_point = c.v[1]; block_profile.call_interp_cap = c.v[2];
    block_profile.bl_call_cap = c.v[3]; block_profile.max_size = c.v[4]; block_profile.decode_fail = c.v[5];
    block_profile.interp_only = c.v[6]; block_profile.leaf_inlined = c.v[7];
}
} // namespace arm64emu
