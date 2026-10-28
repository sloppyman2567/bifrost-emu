// Android.cpp — Bifrost-private host call 0x100E used by the Android
// native/managed launchers (Guest/Android/Launchers/ and Managed/): host
// event pumping, the owned window, input events, audio output state,
// SurfaceView composition and window identity. Operation in x0.
//
// Only available when the emulator runs the Android runtime.
#include "System/Core/Emulator.hpp"
#include "System/Memory/Memory.hpp"
#include "System/Core/CPU.hpp"
#include "System/Syscalls/Syscalls.hpp"
#include "System/Audio/Audio.hpp"
#include "System/Android/Surface/Surface.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace arm64emu {
int64_t syscall_android_launcher(Emulator& emu, CPU& cpu, uint64_t num) {
    switch (num) {
        case 0x100E: { // Native APK launcher event/input-queue adapter.
            if (!emu.android_runtime()) { ret_host(-EPERM); return 0; }
            auto& manager = frost::AndroidSurfaceManager::instance();
            if (cpu.regs[0] == 0) {
                manager.pump_host_events(&cpu);
                cpu.regs[0] = manager.quit_requested();
            } else if (cpu.regs[0] == 1) cpu.regs[0] = manager.input_queue_handle();
            else if (cpu.regs[0] == 2) { // Managed Activity: open the owned window.
                int32_t width = static_cast<int32_t>(cpu.regs[1]);
                int32_t height = static_cast<int32_t>(cpu.regs[2]);
                int result = manager.set_buffers_geometry(width, height, 1);
                if (!result && !manager.open_host_window()) result = -ENODEV;
                if (!result) manager.looper_prepare(cpu.tid, 1);
                ret_host(result);
            } else if (cpu.regs[0] == 3) {
                // Private managed-window ABI: 55 64-bit words, up to eight
                // touch pointers. Floats occupy the low 32 bits; no host
                // pointers cross this boundary.
                uint64_t data[55]{};
                const uint64_t address = cpu.regs[1];
                try { emu.mem().check_access(address, sizeof(data), Memory::GUEST_PROT_WRITE); }
                catch (const EmuError&) { ret_host(-EFAULT); return 0; }
                manager.pump_host_events(nullptr);
                // Java's MessageQueue owns a different platform Looper.
                // Dispatch callbacks registered through the app's adapter on
                // this same UI thread, without blocking its Handler.
                manager.looper_poll(cpu, 0, nullptr, nullptr, nullptr, false);
                data[1] = manager.quit_requested();
                data[2] = manager.width(); data[3] = manager.height();
                uint64_t event = 0;
                if (!manager.queue_get_event(manager.input_queue_handle(), &event)) {
                    data[0] = manager.event_type(event);
                    data[5] = manager.event_source(event);
                    data[6] = manager.event_device_id(event);
                    if (data[0] == static_cast<uint64_t>(frost::AndroidSurfaceManager::EVENT_TYPE_TEXT)) {
                        // Text input: kind in word 4, the time in 11 and 12, the
                        // UTF-8 length in 13 and the bytes packed from word 14
                        // (at most 255 bytes, so up to word 45).
                        char text[BIFROST_WINDOW_TEXT_MAX + 1];
                        uint32_t kind;
                        const uint32_t length = manager.event_text(event, kind, text);
                        data[4] = kind;
                        data[11] = manager.key_down_time(event);
                        data[12] = manager.key_event_time(event);
                        data[13] = length;
                        std::memcpy(&data[14], text, length);
                    } else if (data[0] == 1) {
                        data[4] = manager.key_action(event);
                        data[7] = manager.key_meta_state(event);
                        data[8] = manager.key_code(event);
                        data[9] = manager.key_scan_code(event);
                        data[10] = manager.key_repeat_count(event);
                        data[11] = manager.key_down_time(event);
                        data[12] = manager.key_event_time(event);
                        data[19] = manager.key_flags(event);
                    } else {
                        data[4] = manager.motion_action(event);
                        data[11] = manager.motion_down_time(event);
                        data[12] = manager.motion_event_time(event);
                        const size_t count = std::min<size_t>(manager.motion_pointer_count(event), 8);
                        data[13] = count;
                        // Pointer 0 uses words 14..18 and pointer i > 0 words
                        // 15 + 5i..19 + 5i: x, y, pressure, size, id.
                        for (size_t p = 0; p < count; ++p) {
                            const size_t base = p == 0 ? 14 : 15 + 5 * p;
                            const float values[] = {manager.motion_x(event, p), manager.motion_y(event, p),
                                manager.motion_pressure(event, p), manager.motion_size(event, p)};
                            for (unsigned i = 0; i < 4; ++i) {
                                uint32_t bits; std::memcpy(&bits, &values[i], sizeof(bits));
                                data[base + i] = bits;
                            }
                            data[base + 4] = manager.motion_pointer_id(event, p);
                        }
                    }
                    manager.queue_finish_event(manager.input_queue_handle(), event);
                }
                emu.mem().write(address, data, sizeof(data));
                ret_host(0);
            }
            else if (cpu.regs[0] == 4) { // Managed audio: primary output and PCM count.
                uint32_t rate, frames;
                emu.audio().output_config(rate, frames);
                uint64_t data[] = {rate, frames, emu.audio().bytes_pushed()};
                try { emu.mem().write(cpu.regs[1], data, sizeof(data)); ret_host(0); }
                catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 5) { // Managed SurfaceView composition policy.
                if (cpu.regs[2] > 1) ret_host(-EINVAL);
                else ret_host(manager.set_surface_opaque(manager.surface_handle(cpu.regs[1]), cpu.regs[2] != 0));
            }
            else if (cpu.regs[0] == 6) { // UI thread: UTF-8 title and RGBA icon.
                try {
                    uint64_t record[4]; // title, pixels, width, height
                    emu.mem().read(cpu.regs[1], record, sizeof(record));
                    if (record[2] > 256 || record[3] > 256 ||
                        (record[1] && (!record[2] || !record[3]))) {
                        ret_host(-EINVAL); return 0;
                    }
                    std::string title;
                    if (record[0]) {
                        for (size_t i = 0; i < 1024; ++i) {
                            char c = emu.mem().load<uint8_t>(record[0] + i);
                            if (!c) break;
                            title.push_back(c);
                        }
                        if (title.size() == 1024) { ret_host(-EINVAL); return 0; }
                    }
                    std::vector<uint8_t> pixels(record[1] ? record[2] * record[3] * 4 : 0);
                    if (!pixels.empty()) emu.mem().read(record[1], pixels.data(), pixels.size());
                    ret_host(manager.set_window_identity(title, pixels.empty() ? nullptr : pixels.data(),
                                                         record[2], record[3]));
                } catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 7) { // Native-only launcher: acquire its owned window.
                ret_host(manager.open_host_window() ? manager.from_surface() : 0);
            }
            else if (cpu.regs[0] == 8) { // Managed audio: started host output streams.
                ret_host(static_cast<int64_t>(emu.audio().active_streams()));
            }
            else if (cpu.regs[0] == 9) { // Framework popup window: id, x, y, w, h, ARGB rows.
                try {
                    int64_t record[6];
                    emu.mem().read(cpu.regs[1], record, sizeof(record));
                    const int64_t w = record[3], h = record[4];
                    if (record[0] < 0 || record[0] > UINT32_MAX || w <= 0 || h <= 0 ||
                        w > 8192 || h > 8192 || std::abs(record[1]) > 65536 || std::abs(record[2]) > 65536) {
                        ret_host(-EINVAL); return 0;
                    }
                    std::vector<uint32_t> pixels(static_cast<size_t>(w * h));
                    emu.mem().read(static_cast<uint64_t>(record[5]), pixels.data(), pixels.size() * 4);
                    ret_host(manager.show_popup(static_cast<uint32_t>(record[0]), int32_t(record[1]),
                                                int32_t(record[2]), int32_t(w), int32_t(h), pixels.data()));
                } catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 10) { // Framework popup window removed.
                if (cpu.regs[1] > UINT32_MAX) ret_host(-EINVAL);
                else { manager.remove_popup(static_cast<uint32_t>(cpu.regs[1])); ret_host(0); }
            }
            else if (cpu.regs[0] == 11) { // Managed display: pixel width, height, density dpi.
                if (!manager.ready()) { ret_host(-ENODEV); return 0; }
                const uint64_t data[3] = {static_cast<uint64_t>(manager.width()),
                                          static_cast<uint64_t>(manager.height()),
                                          static_cast<uint64_t>(manager.density_dpi())};
                try { emu.mem().check_access(cpu.regs[1], sizeof(data), Memory::GUEST_PROT_WRITE); }
                catch (const EmuError&) { ret_host(-EFAULT); return 0; }
                emu.mem().write(cpu.regs[1], data, sizeof(data));
                ret_host(0);
            }
            else if (cpu.regs[0] == 12) { // Managed display: max bounds (pixel width, height).
                if (!manager.ready()) { ret_host(-ENODEV); return 0; }
                int32_t width = 0, height = 0;
                manager.display_size(width, height);
                const uint64_t data[2] = {static_cast<uint64_t>(width), static_cast<uint64_t>(height)};
                try { emu.mem().check_access(cpu.regs[1], sizeof(data), Memory::GUEST_PROT_WRITE); }
                catch (const EmuError&) { ret_host(-EFAULT); return 0; }
                emu.mem().write(cpu.regs[1], data, sizeof(data));
                ret_host(0);
            }
            else if (cpu.regs[0] == 14) { // Managed process lifecycle, owned by the Binder realm.
                ret_host(emu.vfs().report_android_process_state(static_cast<int32_t>(cpu.regs[1])));
            }
            else if (cpu.regs[0] == 15) { // Explicit real guest ANativeWindow -> host backing association.
                // x3: optional BufferQueue size (width << 32 | height).
                ret_host(manager.bind_native_window(cpu.regs[1], cpu.regs[2],
                         static_cast<int32_t>(cpu.regs[3] >> 32), static_cast<int32_t>(cpu.regs[3])));
            }
            else if (cpu.regs[0] == 16) { // Acquired Android image, converted to straight ARGB rows.
                try {
                    int64_t record[6];
                    emu.mem().read(cpu.regs[1], record, sizeof(record));
                    const int64_t w = record[3], h = record[4];
                    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
                            record[1] < -65536 || record[1] > 65536 || record[2] < -65536 || record[2] > 65536) {
                        ret_host(-EINVAL); return 0;
                    }
                    uint64_t handle = manager.surface_handle(record[0]);
                    if (!handle) { ret_host(-ENODEV); return 0; }
                    std::vector<uint32_t> pixels(static_cast<size_t>(w * h));
                    emu.mem().read(record[5], pixels.data(), pixels.size() * 4);
                    ret_host(manager.present_pixels(handle, record[1], record[2], w, h, pixels.data()));
                } catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 17) { // Real window's corresponding host reference, when attached.
                uint64_t handle = manager.surface_handle(cpu.regs[1]);
                if (cpu.regs[2] == 1 && handle) manager.acquire(handle);
                else if (cpu.regs[2] == 2 && handle) manager.release(handle);
                else if (cpu.regs[2] > 2) { ret_host(-EINVAL); return 0; }
                ret_host(handle);
            }
            else if (cpu.regs[0] == 18) { // Managed SurfaceView's position and pixel size.
                try {
                    int64_t record[5]; emu.mem().read(cpu.regs[1], record, sizeof(record));
                    if (record[1] < -65536 || record[1] > 65536 || record[2] < -65536 || record[2] > 65536 ||
                            record[3] <= 0 || record[3] > 4096 || record[4] <= 0 || record[4] > 4096) {
                        ret_host(-EINVAL); return 0;
                    }
                    ret_host(manager.surface_geometry(manager.surface_handle(record[0]),
                             record[1], record[2], record[3], record[4]));
                } catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 19) { // Managed window order: surface x1 directly below surface x2.
                ret_host(manager.surface_place_below(manager.surface_handle(cpu.regs[1]),
                                                     manager.surface_handle(cpu.regs[2])));
            }
            else if (cpu.regs[0] == 20) { // Managed text input: active, x, y, width, height.
                try {
                    int64_t record[5];
                    emu.mem().read(cpu.regs[1], record, sizeof(record));
                    if (record[0] < 0 || record[0] > 1 || record[3] < 0 || record[4] < 0 ||
                        std::abs(record[1]) > INT32_MAX || std::abs(record[2]) > INT32_MAX ||
                        record[3] > INT32_MAX || record[4] > INT32_MAX) {
                        ret_host(-EINVAL); return 0;
                    }
                    ret_host(manager.set_text_input(record[0] != 0, static_cast<int32_t>(record[1]),
                             static_cast<int32_t>(record[2]), static_cast<int32_t>(record[3]),
                             static_cast<int32_t>(record[4])));
                } catch (const EmuError&) { ret_host(-EFAULT); }
            }
            else if (cpu.regs[0] == 21) { // Managed non-resizable app: fixed pixel width, height.
                if (cpu.regs[1] == 0 || cpu.regs[1] > 16384 || cpu.regs[2] == 0 || cpu.regs[2] > 16384) {
                    ret_host(-EINVAL); return 0;
                }
                ret_host(manager.set_fixed_window(static_cast<int32_t>(cpu.regs[1]),
                                                  static_cast<int32_t>(cpu.regs[2])));
            }
            else if (cpu.regs[0] == 22) { // HOME/task return: retain the host window and surfaces.
                if (cpu.regs[1] > 1) ret_host(-EINVAL);
                else ret_host(manager.set_task_visible(cpu.regs[1] != 0));
            }
            else if (cpu.regs[0] == 23) { // Managed orientation: release, portrait, landscape.
                if (cpu.regs[1] > 2) ret_host(-EINVAL);
                else ret_host(manager.set_window_orientation(static_cast<int32_t>(cpu.regs[1])));
            }
            else if (cpu.regs[0] == 24) { // Managed pointer shape: a BIFROST_WINDOW_CURSOR_* value.
                if (cpu.regs[1] > INT32_MAX) ret_host(-EINVAL);
                else ret_host(manager.set_cursor(static_cast<int>(cpu.regs[1])));
            }
            else ret_host(-EINVAL);
            return 0;
        }
        default:
            return SYSCALL_NOT_HANDLED;
    }
    return 0;
}
} // namespace arm64emu
