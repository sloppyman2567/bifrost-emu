// frost/gl_state.hpp — GLStateTracker: mirrors guest OpenGL state for
// consistent query results across thunked calls.
//
// v1.5.1-alpha: NEW. The thunk forwards GL/EGL/SDL2 calls to the host,
// but state queries (glIsEnabled, glGetIntegerv, etc.) can return
// inconsistent results if the host context differs from what the guest
// expects. GLStateTracker maintains a software-side mirror of the
// guest's GL state so queries return the values the guest set.
//
// Design:
//   - State setters update the tracker AND forward to host.
//   - State queries are answered from the tracker WITHOUT calling host,
//     avoiding context-state divergence.
//   - Un tracked queries fall through to the host.
//   - Thread-safe: a mutex protects all state access.
#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <unordered_map>
#include <mutex>

namespace arm64emu {
// Forward declarations.
class CPU;
class Memory;

// Minimal GL enum definitions (subset needed for state tracking).
// These match the standard OpenGL 2.1 / GLES 2.0 values.
namespace GL {
    constexpr uint32_t FALSE = 0;
    constexpr uint32_t TRUE = 1;

    // Capabilities (glIsEnabled / glEnable / glDisable).
    constexpr uint32_t BLEND = 0x0BE2;
    constexpr uint32_t CULL_FACE = 0x0B44;
    constexpr uint32_t DEPTH_TEST = 0x0B71;
    constexpr uint32_t DITHER = 0x0BD0;
    constexpr uint32_t LINE_SMOOTH = 0x0B20;
    constexpr uint32_t POLYGON_OFFSET_FILL = 0x8037;
    constexpr uint32_t SCISSOR_TEST = 0x0C11;
    constexpr uint32_t STENCIL_TEST = 0x0B90;
    constexpr uint32_t TEXTURE_2D = 0x0DE1;
    constexpr uint32_t SAMPLE_ALPHA_TO_COVERAGE = 0x809E;
    constexpr uint32_t SAMPLE_COVERAGE = 0x80A0;
    constexpr uint32_t MULTISAMPLE = 0x809D;

    // Blend functions.
    constexpr uint32_t ZERO = 0;
    constexpr uint32_t ONE = 1;
    constexpr uint32_t SRC_ALPHA = 0x0302;
    constexpr uint32_t ONE_MINUS_SRC_ALPHA = 0x0303;
    constexpr uint32_t DST_ALPHA = 0x0304;
    constexpr uint32_t ONE_MINUS_DST_ALPHA = 0x0305;
    constexpr uint32_t SRC_COLOR = 0x0300;
    constexpr uint32_t ONE_MINUS_SRC_COLOR = 0x0301;
    constexpr uint32_t DST_COLOR = 0x0306;
    constexpr uint32_t ONE_MINUS_DST_COLOR = 0x0307;
    constexpr uint32_t SRC_ALPHA_SATURATE = 0x0308;

    // Depth funcs.
    constexpr uint32_t NEVER = 0x0200;
    constexpr uint32_t LESS = 0x0201;
    constexpr uint32_t EQUAL = 0x0202;
    constexpr uint32_t LEQUAL = 0x0203;
    constexpr uint32_t GREATER = 0x0204;
    constexpr uint32_t NOTEQUAL = 0x0205;
    constexpr uint32_t GEQUAL = 0x0206;
    constexpr uint32_t ALWAYS = 0x0207;

    // Cull / face.
    constexpr uint32_t FRONT = 0x0404;
    constexpr uint32_t BACK = 0x0405;
    constexpr uint32_t FRONT_AND_BACK = 0x0408;
    constexpr uint32_t CW = 0x0900;
    constexpr uint32_t CCW = 0x0901;

    // Polygon modes.
    constexpr uint32_t POINT = 0x1B00;
    constexpr uint32_t LINE = 0x1B01;
    constexpr uint32_t FILL = 0x1B02;

    // Texture units.
    constexpr uint32_t TEXTURE0 = 0x84C0;
    constexpr uint32_t TEXTURE1 = 0x84C1;
    constexpr uint32_t TEXTURE2 = 0x84C2;
    constexpr uint32_t TEXTURE3 = 0x84C3;
    constexpr uint32_t TEXTURE4 = 0x84C4;
    constexpr uint32_t TEXTURE5 = 0x84C5;
    constexpr uint32_t TEXTURE6 = 0x84C6;
    constexpr uint32_t TEXTURE7 = 0x84C7;
    constexpr uint32_t TEXTURE8 = 0x84C8;
    constexpr uint32_t TEXTURE9 = 0x84C9;
    constexpr uint32_t TEXTURE15 = 0x84CF;

