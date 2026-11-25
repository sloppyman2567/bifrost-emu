// Host sensors (against fixture /proc and /sys trees) and the in-window stats panel.
#include "System/Graphics/Display/HostSensors.hpp"
#include "System/Graphics/Display/StatsOverlay.hpp"
#include "System/Graphics/Display/WindowStats.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
namespace fs = std::filesystem;
using namespace arm64emu::window_stats;

static void put(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path) << text;
}
static bool near(double a, double b) { return std::fabs(a - b) < 0.01; }
// One text line per row: the cells of a table row joined by `|` (figure then unit), a note's right-hand text after a
// tab, a gap as an empty line and the graph as its sample count.
static std::string flat(const std::vector<OverlayRow> &rows) {
  std::string text;
  for (const auto &row : rows) {
    if (row.kind == OverlayRow::Graph) {
      text += "[graph " + std::to_string(row.samples.size()) + "]\n";
      continue;
    }
    for (size_t c = 0; c < row.cells.size(); ++c) {
      if (c) text += row.kind == OverlayRow::Note ? "\t" : "|";
      text += row.cells[c].text + row.cells[c].unit;
    }
    text += '\n';
  }
  return text;
}
static const OverlayRow *find_row(const std::vector<OverlayRow> &rows, const char *label) {
  for (const auto &row : rows)
    if (row.kind == OverlayRow::Table && !row.cells.empty() && row.cells[0].text == label) return &row;
  return nullptr;
}
static bool has_pixel(const OverlayImage &image, bool (*test)(uint32_t)) {
  for (auto pixel : image.argb)
    if (test(pixel)) return true;
  return false;
}

