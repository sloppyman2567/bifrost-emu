// frost_graphics/gl_state.cpp — GLStateTracker implementation.
//
// 1.5.4-alpha: NEW. Maintains a software-side mirror of guest OpenGL
// state so that state queries return consistent results regardless of
// host context state.
#include "frost/gl_state.hpp"
#include "core/cpu.h"
#include <cmath>
#include <cstring>

namespace arm64emu {
namespace {
} // anonymous namespace

GLStateTracker::GLStateTracker() = default;
GLStateTracker::~GLStateTracker() = default;

void GLStateTracker::reset() {
    std::lock_guard<std::mutex> g(mu_);
    enabled_.clear();
    clear_color_[0] = clear_color_[1] = clear_color_[2] = clear_color_[3] = 0;
    current_color_[0] = current_color_[1] = current_color_[2] = current_color_[3] = 0;
    viewport_ = {0, 0, 640, 480};
    line_width_ = 1.0f;
    point_size_ = 1.0f;
    active_texture_.store(GL::TEXTURE0, std::memory_order_relaxed);
    blend_src_rgb_ = GL::SRC_ALPHA;
    blend_dst_rgb_ = GL::ONE_MINUS_SRC_ALPHA;
    blend_src_alpha_ = GL::SRC_ALPHA;
    blend_dst_alpha_ = GL::ONE_MINUS_SRC_ALPHA;
    blend_equation_rgb_ = 0x8006;
    blend_equation_alpha_ = 0x8006;
    depth_func_ = GL::LEQUAL;
    depth_mask_ = true;
    cull_face_mode_ = GL::BACK;
    front_face_ = GL::CCW;
    polygon_mode_ = {GL::FILL, GL::FILL};
    scissor_ = {0, 0, 0, 0};
    stencil_front_ = {};
    stencil_back_ = {};
    current_program_.store(0, std::memory_order_relaxed);
    array_buffer_binding_.store(0, std::memory_order_relaxed);
    element_array_buffer_binding_ = 0;
    draw_indirect_buffer_binding_ = 0;
    texture_bindings_.clear();
    pixel_store_unpack_alignment_ = 4;
    pixel_store_pack_alignment_ = 4;
    depth_range_near_ = 0.0f;
    depth_range_far_ = 1.0f;
    hint_fog_ = 0x1400;
    hint_generate_mipmap_ = 0x1400;
    hint_line_smooth_ = 0x1400;
    hint_polygon_smooth_ = 0x1400;
    hint_texture_compression_ = 0x1400;
}

// ── State setters ────────────────────────────────────────────────────────

void GLStateTracker::set_enabled(uint32_t cap, bool on) {
    std::lock_guard<std::mutex> g(mu_);
    enabled_[cap] = on;
}

void GLStateTracker::set_clear_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lock(mu_);
    clear_color_[0] = r; clear_color_[1] = g;
    clear_color_[2] = b; clear_color_[3] = a;
}

void GLStateTracker::set_viewport(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> g(mu_);
    viewport_ = {x, y, w, h};
}

void GLStateTracker::set_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lock(mu_);
    current_color_[0] = r; current_color_[1] = g;
    current_color_[2] = b; current_color_[3] = a;
}

void GLStateTracker::set_line_width(float w) {
    std::lock_guard<std::mutex> g(mu_);
    line_width_ = w;
}

void GLStateTracker::set_point_size(float s) {
    std::lock_guard<std::mutex> g(mu_);
    point_size_ = s;
}

void GLStateTracker::set_active_texture(uint32_t unit) {
    std::lock_guard<std::mutex> g(mu_);
    active_texture_.store(unit, std::memory_order_relaxed);
}

void GLStateTracker::set_blend_func(uint32_t src, uint32_t dst) {
    std::lock_guard<std::mutex> g(mu_);
    blend_src_rgb_ = src;
    blend_dst_rgb_ = dst;
    blend_src_alpha_ = src;
    blend_dst_alpha_ = dst;
}

void GLStateTracker::set_blend_func_separate(uint32_t srcRGB, uint32_t dstRGB,
                                              uint32_t srcAlpha, uint32_t dstAlpha) {
    std::lock_guard<std::mutex> g(mu_);
    blend_src_rgb_ = srcRGB;
    blend_dst_rgb_ = dstRGB;
    blend_src_alpha_ = srcAlpha;
    blend_dst_alpha_ = dstAlpha;
}

void GLStateTracker::set_depth_func(uint32_t func) {
    std::lock_guard<std::mutex> g(mu_);
    depth_func_ = func;
}