    // glGet* pnames.
    constexpr uint32_t VIEWPORT = 0x0BA2;
    constexpr uint32_t COLOR_CLEAR_VALUE = 0x0C22;
    constexpr uint32_t COLOR_WRITEMASK = 0x0BA3;
    constexpr uint32_t DEPTH_WRITEMASK = 0x0B72;
    constexpr uint32_t DEPTH_FUNC = 0x0B74;
    constexpr uint32_t DEPTH_RANGE = 0x0B70;
    constexpr uint32_t BLEND_SRC = 0x0BE1;
    constexpr uint32_t BLEND_DST = 0x0BE0;
    constexpr uint32_t BLEND_SRC_ALPHA = 0x0BC1;
    constexpr uint32_t BLEND_DST_ALPHA = 0x0BC0;
    constexpr uint32_t BLEND_EQUATION_RGB = 0x8009;
    constexpr uint32_t BLEND_EQUATION_ALPHA = 0x883D;
    constexpr uint32_t CULL_FACE_MODE = 0x0B45;
    constexpr uint32_t FRONT_FACE = 0x0B46;
    constexpr uint32_t LINE_WIDTH = 0x0B21;
    constexpr uint32_t POINT_SIZE = 0x0B11;
    constexpr uint32_t POLYGON_MODE = 0x0B40;
    constexpr uint32_t SCISSOR_BOX = 0x0C10;
    constexpr uint32_t STENCIL_FUNC = 0x0B92;
    constexpr uint32_t STENCIL_VALUE_MASK = 0x0B93;
    constexpr uint32_t STENCIL_FAIL = 0x0B94;
    constexpr uint32_t STENCIL_PASS_DEPTH_FAIL = 0x0B95;
    constexpr uint32_t STENCIL_PASS_DEPTH_PASS = 0x0B96;
    constexpr uint32_t STENCIL_WRITEMASK = 0x0B98;
    constexpr uint32_t STENCIL_BACK_FUNC = 0x8800;
    constexpr uint32_t STENCIL_BACK_VALUE_MASK = 0x8CA4;
    constexpr uint32_t STENCIL_BACK_FAIL = 0x8801;
    constexpr uint32_t STENCIL_BACK_PASS_DEPTH_FAIL = 0x8802;
    constexpr uint32_t STENCIL_BACK_PASS_DEPTH_PASS = 0x8803;
    constexpr uint32_t STENCIL_BACK_WRITEMASK = 0x8CA5;
    constexpr uint32_t STENCIL_REF = 0x0B97;
    constexpr uint32_t STENCIL_BACK_REF = 0x8CA3;
    constexpr uint32_t ACTIVE_TEXTURE = 0x84E0;
    constexpr uint32_t ARRAY_BUFFER_BINDING = 0x8894;
    constexpr uint32_t ELEMENT_ARRAY_BUFFER_BINDING = 0x8895;
    constexpr uint32_t TEXTURE_BINDING_2D = 0x8069;
    constexpr uint32_t CURRENT_PROGRAM = 0x8B8D;
    constexpr uint32_t MAX_TEXTURE_SIZE = 0x0D33;
    constexpr uint32_t MAX_VERTEX_ATTRIBS = 0x8869;
    constexpr uint32_t MAX_TEXTURE_IMAGE_UNITS = 0x8872;
    constexpr uint32_t MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8B4D;
    constexpr uint32_t MAX_VERTEX_TEXTURE_IMAGE_UNITS = 0x8B4C;
    constexpr uint32_t MAX_FRAGMENT_UNIFORM_COMPONENTS = 0x8B49;
    constexpr uint32_t MAX_VERTEX_UNIFORM_COMPONENTS = 0x8B4A;
    constexpr uint32_t NUM_EXTENSIONS = 0x821D;
    constexpr uint32_t RENDERER = 0x1F01;
    constexpr uint32_t VERSION = 0x1F02;
    constexpr uint32_t SHADING_LANGUAGE_VERSION = 0x8B8C;
    constexpr uint32_t EXTENSIONS = 0x1F03;
    constexpr uint32_t RED_BITS = 0x00D5;
    constexpr uint32_t GREEN_BITS = 0x00D6;
    constexpr uint32_t BLUE_BITS = 0x00D7;
    constexpr uint32_t ALPHA_BITS = 0x00D8;
    constexpr uint32_t DEPTH_BITS = 0x00D2;
    constexpr uint32_t STENCIL_BITS = 0x00D3;
    constexpr uint32_t MAJOR_VERSION = 0x821B;
    constexpr uint32_t MINOR_VERSION = 0x821C;
    constexpr uint32_t CONTEXT_FLAGS = 0x821E;
    constexpr uint32_t SAMPLE_BUFFERS = 0x80ED;
    constexpr uint32_t SAMPLES = 0x80D6;
    constexpr uint32_t SUBPIXEL_BITS = 0x0D50;
    constexpr uint32_t CONTEXT_PROFILE_MASK = 0x9126;
    constexpr uint32_t CONTEXT_CORE_PROFILE_BIT = 0x00000001;
    constexpr uint32_t CONTEXT_COMPATIBILITY_PROFILE_BIT = 0x00000002;