int main() {
  // /proc/stat: busy excludes idle and iowait.
  detail::CpuTimes times;
  assert(detail::parse_cpu_stat("cpu  100 0 50 800 50 0 0 0 0 0\ncpu0 1 2 3 4\n", times));
  assert(times.total == 1000 && times.busy == 150);
  assert(!detail::parse_cpu_stat("cpu0 1 2 3 4\n", times) && !detail::parse_cpu_stat("cpu 1 2\n", times));
  // /proc/PID/stat: the command may hold spaces and parentheses.
  uint64_t ticks = 0;
  assert(detail::parse_process_ticks("42 (a) b (c) S 1 2 3 4 5 6 7 8 9 10 111 222 0 0 20 0 1 0", ticks) && ticks == 333);
  assert(!detail::parse_process_ticks("42 (x) S 1 2", ticks));
  {
    // Session memory: the Binder realm plus descendants, by PSS, with VmRSS only where smaps_rollup is unreadable.
    using std::string_literals::operator""s;
    long parent = 0;
    assert(detail::parse_parent_pid("42 (a) b (c) S 7 2", parent) && parent == 7);
    assert(!detail::parse_parent_pid("42 (a", parent));
    assert(detail::realm_socket("emu\0--x\0--android-binder-broker\0/s\0"s) == "/s");
    assert(detail::realm_socket("emu\0--asgard-broker\0/s\0"s) == "/s");
    assert(detail::realm_socket("python3\0--binder-broker\0/s\0"s).empty());
    assert(detail::realm_socket("emu\0--asgard-broker"s).empty());
    uint64_t bytes = 0;
    assert(detail::parse_kb_field("7f0 [rollup]\nRss:  9 kB\nPss:  4 kB\nPss_Anon: 2 kB\n", "Pss:", bytes) && bytes == 4096);
    assert(!detail::parse_kb_field("Pss_Anon: 2 kB\n", "Pss:", bytes));
    const fs::path session = fs::temp_directory_path() / ("bifrost-session-" + std::to_string(getpid()));
    fs::remove_all(session);
    const auto process = [&](int pid, int ppid, const std::string &cmdline, int pss_kb, int rss_kb = 0) {
      const fs::path dir = session / std::to_string(pid);
      put(dir / "stat", std::to_string(pid) + " (a b) S " + std::to_string(ppid) + " 1 1\n");
      put(dir / "cmdline", cmdline);
      if (pss_kb) put(dir / "smaps_rollup", "0 [rollup]\nRss: 999999 kB\nPss: " + std::to_string(pss_kb) + " kB\n");
      if (rss_kb) put(dir / "status", "Name: a\nVmRSS: " + std::to_string(rss_kb) + " kB\n");
    };
    const std::string realm = "--android-binder-broker\0/s\0"s;
    process(10, 5, "emu\0"s + realm, 1000);                       // the app, started by its launcher
    process(5, 1, "python3\0--binder-broker\0/s\0"s, 64);         // the launcher is not emulator memory
    process(20, 1, "emu\0--asgard-broker\0/s\0"s, 200);           // broker
    process(21, 20, "emu\0--asgard-service-parent\0" "20\0"s + realm, 30);  // realm service
    process(30, 5, "emu\0"s + realm, 400);                        // another process of the realm
    process(31, 30, "tool\0"s, 7);                                // a realm member's child
    process(11, 10, "helper\0"s, 50);                             // the app's own descendants
    process(12, 11, "helper\0"s, 5);
    process(40, 1, "emu\0--android-binder-broker\0/other\0"s, 8000);  // another realm
    process(41, 1, "unrelated\0"s, 16000);
    process(32, 30, "old-kernel\0"s, 0, 90);                      // no smaps_rollup: VmRSS
    process(33, 30, ""s, 0);                                      // a kernel thread or zombie has no figures
    put(session / "self/cmdline", "not a pid");
    assert(session_memory(session.string(), 10) == uint64_t(1000 + 200 + 30 + 400 + 7 + 50 + 5 + 90) * 1024);
    assert(session_memory(session.string(), 41) == uint64_t(16000) * 1024);  // no realm: only itself and descendants
    assert(session_memory(session.string(), 11) == uint64_t(50 + 5) * 1024);
    assert(session_memory((session / "missing").string(), 10) == 0);
    fs::remove_all(session);
    assert(session_memory("/proc", getpid()) > 0);
  }
  double celsius = 0;
  assert(detail::plausible_temperature(71250, celsius) && near(celsius, 71.25));
  assert(!detail::plausible_temperature(0, celsius) && !detail::plausible_temperature(200000, celsius));

  const fs::path root = fs::temp_directory_path() / ("bifrost-overlay-test-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto proc = (root / "proc").string(), sys = (root / "sys").string();
  put(root / "proc/stat", "cpu  100 0 100 700 100 0 0 0 0 0\n");
  put(root / "proc/self/stat", "7 (probe) S 1 1 1 0 -1 0 0 0 0 0 100 100 0 0 20 0 1 0 1 1 1\n");
  put(root / "proc/meminfo", "MemTotal:       16000000 kB\nMemFree:  1000000 kB\nMemAvailable:    4000000 kB\n");
  // AMD CPU (Tdie wins over Tctl, a non-CPU hwmon is ignored) and an AMD GPU (edge wins over junction).
  put(root / "sys/class/hwmon/hwmon0/name", "nvme\n");
  put(root / "sys/class/hwmon/hwmon0/temp1_input", "30000\n");
  put(root / "sys/class/hwmon/hwmon1/name", "k10temp\n");
  put(root / "sys/class/hwmon/hwmon1/temp1_label", "Tctl\n");
  put(root / "sys/class/hwmon/hwmon1/temp1_input", "75000\n");
  put(root / "sys/class/hwmon/hwmon1/temp2_label", "Tdie\n");
  put(root / "sys/class/hwmon/hwmon1/temp2_input", "65500\n");
  const auto gpu = (root / "sys/devices/pci0/0000:03:00.0").string();
  put(gpu + "/hwmon/hwmon4/temp1_label", "edge\n");
  put(gpu + "/hwmon/hwmon4/temp1_input", "63000\n");
  put(gpu + "/hwmon/hwmon4/temp2_label", "junction\n");
  put(gpu + "/hwmon/hwmon4/temp2_input", "79000\n");
  put(gpu + "/gpu_busy_percent", "41\n");
  put(gpu + "/mem_info_vram_total", "8573157376\n");
  put(gpu + "/mem_info_vram_used", "2674360320\n");
  fs::create_directories(root / "sys/bus/pci/drivers/amdgpu");
  fs::create_directory_symlink("../../../../bus/pci/drivers/amdgpu", gpu + "/driver");
  HostSensors sensors(proc, sys);
  auto first = sensors.sample(10000000000ULL, gpu);
  assert(first.cpu_percent < 0 && first.process_percent < 0);  // a rate needs two samples
  assert(near(first.cpu_temp, 65.5) && near(first.gpu_temp, 63) && near(first.gpu_percent, 41));
  assert(first.vram_known && first.vram_total == 8573157376ULL && first.vram_used == 2674360320ULL);
  assert(first.gpu_driver == "amdgpu");
  assert(first.ram_known && first.ram_total == 16000000ULL * 1024 && first.ram_used == 12000000ULL * 1024);
  put(root / "proc/stat", "cpu  200 0 200 800 100 0 0 0 0 0\n");  // +200 busy of +300 total
  put(root / "proc/self/stat", "7 (probe) S 1 1 1 0 -1 0 0 0 0 0 150 150 0 0 20 0 1 0 1 1 1\n");
  assert(near(sensors.sample(10500000000ULL, gpu).cpu_percent, -1));  // under a second: cached reading
  auto second = sensors.sample(11000000000ULL, gpu);
  assert(near(second.cpu_percent, 100.0 * 200 / 300));
  const long cpus = sysconf(_SC_NPROCESSORS_ONLN), rate = sysconf(_SC_CLK_TCK);
  assert(near(second.process_percent, 100.0 * (100.0 / rate) / 1.0 / cpus));  // 100 ticks in one second
  // Without a known GPU its readings stay unavailable.
  assert(sensors.sample(12000000000ULL, "").gpu_temp < 0 && sensors.sample(13000000000ULL, "").gpu_percent < 0);
  assert(!sensors.sample(14000000000ULL, "").vram_known && sensors.sample(15000000000ULL, "").gpu_driver.empty());
  // Counters that disagree (used above total) are not trusted.
  put(gpu + "/mem_info_vram_used", "9999999999999\n");
  assert(!sensors.sample(16000000000ULL, gpu).vram_known);

  // Without a usage file (not amdgpu) the process's DRM engine times give the share: the busiest engine over the
  // elapsed time and its engine count, each client once, other GPUs ignored.
  {
    detail::DrmEngines e;
    assert(detail::parse_drm_engines("drm-driver: i915\ndrm-client-id: 7\ndrm-pdev: 0000:00:02.0\n"
        "drm-engine-render: 1000 ns\ndrm-engine-capacity-render: 2\ndrm-engine-copy: 5 ns\n"
        "drm-cycles-render: 9\ndrm-engine-video: 3 cycles\n", e));
    assert(e.client == "7" && e.pdev == "0000:00:02.0" && e.busy_ns.size() == 2 && e.busy_ns["render"] == 1000 &&
           e.busy_ns["copy"] == 5 && e.capacity["render"] == 2);
    assert(!detail::parse_drm_engines("drm-engine-render: 1 ns\n", e));  // not a DRM client
    const auto client = [](const char *id, const char *pdev, const char *ns, const char *capacity = "") {
      return std::string("drm-client-id: ") + id + "\ndrm-pdev: " + pdev + "\ndrm-engine-render: " + ns + " ns\n" + capacity;
    };
    const fs::path root2 = fs::temp_directory_path() / ("bifrost-overlay-engines-" + std::to_string(getpid()));
    fs::remove_all(root2);
    const auto device = (root2 / "sys/devices/pci0/0000:03:00.0").string();  // no gpu_busy_percent here
    fs::create_directories(device);
    put(root2 / "proc/self/fdinfo/3", client("5", "0000:03:00.0", "1000000000"));
    put(root2 / "proc/self/fdinfo/4", client("5", "0000:03:00.0", "1000000000"));  // the same client again
    put(root2 / "proc/self/fdinfo/5", client("6", "0000:09:00.0", "99000000000"));  // another GPU
    put(root2 / "proc/self/fdinfo/6", "pos:\t0\nflags:\t02\n");                     // not a DRM fd
    HostSensors engines((root2 / "proc").string(), (root2 / "sys").string());
    assert(engines.sample(100000000000ULL, device).gpu_percent < 0);  // the first sample has nothing to compare
    put(root2 / "proc/self/fdinfo/3", client("5", "0000:03:00.0", "1500000000"));
    put(root2 / "proc/self/fdinfo/4", client("5", "0000:03:00.0", "1500000000"));
    assert(near(engines.sample(101000000000ULL, device).gpu_percent, 50));  // 0.5 s busy in 1 s, counted once
    // Descriptors of one client always show the same counters.
    for (const char *fd : {"3", "4"})
      put(root2 / "proc/self/fdinfo" / fd, client("5", "0000:03:00.0", "2000000000", "drm-engine-capacity-render: 2\n"));
    assert(near(engines.sample(102000000000ULL, device).gpu_percent, 25));  // two render engines share the time
    for (const char *fd : {"3", "4"})
      put(root2 / "proc/self/fdinfo" / fd, client("5", "0000:03:00.0", "9000000000", "drm-engine-capacity-render: 1\n"));
    assert(near(engines.sample(103000000000ULL, device).gpu_percent, 100));  // never above 100
    fs::remove_all(root2);
  }

  // Intel: the package sensor wins over per-core ones.
  fs::remove_all(root / "sys");
  put(root / "sys/class/hwmon/hwmon2/name", "coretemp\n");
  put(root / "sys/class/hwmon/hwmon2/temp1_label", "Core 0\n");
  put(root / "sys/class/hwmon/hwmon2/temp1_input", "50000\n");
  put(root / "sys/class/hwmon/hwmon2/temp2_label", "Package id 0\n");
  put(root / "sys/class/hwmon/hwmon2/temp2_input", "58000\n");
  assert(near(HostSensors(proc, sys).sample(1000000000ULL, "").cpu_temp, 58));
  // A thermal zone is the fallback; nothing at all stays unavailable.
  fs::remove_all(root / "sys");
  put(root / "sys/class/thermal/thermal_zone3/type", "x86_pkg_temp\n");
  put(root / "sys/class/thermal/thermal_zone3/temp", "44000\n");
  assert(near(HostSensors(proc, sys).sample(1000000000ULL, "").cpu_temp, 44));
  fs::remove_all(root / "sys");
  assert(HostSensors(proc, sys).sample(1000000000ULL, "").cpu_temp < 0);
  fs::remove_all(root);

  // The GPU in use is the device whose DRM clients hold the most memory, each client counted once.
  {
    using arm64emu::window_stats::detail::DrmClient;
    using arm64emu::window_stats::detail::pick_gpu_device;
    DrmClient render{"0000:09:00.0", "5", 3000, true}, scanout{"0000:07:00.0", "6", 100, true};
    assert(pick_gpu_device({}) == "");
    assert(pick_gpu_device({render}) == "0000:09:00.0");
    assert(pick_gpu_device({scanout, render, render}) == "0000:09:00.0");  // a duplicate fd is not double usage
    DrmClient unknown{"0000:07:00.0", "7", 0, false}, anonymous{"0000:05:00.0", "", 9999, true};
    assert(pick_gpu_device({unknown}) == "0000:07:00.0");                  // unknown usage still names a device
    assert(pick_gpu_device({anonymous, scanout}) == "0000:07:00.0");       // a node without a client id is ignored
  }

  // Text.
  assert(detail::short_cpu_name("AMD Ryzen 7 5800X3D 8-Core Processor") == "AMD Ryzen 7 5800X3D");
  assert(detail::short_cpu_name("Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz") == "Intel Core i7-9700K");
  assert(detail::short_gpu_name("AMD Radeon RX 7600 (radeonsi, navi33, LLVM 21.1.4, DRM 3.64)") == "AMD Radeon RX 7600");
  assert(detail::short_gpu_name("NVIDIA GeForce RTX 3060/PCIe/SSE2") == "NVIDIA GeForce RTX 3060");
  assert(detail::short_gpu_name("llvmpipe (LLVM 20.1.2, 256 bits)") == "llvmpipe");
  // Case kept; accents dropped; dashes and quotes as ASCII; the degree sign, middle dot and ellipsis drawn.
  assert(detail::overlay_text("Pok\xC3\xA9mon \xC2\xB0 \xE2\x82\xAC x\x01") == "Pokemon \xC2\xB0 ? x ");
  assert(detail::overlay_text("A \xE2\x80\x94 \xE2\x80\x9C" "b\xE2\x80\x9D \xC2\xB7 \xE2\x80\xA6 \xE3\x82\xA6") ==
         "A - \"b\" \xC2\xB7 \xE2\x80\xA6 ?");
  assert(detail::glyph_columns('g')[0] & 0x100 && !(detail::glyph_columns('G')[0] & 0x180));  // the 5x7 font's descenders
  OverlayStats stats;
  stats.version = "2.0.0-beta.1"; stats.app = "Subway Surfers"; stats.backend = "Android EGL";
  stats.fps = 60; stats.active = true; stats.width = 1440; stats.height = 1384; stats.frames = 123456;
  stats.sensors.cpu_percent = 34.4; stats.sensors.process_percent = 12.6; stats.sensors.cpu_temp = 72;
  stats.sensors.gpu_percent = 58; stats.sensors.gpu_temp = 64;
  stats.sensors.ram_used = 12300000000ULL; stats.sensors.ram_total = 31300000000ULL; stats.sensors.ram_known = true;
  stats.emulator_ram = 2100000000ULL; stats.vram_used = 1200000000ULL; stats.vram_total = 8176000000ULL;
  stats.vram_known = true;
  stats.cpu_name = "AMD Ryzen 7 5800X3D 8-Core Processor";
  stats.gpu_name = "AMD Radeon RX 7600 (radeonsi, navi33)";
  stats.frame_ms = {16.0f, 20.0f, 12.5f};
  const std::string deg = "\xC2\xB0" "C";
  const auto text = flat(overlay_rows(stats));
  assert(text ==
         "Android EGL|60FPS|16.7ms\n"
         "GPU|58%|64" + deg + "\n"
         "CPU|34%|72" + deg + "\n"
         "VRAM|1.2GB|/8.2GB\n"
         "RAM|12.3GB|/31.3GB\n"
         "\n"
         "Frametime\tmin 12.5 \xC2\xB7 max 20.0 ms\n"
         "[graph 3]\n"
         "\n"
         "Subway Surfers\n"
         "Emulator 13% CPU \xC2\xB7 2.1 GB\n"
         "AMD Radeon RX 7600\t2.0.0-beta.1\n");
  {
    // MangoHud's colours: a label per metric, white figures that turn amber from 80 percent and red from 95
    // (temperatures at 80 and 90 degrees).
    const auto rows = overlay_rows(stats);
    assert(find_row(rows, "GPU")->cells[0].color == 0xFF2E9762 && find_row(rows, "CPU")->cells[0].color == 0xFF2E97CB);
    assert(find_row(rows, "VRAM")->cells[0].color == 0xFFAD64C1 && find_row(rows, "RAM")->cells[0].color == 0xFFC26693);
    assert(find_row(rows, "GPU")->cells[1].color == 0xFFFFFFFF);
    OverlayStats hot = stats;
    hot.sensors.gpu_percent = 97; hot.sensors.gpu_temp = 85; hot.sensors.cpu_percent = 85; hot.sensors.cpu_temp = 91;
    const auto loaded = overlay_rows(hot);
    assert(find_row(loaded, "GPU")->cells[1].color == 0xFFFF4D4D && find_row(loaded, "GPU")->cells[2].color == 0xFFFDFD09);
    assert(find_row(loaded, "CPU")->cells[1].color == 0xFFFDFD09 && find_row(loaded, "CPU")->cells[2].color == 0xFFFF4D4D);
  }
  {
    // The title comes from the app and is cut to the panel's width when drawn; one with nothing printable is left out.
    OverlayStats named = stats;
    named.app = "\xE3\x82\xA6\xE3\x83\x9E\xE5\xA8\x98";
    assert(flat(overlay_rows(named)).find("\n\nEmulator") != std::string::npos);
    assert(overlay_rows(named).size() == overlay_rows(stats).size() - 1);
  }
  OverlayStats idle = stats;
  idle.active = false; idle.sensors = SensorReading{}; idle.vram_known = false; idle.app.clear();
  idle.emulator_ram = 0; idle.gpu_name = "llvmpipe (LLVM 20.1.2, 256 bits)"; idle.frame_ms.clear();
  assert(flat(overlay_rows(idle)) ==
         "Android EGL|idle|\n"
         "GPU|--|--\n"
         "CPU|--|--\n"
         "VRAM|--|\n"
         "RAM|--|\n"
         "\n"
         "Frametime\t--\n"
         "[graph 0]\n"
         "\n"
         "Emulator --% CPU \xC2\xB7 --\n"
         "llvmpipe\t2.0.0-beta.1\n");

  // Text measures per glyph: digits share one width (so figures align), and a wider letter is wider.
  assert(near(detail::text_width("0", 20), detail::text_width("7", 20)));
  assert(detail::text_width("WWW", 20) > detail::text_width("iii", 20) * 2);
  assert(near(detail::text_width("ab", 40), 2 * detail::text_width("ab", 20)));

  // Rendering: translucent rounded panel, outlined text, a green frame-time line.
  assert(overlay_font_px(720) == 16 && overlay_font_px(1384) == 25 && overlay_font_px(2160) == 39 &&
         overlay_font_px(99999) == 40 && overlay_font_px(100) == 16);
  const auto image = render_overlay(overlay_rows(stats), 24);
  assert(image.w > 200 && image.h > 200 && image.argb.size() == size_t(image.w) * image.h);
  assert(image.argb[0] == 0 && image.argb[image.w - 1] == 0 && image.argb.back() == 0);  // rounded corners
  const uint32_t panel = image.argb[size_t(image.h / 2) * image.w + 1];
  assert((panel >> 24) >= 0x70 && (panel >> 24) <= 0x90);  // MangoHud's 50% background
  assert(has_pixel(image, [](uint32_t p) { return p == 0xFFFFFFFF; }));                                  // white text
  assert(has_pixel(image, [](uint32_t p) { return (p >> 24) == 0xFF && (p & 0xFF) < 0x40 && ((p >> 8) & 0xFF) > 0xC0; }));  // frame line
  assert(!has_pixel(render_overlay(overlay_rows(idle), 24),
                    [](uint32_t p) { return (p >> 24) == 0xFF && (p & 0xFF) < 0x40 && ((p >> 8) & 0xFF) > 0xC0 && ((p >> 16) & 0xFF) < 0x40; }));
  // A larger font makes a larger panel in proportion.
  assert(render_overlay(overlay_rows(stats), 48).w > 18 * image.w / 10 && render_overlay(overlay_rows(stats), 48).w < 22 * image.w / 10);
  {
    // The panel keeps its width as the figures change, and a long title is cut instead of widening it.
    OverlayStats slow = stats, fast = stats, long_title = stats;
    slow.fps = 9; fast.fps = 144; long_title.app = "The Quite Unreasonably Long Game Title Of Doom";
    assert(render_overlay(overlay_rows(slow), 24).w == image.w && render_overlay(overlay_rows(fast), 24).w == image.w);
    assert(render_overlay(overlay_rows(long_title), 24).w == image.w);
    assert(render_overlay(overlay_rows(idle), 24).w == image.w);
  }
  // Unknown characters still draw ('?'), and a graph with no room or a single sample draws nothing and does not fault.
  {
    OverlayRow row;
    row.cells = {{"a~\xC3\xA9\x01", "", 0xFFFFFFFF, "", ""}, {"1", "ms", 0xFFFFFFFF, "", ""}};
    OverlayRow graph;
    graph.kind = OverlayRow::Graph;
    graph.samples = {5.0f};
    assert(render_overlay({row, graph}, 20).w > 0);
    graph.samples.assign(1000, 99999.0f);  // more than the history, far above the top
    assert(render_overlay({row, graph}, 20).h > 0);
  }
  return 0;
}