void GLStateTracker::set_depth_mask(bool flag) {
    std::lock_guard<std::mutex> g(mu_);
    depth_mask_ = flag;
}

void GLStateTracker::set_cull_face_mode(uint32_t mode) {
    std::lock_guard<std::mutex> g(mu_);
    cull_face_mode_ = mode;
}

void GLStateTracker::set_front_face(uint32_t mode) {
    std::lock_guard<std::mutex> g(mu_);
    front_face_ = mode;
}

void GLStateTracker::set_polygon_mode(uint32_t face, uint32_t mode) {
    std::lock_guard<std::mutex> g(mu_);
    if (face == GL::FRONT_AND_BACK) {
        polygon_mode_[0] = polygon_mode_[1] = mode;
    } else if (face == GL::FRONT) {
        polygon_mode_[0] = mode;
    } else if (face == GL::BACK) {
        polygon_mode_[1] = mode;
    }
}

void GLStateTracker::set_scissor(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> g(mu_);
    scissor_ = {x, y, w, h};
}

void GLStateTracker::set_stencil_func(uint32_t func, int ref, uint32_t mask) {
    std::lock_guard<std::mutex> g(mu_);
    stencil_front_.func = func;
    stencil_front_.ref = ref;
    stencil_front_.mask = mask;
}

void GLStateTracker::set_stencil_op(uint32_t fail, uint32_t zfail, uint32_t zpass) {
    std::lock_guard<std::mutex> g(mu_);
    stencil_front_.fail = fail;
    stencil_front_.zfail = zfail;
    stencil_front_.zpass = zpass;
}

void GLStateTracker::set_stencil_mask(uint32_t mask) {
    std::lock_guard<std::mutex> g(mu_);
    stencil_front_.write_mask = mask;
}

void GLStateTracker::set_stencil_func_separate(uint32_t face, uint32_t func,
                                                int ref, uint32_t mask) {
    std::lock_guard<std::mutex> g(mu_);
    StencilState& s = (face == GL::BACK) ? stencil_back_ : stencil_front_;
    s.func = func; s.ref = ref; s.mask = mask;
}

void GLStateTracker::set_stencil_op_separate(uint32_t face, uint32_t fail,
                                              uint32_t zfail, uint32_t zpass) {
    std::lock_guard<std::mutex> g(mu_);
    StencilState& s = (face == GL::BACK) ? stencil_back_ : stencil_front_;
    s.fail = fail; s.zfail = zfail; s.zpass = zpass;
}

void GLStateTracker::set_stencil_mask_separate(uint32_t face, uint32_t mask) {
    std::lock_guard<std::mutex> g(mu_);
    StencilState& s = (face == GL::BACK) ? stencil_back_ : stencil_front_;
    s.write_mask = mask;
}

void GLStateTracker::set_program(uint32_t program) {
    std::lock_guard<std::mutex> g(mu_);
    current_program_.store(program, std::memory_order_relaxed);
}

// ── Redundant-bind elision support ────────────────────────────────
// Getters snapshot under one lock each; the dispatch check-then-call
// sequence assumes the single-context game pattern (all GL calls flow
// through dispatch on one context — true for every current target).
uint32_t GLStateTracker::current_program() const {
    std::lock_guard<std::mutex> g(mu_);
    return current_program_.load(std::memory_order_relaxed);
}

uint32_t GLStateTracker::active_texture() const {
    std::lock_guard<std::mutex> g(mu_);
    return active_texture_.load(std::memory_order_relaxed);
}

uint32_t GLStateTracker::texture_binding(uint32_t target) const {
    uint64_t unit = active_texture_.load(std::memory_order_relaxed);
    uint64_t key = (unit << 32) | target;
    if (tex_last_key_.load(std::memory_order_relaxed) == key)
        return tex_last_val_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(mu_);
    uint32_t mkey = (static_cast<uint32_t>(unit) << 16) | (target & 0xFFFF);
    auto it = texture_bindings_.find(mkey);
    uint32_t v = it != texture_bindings_.end() ? it->second : 0;
    tex_last_key_.store(key, std::memory_order_relaxed);
    tex_last_val_.store(v, std::memory_order_relaxed);
    return v;
}

void GLStateTracker::set_vao_binding(uint32_t vao) {
    std::lock_guard<std::mutex> g(mu_);
    vao_binding_.store(vao, std::memory_order_relaxed);
    vao_known_.store(true, std::memory_order_relaxed);
}

uint32_t GLStateTracker::vao_binding(bool& known) const {
    std::lock_guard<std::mutex> g(mu_);
    known = vao_known_.load(std::memory_order_relaxed);
    return vao_binding_.load(std::memory_order_relaxed);
}