    // glGetString name args.
    constexpr uint32_t GL_VENDOR = 0x1F00;
    constexpr uint32_t GL_RENDERER = 0x1F01;
    constexpr uint32_t GL_VERSION = 0x1F02;
    constexpr uint32_t GL_SHADING_LANGUAGE_VERSION = 0x8B8C;
    constexpr uint32_t GL_EXTENSIONS = 0x1F03;
} // namespace GL

class GLStateTracker {
public:
    GLStateTracker();
    ~GLStateTracker();

    GLStateTracker(const GLStateTracker&) = delete;
    GLStateTracker& operator=(const GLStateTracker&) = delete;

    // Reset all tracked state to OpenGL defaults.
    void reset();

    // ── State setters ────────────────────────────────────────────────
    // Call these after forwarding the corresponding GL call to the host.
    void set_enabled(uint32_t cap, bool on);
    void set_clear_color(float r, float g, float b, float a);
    void set_viewport(int x, int y, int w, int h);
    void set_color(float r, float g, float b, float a);
    void set_line_width(float w);
    void set_point_size(float s);
    void set_active_texture(uint32_t unit);
    void set_blend_func(uint32_t src, uint32_t dst);
    void set_blend_func_separate(uint32_t srcRGB, uint32_t dstRGB,
                                 uint32_t srcAlpha, uint32_t dstAlpha);
    void set_depth_func(uint32_t func);
    void set_depth_mask(bool flag);
    void set_cull_face_mode(uint32_t mode);
    void set_front_face(uint32_t mode);
    void set_polygon_mode(uint32_t face, uint32_t mode);
    void set_scissor(int x, int y, int w, int h);
    void set_stencil_func(uint32_t func, int ref, uint32_t mask);
    void set_stencil_op(uint32_t fail, uint32_t zfail, uint32_t zpass);
    void set_stencil_mask(uint32_t mask);
    void set_stencil_func_separate(uint32_t face, uint32_t func,
                                   int ref, uint32_t mask);
    void set_stencil_op_separate(uint32_t face, uint32_t fail,
                                 uint32_t zfail, uint32_t zpass);
    void set_stencil_mask_separate(uint32_t face, uint32_t mask);
    void set_program(uint32_t program);
    void set_array_buffer_binding(uint32_t buffer);
    void set_element_array_buffer_binding(uint32_t buffer);
    // v1.5.2.alpha: used to decide whether the final arg of
    // glVertexAttribPointer / glDrawElements is a VBO byte offset (buffer
    // bound) or a real guest pointer (client-side vertex/index arrays).
    uint32_t array_buffer_binding() const;
    uint32_t element_array_buffer_binding() const;
    void set_texture_binding(uint32_t target, uint32_t texture);
    void set_pixel_store_i(uint32_t pname, int param);
    void set_hint(uint32_t target, uint32_t mode);

    // ── Query handlers ───────────────────────────────────────────────
    // These are called from GraphicThunk::dispatch before forwarding to
    // host. Return true if the query was handled; false means fall
    // through to host.
    //
    // Pre-conditions: pointer args have already been translated to host
    // addresses by GraphicThunk::dispatch.
    bool try_handle_is_enabled(uint32_t cap, uint32_t& result_out) const;
    bool try_handle_get_booleanv(uint32_t pname, uint8_t* out_ptr) const;
    bool try_handle_get_integerv(uint32_t pname, int32_t* out_ptr) const;
    bool try_handle_get_floatv(uint32_t pname, float* out_ptr) const;