// Deleting a bound object unbinds it (→ 0). Walk every unit map entry;
// counts are tiny (units × targets), so the linear scan is fine.
void GLStateTracker::unbind_textures(const uint32_t* names, size_t n) {
    if (!names) return;
    std::lock_guard<std::mutex> g(mu_);
    tex_last_key_.store(~0ULL, std::memory_order_relaxed);
    for (size_t k = 0; k < n; k++) {
        uint32_t dead = names[k];
        if (!dead) continue;
        for (auto& kv : texture_bindings_) {
            if (kv.second == dead) kv.second = 0;
        }
    }
}

void GLStateTracker::clear_texture_bindings() {
    std::lock_guard<std::mutex> g(mu_);
    tex_last_key_.store(~0ULL, std::memory_order_relaxed);
    texture_bindings_.clear();
}

void GLStateTracker::unbind_program(uint32_t prog) {
    if (!prog) return;
    std::lock_guard<std::mutex> g(mu_);
    if (current_program_.load(std::memory_order_relaxed) == prog) current_program_.store(0, std::memory_order_relaxed);
}

void GLStateTracker::unbind_vaos(const uint32_t* names, size_t n) {
    if (!names) return;
    std::lock_guard<std::mutex> g(mu_);
    for (size_t k = 0; k < n; k++) {
        if (names[k] && vao_binding_.load(std::memory_order_relaxed) == names[k]) {
            vao_binding_.store(0, std::memory_order_relaxed);
        }
        if (names[k]) vao_element_.erase(names[k]);
    }
}

void GLStateTracker::mark_vao_unknown() {
    std::lock_guard<std::mutex> g(mu_);
    vao_known_.store(false, std::memory_order_relaxed);
}

void GLStateTracker::set_array_buffer_binding(uint32_t buffer) {
    array_buffer_binding_.store(buffer, std::memory_order_relaxed);
}

void GLStateTracker::set_element_array_buffer_binding(uint32_t buffer) {
    std::lock_guard<std::mutex> g(mu_);
    element_array_buffer_binding_ = buffer;
    if (vao_known_.load(std::memory_order_relaxed))
        vao_element_[vao_binding_.load(std::memory_order_relaxed)] = buffer;
}

void GLStateTracker::set_draw_indirect_buffer_binding(uint32_t buffer) {
    std::lock_guard<std::mutex> g(mu_);
    draw_indirect_buffer_binding_ = buffer;
}

uint32_t GLStateTracker::array_buffer_binding() const {
    return array_buffer_binding_.load(std::memory_order_relaxed);
}

uint32_t GLStateTracker::element_array_buffer_binding() const {
    std::lock_guard<std::mutex> g(mu_);
    return element_array_buffer_binding_;
}

uint32_t GLStateTracker::draw_indirect_buffer_binding() const {
    std::lock_guard<std::mutex> g(mu_);
    return draw_indirect_buffer_binding_;
}

void GLStateTracker::set_buffer_binding(uint32_t target, uint32_t buffer) {
    std::lock_guard<std::mutex> g(mu_);
    buffer_bindings_[target] = buffer;
}

uint32_t GLStateTracker::buffer_binding(uint32_t target) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = buffer_bindings_.find(target);
    return it != buffer_bindings_.end() ? it->second : 0;
}

void GLStateTracker::set_texture_binding(uint32_t target, uint32_t texture) {
    std::lock_guard<std::mutex> g(mu_);
    uint32_t unit = active_texture_.load(std::memory_order_relaxed);
    uint32_t key = (unit << 16) | (target & 0xFFFF);
    texture_bindings_[key] = texture;
    tex_last_key_.store((static_cast<uint64_t>(unit) << 32) | target,
                        std::memory_order_relaxed);
    tex_last_val_.store(texture, std::memory_order_relaxed);
}

void GLStateTracker::set_pixel_store_i(uint32_t pname, int param) {
    std::lock_guard<std::mutex> g(mu_);
    if (pname == 0x0CF5) { // GL_UNPACK_ALIGNMENT
        pixel_store_unpack_alignment_ = param;
    } else if (pname == 0x0D31) { // GL_PACK_ALIGNMENT
        pixel_store_pack_alignment_ = param;
    }
}

void GLStateTracker::set_depth_range(float near_val, float far_val) {
    std::lock_guard<std::mutex> g(mu_);
    // GL clamps depth-range arguments to [0, 1] — mirror the CLAMPED
    // host state (also keeps the GLint query's float→int conversion in
    // range; a NaN argument clamps to 0).
    auto clamp01 = [](float v) {
        if (std::isnan(v)) return 0.0f;
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    depth_range_near_ = clamp01(near_val);
    depth_range_far_ = clamp01(far_val);
}

int GLStateTracker::pixel_store_unpack_alignment() const {
    std::lock_guard<std::mutex> g(mu_);
    int a = pixel_store_unpack_alignment_;
    return (a == 1 || a == 2 || a == 4 || a == 8) ? a : 4;
}

int GLStateTracker::pixel_store_pack_alignment() const {
    std::lock_guard<std::mutex> g(mu_);
    int a = pixel_store_pack_alignment_;
    return (a == 1 || a == 2 || a == 4 || a == 8) ? a : 4;
}

void GLStateTracker::set_hint(uint32_t target, uint32_t mode) {
    std::lock_guard<std::mutex> g(mu_);
    switch (target) {
        case 0x0C50: hint_fog_ = mode; break;
        case 0x8192: hint_generate_mipmap_ = mode; break;
        case 0x0C52: hint_line_smooth_ = mode; break;
        case 0x0C53: hint_polygon_smooth_ = mode; break;
        case 0x8193: hint_texture_compression_ = mode; break;
    }
}

// ── Locked query helpers ─────────────────────────────────────────────────

bool GLStateTracker::is_enabled_locked(uint32_t cap) const {
    auto it = enabled_.find(cap);
    return (it != enabled_.end()) ? it->second : false;
}

bool GLStateTracker::get_integerv_locked(uint32_t pname, int32_t& out_val) const {
    switch (pname) {
        case GL::ACTIVE_TEXTURE:
            out_val = static_cast<int32_t>(
                active_texture_.load(std::memory_order_relaxed));
            return true;
        case GL::ARRAY_BUFFER_BINDING:
            out_val = static_cast<int32_t>(array_buffer_binding_);
            return true;
        case GL::ELEMENT_ARRAY_BUFFER_BINDING:
            out_val = static_cast<int32_t>(element_array_buffer_binding_);
            return true;
        case GL::TEXTURE_BINDING_2D: {
            // Key must use the bind TARGET (GL_TEXTURE_2D), not the
            // query pname (GL_TEXTURE_BINDING_2D) — set_texture_binding
            // keys on target, so 0x8069 here never matched.
            uint32_t key = (active_texture_.load(std::memory_order_relaxed) << 16) | (GL::TEXTURE_2D & 0xFFFF);
            auto it = texture_bindings_.find(key);
            out_val = (it != texture_bindings_.end()) ? static_cast<int32_t>(it->second) : 0;
            return true;
        }
        case GL::CURRENT_PROGRAM:
            out_val = static_cast<int32_t>(current_program_.load(std::memory_order_relaxed));
            return true;
        // NOTE: implementation limits and framebuffer formats (MAX_*,
        // *_BITS, SAMPLES, …) are deliberately NOT intercepted — the
        // tracker has no faithful mirror of them, and hardcoded answers
        // undersell the real host GPU (MAX_TEXTURE_SIZE 4096 on a host
        // that reports 16384, MAX_VERTEX_TEXTURE_IMAGE_UNITS 0 → engines
        // disable their vertex-texture-fetch path). Return false so the
        // dispatcher forwards the query to the host GL, which is
        // authoritative. Only state the tracker actually mirrors belongs
        // here (bindings, active unit, current program).
        default:
            return false;
    }
}

bool GLStateTracker::get_floatv_locked(uint32_t pname, float& out_val) const {
    switch (pname) {
        case GL::LINE_WIDTH:
            out_val = line_width_;
            return true;
        case GL::POINT_SIZE:
            out_val = point_size_;
            return true;
        default:
            return false;
    }
}

bool GLStateTracker::get_booleanv_locked(uint32_t pname, uint8_t& out_val) const {
    switch (pname) {
        case GL::DEPTH_WRITEMASK:
            out_val = depth_mask_ ? GL::TRUE : GL::FALSE;
            return true;
        default:
            return false;
    }
}

// ── Public query handlers ────────────────────────────────────────────────

bool GLStateTracker::try_handle_is_enabled(uint32_t cap, uint32_t& result_out) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = enabled_.find(cap);
    if (it == enabled_.end()) {
        return false; // untracked capability — not handled
    }
    result_out = it->second ? GL::TRUE : GL::FALSE;
    return true;
}