    // ── Dispatch integration ─────────────────────────────────────────
    // Returns true if this symbol is a state query that was handled.
    // `name` is the GL symbol name ("glIsEnabled", "glGetIntegerv",
    // "glGetFloatv", "glGetBooleanv") so we dispatch to the correct
    // typed handler instead of guessing from the pname. `args` holds the
    // dispatcher's argument registers after pointer translation (i.e.
    // pointer args are already host addresses, either direct-window
    // aliases or bounce buffers). cpu.regs[0] is updated for scalar
    // returns; pointer args are written directly to the host buffer the
    // dispatcher will write back to guest memory.
    bool try_handle_query(const std::string& name, const uint64_t args[12], CPU& cpu);

    // Update tracked state after a successful host dispatch.
    // `name` is the GL symbol name (e.g. "glEnable").
    // `args` contains the original pre-call integer args (x0..).
    // `fv` contains the original pre-call float args (v0..), or nullptr
    //    if the function has no float args.
    // `n_float` is the number of valid float args in `fv`.
    void track_state_change(const std::string& name, const uint64_t args[12], const float* fv, uint8_t n_float);

private:
    // ── Internal helpers ─────────────────────────────────────────────
    struct Viewport { int x, y, w, h; };
    struct Scissor { int x, y, w, h; };
    struct StencilState {
        uint32_t func = 0x0201; // GL_LESS
        int ref = 0;
        uint32_t mask = 0xFFFFFFFF;
        uint32_t fail = 0x0000; // GL_KEEP
        uint32_t zfail = 0x0000;
        uint32_t zpass = 0x0000;
        uint32_t write_mask = 0xFFFFFFFF;
    };

    mutable std::mutex mu_;
    // Enabled capabilities (sparse map: cap enum -> bool).
    std::unordered_map<uint32_t, bool> enabled_;
    // Default state values.
    float clear_color_[4] = {0, 0, 0, 0};
    float current_color_[4] = {0, 0, 0, 0};
    Viewport viewport_ = {0, 0, 640, 480};
    float line_width_ = 1.0f;
    float point_size_ = 1.0f;
    uint32_t active_texture_ = 0x84C0; // GL_TEXTURE0
    uint32_t blend_src_rgb_ = 0x0302; // GL_SRC_ALPHA
    uint32_t blend_dst_rgb_ = 0x0303; // GL_ONE_MINUS_SRC_ALPHA
    uint32_t blend_src_alpha_ = 0x0302;
    uint32_t blend_dst_alpha_ = 0x0303;
    uint32_t blend_equation_rgb_ = 0x8006; // GL_FUNC_ADD
    uint32_t blend_equation_alpha_ = 0x8006;
    uint32_t depth_func_ = 0x0203; // GL_LEQUAL
    bool depth_mask_ = true;
    uint32_t cull_face_mode_ = 0x0405; // GL_BACK
    uint32_t front_face_ = 0x0901; // GL_CCW
    std::array<uint32_t, 2> polygon_mode_ = {0x1B02, 0x1B02}; // front, back
    Scissor scissor_ = {0, 0, 0, 0};
    StencilState stencil_front_;
    StencilState stencil_back_;
    uint32_t current_program_ = 0;
    uint32_t array_buffer_binding_ = 0;
    uint32_t element_array_buffer_binding_ = 0;
    // Per-texture-unit bindings: unit -> target -> texture name.
    // Flat map: (unit << 16) | target -> texture name.
    std::unordered_map<uint32_t, uint32_t> texture_bindings_;
    // Pixel store / hint state.
    int pixel_store_unpack_alignment_ = 4;
    int pixel_store_pack_alignment_ = 4;
    uint32_t hint_fog_ = 0x1400; // GL_DONT_CARE
    uint32_t hint_generate_mipmap_ = 0x1400; // GL_DONT_CARE
    uint32_t hint_line_smooth_ = 0x1400;
    uint32_t hint_polygon_smooth_ = 0x1400;
    uint32_t hint_texture_compression_ = 0x1400;

    bool is_enabled_locked(uint32_t cap) const;
    bool get_integerv_locked(uint32_t pname, int32_t& out_val) const;
    bool get_floatv_locked(uint32_t pname, float& out_val) const;
    bool get_booleanv_locked(uint32_t pname, uint8_t& out_val) const;
};

} // namespace arm64emu