bool GLStateTracker::try_handle_get_booleanv(uint32_t pname, uint8_t* out_ptr) const {
    if (!out_ptr) return false;

    switch (pname) {
        case GL::COLOR_WRITEMASK: {
            // Untracked: glColorMask has no tracker setter, so a
            // hardcoded TRUE×4 lies after the guest masks channels.
            // Return false and let the host GL answer authoritatively.
            return false;
        }
        default: {
            uint8_t val = 0;
            if (get_booleanv_locked(pname, val)) {
                *out_ptr = val;
                return true;
            }
            return false;
        }
    }
}

bool GLStateTracker::try_handle_get_integerv(uint32_t pname, int32_t* out_ptr) const {
    if (!out_ptr) return false;

    switch (pname) {
        case GL::VIEWPORT: {
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = viewport_.x;
            out_ptr[1] = viewport_.y;
            out_ptr[2] = viewport_.w;
            out_ptr[3] = viewport_.h;
            return true;
        }
        case GL::SCISSOR_BOX: {
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = scissor_.x;
            out_ptr[1] = scissor_.y;
            out_ptr[2] = scissor_.w;
            out_ptr[3] = scissor_.h;
            return true;
        }
        case GL::DEPTH_RANGE: {
            // GLint variant: glGetIntegerv ROUNDS floating-point state to
            // the nearest integer (Khronos glGet type-conversion rules);
            // a plain cast truncates and diverges from the host driver.
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = static_cast<int32_t>(std::lround(depth_range_near_));
            out_ptr[1] = static_cast<int32_t>(std::lround(depth_range_far_));
            return true;
        }
        case GL::COLOR_WRITEMASK: {
            // Untracked: glColorMask has no tracker setter, so a
            // hardcoded TRUE×4 lies after the guest masks channels.
            // Return false and let the host GL answer authoritatively.
            return false;
        }
        case GL::STENCIL_VALUE_MASK:
        case GL::STENCIL_REF:
        case GL::STENCIL_WRITEMASK: {
            std::lock_guard<std::mutex> g(mu_);
            if (pname == GL::STENCIL_VALUE_MASK) out_ptr[0] = static_cast<int32_t>(stencil_front_.mask);
            else if (pname == GL::STENCIL_REF) out_ptr[0] = stencil_front_.ref;
            else out_ptr[0] = static_cast<int32_t>(stencil_front_.write_mask);
            return true;
        }
        case GL::STENCIL_BACK_VALUE_MASK:
        case GL::STENCIL_BACK_REF:
        case GL::STENCIL_BACK_WRITEMASK: {
            std::lock_guard<std::mutex> g(mu_);
            if (pname == GL::STENCIL_BACK_VALUE_MASK) out_ptr[0] = static_cast<int32_t>(stencil_back_.mask);
            else if (pname == GL::STENCIL_BACK_REF) out_ptr[0] = stencil_back_.ref;
            else out_ptr[0] = static_cast<int32_t>(stencil_back_.write_mask);
            return true;
        }
        case GL::STENCIL_FUNC:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_front_.func); return true; }
        case GL::STENCIL_BACK_FUNC:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_back_.func); return true; }
        case GL::STENCIL_FAIL:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_front_.fail); return true; }
        case GL::STENCIL_BACK_FAIL:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_back_.fail); return true; }
        case GL::STENCIL_PASS_DEPTH_FAIL:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_front_.zfail); return true; }
        case GL::STENCIL_BACK_PASS_DEPTH_FAIL:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_back_.zfail); return true; }
        case GL::STENCIL_PASS_DEPTH_PASS:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_front_.zpass); return true; }
        case GL::STENCIL_BACK_PASS_DEPTH_PASS:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(stencil_back_.zpass); return true; }
        case GL::BLEND_SRC:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_src_rgb_); return true; }
        case GL::BLEND_DST:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_dst_rgb_); return true; }
        case GL::BLEND_SRC_ALPHA:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_src_alpha_); return true; }
        case GL::BLEND_DST_ALPHA:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_dst_alpha_); return true; }
        case GL::BLEND_EQUATION_RGB:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_equation_rgb_); return true; }
        case GL::BLEND_EQUATION_ALPHA:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(blend_equation_alpha_); return true; }
        case GL::DEPTH_FUNC:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(depth_func_); return true; }
        case GL::CULL_FACE_MODE:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(cull_face_mode_); return true; }
        case GL::FRONT_FACE:
            { std::lock_guard<std::mutex> g(mu_); out_ptr[0] = static_cast<int32_t>(front_face_); return true; }
        case GL::COLOR_CLEAR_VALUE: {
            // GLint variant: round like DEPTH_RANGE above, don't truncate.
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = static_cast<int32_t>(std::lround(clear_color_[0]));
            out_ptr[1] = static_cast<int32_t>(std::lround(clear_color_[1]));
            out_ptr[2] = static_cast<int32_t>(std::lround(clear_color_[2]));
            out_ptr[3] = static_cast<int32_t>(std::lround(clear_color_[3]));
            return true;
        }
        default: {
            int32_t val = 0;
            if (get_integerv_locked(pname, val)) {
                *out_ptr = val;
                return true;
            }
            return false;
        }
    }
}

bool GLStateTracker::try_handle_get_floatv(uint32_t pname, float* out_ptr) const {
    if (!out_ptr) return false;

    switch (pname) {
        case GL::COLOR_CLEAR_VALUE: {
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = clear_color_[0];
            out_ptr[1] = clear_color_[1];
            out_ptr[2] = clear_color_[2];
            out_ptr[3] = clear_color_[3];
            return true;
        }
        case GL::DEPTH_RANGE: {
            std::lock_guard<std::mutex> g(mu_);
            out_ptr[0] = depth_range_near_;
            out_ptr[1] = depth_range_far_;
            return true;
        }
        default: {
            float val = 0.0f;
            if (get_floatv_locked(pname, val)) {
                *out_ptr = val;
                return true;
            }
            return false;
        }
    }
}

// ── Dispatch integration ─────────────────────────────────────────────────

bool GLStateTracker::try_handle_query(const std::string& name, const uint64_t args[12], CPU& cpu) {
    // Dispatch precisely by symbol name so that a pname handled by more
    // than one typed query (e.g. COLOR_CLEAR_VALUE, which is valid for
    // both glGetIntegerv and glGetFloatv) is answered with the correct
    // element type/width. Without this, glGetFloatv(COLOR_CLEAR_VALUE)
    // would be wrongly intercepted by the integerv handler and write
    // truncated ints into a float buffer.
    //
    // args[1] is already a HOST pointer (direct-window alias or bounce
    // buffer) thanks to GraphicThunk::dispatch's translate_ptr step.

    if (name == "glIsEnabled") {
        // GLboolean glIsEnabled(GLenum cap);  (cap in x0 = args[0])
        uint32_t cap = static_cast<uint32_t>(args[0]);
        uint32_t result = 0;
        if (try_handle_is_enabled(cap, result)) {
            cpu.regs[0] = result;
            return true;
        }
        return false;
    }

    if (name == "glGetBooleanv") {
        // void glGetBooleanv(GLenum pname, GLboolean *params);
        uint32_t pname = static_cast<uint32_t>(args[0]);
        uint8_t* ptr = reinterpret_cast<uint8_t*>(args[1]);
        return ptr && try_handle_get_booleanv(pname, ptr);
    }

    if (name == "glGetIntegerv") {
        // void glGetIntegerv(GLenum pname, GLint *params);
        uint32_t pname = static_cast<uint32_t>(args[0]);
        int32_t* ptr = reinterpret_cast<int32_t*>(args[1]);
        return ptr && try_handle_get_integerv(pname, ptr);
    }

    if (name == "glGetFloatv") {
        // void glGetFloatv(GLenum pname, GLfloat *params);
        uint32_t pname = static_cast<uint32_t>(args[0]);
        float* ptr = reinterpret_cast<float*>(args[1]);
        return ptr && try_handle_get_floatv(pname, ptr);
    }

    return false;
}

bool GLStateTracker::tracks_state(const std::string& name) {
    // Keep in sync with the name set below (track_state_change). A
    // function-local static builds the set once on first call.
    static const std::unordered_set<std::string> kTracked = {
        "glEnable", "glDisable", "glClearColor", "glColor3f", "glColor4f",
        "glLineWidth", "glPointSize", "glViewport", "glActiveTexture",
        "glBlendFunc", "glBlendFuncSeparate", "glBlendEquation",
        "glBlendEquationSeparate", "glDepthFunc", "glDepthMask",
        "glCullFace", "glFrontFace", "glPolygonMode", "glScissor",
        "glStencilFunc", "glStencilOp", "glStencilMask",
        "glStencilFuncSeparate", "glStencilOpSeparate",
        "glStencilMaskSeparate",         "glUseProgram", "glBindBuffer",
        "glBindBufferBase", "glBindBufferRange",
        "glBindTexture", "glBindFramebuffer", "glBindRenderbuffer",
        "glBindVertexArray",
        "glPixelStorei", "glHint", "glDepthRangef",
    };
    return kTracked.count(name) != 0;
}

void GLStateTracker::track_state_change(const std::string& name, const uint64_t args[12], const float* fv, uint8_t n_float) {
    // Capabilities.
    if (name == "glEnable") {
        set_enabled(static_cast<uint32_t>(args[0]), true);
        return;
    }
    if (name == "glDisable") {
        set_enabled(static_cast<uint32_t>(args[0]), false);
        return;
    }

    // Float-only AAPCS64 state setters (read from saved fv[]).
    if (name == "glClearColor") {
        float r = (n_float > 0) ? fv[0] : 0;
        float g = (n_float > 1) ? fv[1] : 0;
        float b = (n_float > 2) ? fv[2] : 0;
        float a = (n_float > 3) ? fv[3] : 0;
        set_clear_color(r, g, b, a);
        return;
    }
    if (name == "glColor3f") {
        float r = (n_float > 0) ? fv[0] : 0;
        float g = (n_float > 1) ? fv[1] : 0;
        float b = (n_float > 2) ? fv[2] : 0;
        set_color(r, g, b, 1.0f);
        return;
    }
    if (name == "glColor4f") {
        float r = (n_float > 0) ? fv[0] : 0;
        float g = (n_float > 1) ? fv[1] : 0;
        float b = (n_float > 2) ? fv[2] : 0;
        float a = (n_float > 3) ? fv[3] : 0;
        set_color(r, g, b, a);
        return;
    }
    if (name == "glLineWidth") {
        float w = (n_float > 0) ? fv[0] : 1.0f;
        set_line_width(w);
        return;
    }
    if (name == "glPointSize") {
        float s = (n_float > 0) ? fv[0] : 1.0f;
        set_point_size(s);
        return;
    }

    // Integer/pointer state setters (read from args[]).
    if (name == "glViewport") {
        set_viewport(static_cast<int>(args[0]),
                     static_cast<int>(args[1]),
                     static_cast<int>(args[2]),
                     static_cast<int>(args[3]));
        return;
    }
    if (name == "glActiveTexture") {
        set_active_texture(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glBlendFunc") {
        set_blend_func(static_cast<uint32_t>(args[0]),
                       static_cast<uint32_t>(args[1]));
        return;
    }
    if (name == "glBlendFuncSeparate") {
        set_blend_func_separate(static_cast<uint32_t>(args[0]),
                                static_cast<uint32_t>(args[1]),
                                static_cast<uint32_t>(args[2]),
                                static_cast<uint32_t>(args[3]));
        return;
    }
    if (name == "glBlendEquation") {
        uint32_t mode = static_cast<uint32_t>(args[0]);
        std::lock_guard<std::mutex> g(mu_);
        blend_equation_rgb_ = mode;
        blend_equation_alpha_ = mode;
        return;
    }
    if (name == "glBlendEquationSeparate") {
        std::lock_guard<std::mutex> g(mu_);
        blend_equation_rgb_ = static_cast<uint32_t>(args[0]);
        blend_equation_alpha_ = static_cast<uint32_t>(args[1]);
        return;
    }
    if (name == "glDepthFunc") {
        set_depth_func(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glDepthMask") {
        set_depth_mask(args[0] != 0);
        return;
    }
    if (name == "glCullFace") {
        set_cull_face_mode(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glFrontFace") {
        set_front_face(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glPolygonMode") {
        set_polygon_mode(static_cast<uint32_t>(args[0]),
                         static_cast<uint32_t>(args[1]));
        return;
    }
    if (name == "glScissor") {
        set_scissor(static_cast<int>(args[0]),
                    static_cast<int>(args[1]),
                    static_cast<int>(args[2]),
                    static_cast<int>(args[3]));
        return;
    }
    if (name == "glStencilFunc") {
        set_stencil_func(static_cast<uint32_t>(args[0]),
                         static_cast<int>(args[1]),
                         static_cast<uint32_t>(args[2]));
        return;
    }
    if (name == "glStencilOp") {
        set_stencil_op(static_cast<uint32_t>(args[0]),
                       static_cast<uint32_t>(args[1]),
                       static_cast<uint32_t>(args[2]));
        return;
    }
    if (name == "glStencilMask") {
        set_stencil_mask(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glStencilFuncSeparate") {
        set_stencil_func_separate(static_cast<uint32_t>(args[0]),
                                  static_cast<uint32_t>(args[1]),
                                  static_cast<int>(args[2]),
                                  static_cast<uint32_t>(args[3]));
        return;
    }
    if (name == "glStencilOpSeparate") {
        set_stencil_op_separate(static_cast<uint32_t>(args[0]),
                                static_cast<uint32_t>(args[1]),
                                static_cast<uint32_t>(args[2]),
                                static_cast<uint32_t>(args[3]));
        return;
    }
    if (name == "glStencilMaskSeparate") {
        set_stencil_mask_separate(static_cast<uint32_t>(args[0]),
                                  static_cast<uint32_t>(args[1]));
        return;
    }
    if (name == "glUseProgram") {
        set_program(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glBindBuffer") {
        uint32_t target = static_cast<uint32_t>(args[0]);
        uint32_t buffer = static_cast<uint32_t>(args[1]);
        // Record the binding for ANY target in the general map (the
        // glMapBuffer/glUnmapBuffer bounce consults it).
        set_buffer_binding(target, buffer);
        if (target == 0x8892) { // GL_ARRAY_BUFFER
            set_array_buffer_binding(buffer);
        } else if (target == 0x8893) { // GL_ELEMENT_ARRAY_BUFFER
            set_element_array_buffer_binding(buffer);
        } else if (target == 0x8F3F) { // GL_DRAW_INDIRECT_BUFFER
            set_draw_indirect_buffer_binding(buffer);
        }
        return;
    }
    if (name == "glBindBufferBase" || name == "glBindBufferRange") {
        // glBindBufferBase(target, index, buffer)
        // glBindBufferRange(target, index, buffer, offset, size)
        // — buffer is arg 2 in BOTH (the general binding map keys by
        // target; last-index-wins is accurate enough for the glMapBuffer
        // bounce). The old code read args[4] for Range, which is the
        // SIZE — buffer_bindings_ then held garbage and glMapBuffer on
        // an indexed target resolved the wrong buffer.
        uint32_t target = static_cast<uint32_t>(args[0]);
        uint32_t buffer = static_cast<uint32_t>(args[2]);
        set_buffer_binding(target, buffer);
        return;
    }
    if (name == "glBindTexture") {
        set_texture_binding(static_cast<uint32_t>(args[0]),
                            static_cast<uint32_t>(args[1]));
        return;
    }
    if (name == "glBindVertexArray") {
        set_vao_binding(static_cast<uint32_t>(args[0]));
        return;
    }
    if (name == "glBindFramebuffer") {
        // Framebuffer binding not yet tracked; reserved for future.
        return;
    }
    if (name == "glBindRenderbuffer") {
        // Renderbuffer binding not yet tracked; reserved for future.
        return;
    }
    if (name == "glPixelStorei") {
        set_pixel_store_i(static_cast<uint32_t>(args[0]),
                          static_cast<int>(args[1]));
        return;
    }
    if (name == "glHint") {
        set_hint(static_cast<uint32_t>(args[0]),
                 static_cast<uint32_t>(args[1]));
        return;
    }
    if (name == "glDepthRangef") {
        // glDepthRangef(GLclampf near, GLclampf far) — float-only row,
        // so the values arrive via fv[] (the float dispatch path).
        float n = (n_float > 0) ? fv[0] : 0.0f;
        float f = (n_float > 1) ? fv[1] : 1.0f;
        set_depth_range(n, f);
        return;
    }
}

// VAO-modeled element binding for elision. Single lock for the VAO +
// per-VAO lookup so a VAO switch racing between the two cannot mix them
// (GL is single-threaded in practice; this is belt and braces).
uint32_t GLStateTracker::vao_element_binding(bool& known) const {
    std::lock_guard<std::mutex> g(mu_);
    known = vao_known_.load(std::memory_order_relaxed);
    if (!known) return 0;
    auto it = vao_element_.find(vao_binding_.load(std::memory_order_relaxed));
    if (it == vao_element_.end()) { known = false; return 0; }
    return it->second;
}

// Deleting a bound buffer unbinds it everywhere it is recorded: the
// array member, every per-VAO element entry, and the general map (which
// the map-bounce logic reads — previously went stale on delete).
void GLStateTracker::unbind_buffers(const uint32_t* names, size_t n) {
    if (!names) return;
    std::lock_guard<std::mutex> g(mu_);
    for (size_t k = 0; k < n; k++) {
        uint32_t dead = names[k];
        if (!dead) continue;
        if (array_buffer_binding_.load(std::memory_order_relaxed) == dead)
            array_buffer_binding_.store(0, std::memory_order_relaxed);
        if (element_array_buffer_binding_ == dead)
            element_array_buffer_binding_ = 0;
        for (auto& kv : vao_element_) {
            if (kv.second == dead) kv.second = 0;
        }
        for (auto it = buffer_bindings_.begin(); it != buffer_bindings_.end(); ) {
            if (it->second == dead) it = buffer_bindings_.erase(it);
            else ++it;
        }
    }
}

void GLStateTracker::unbind_vao_elements(uint32_t vao) {
    std::lock_guard<std::mutex> g(mu_);
    vao_element_.erase(vao);
}

} // namespace arm64emu
