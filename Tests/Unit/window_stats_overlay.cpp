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
// One text line per panel line; a tab marks where the runs flush with the right edge begin.
static std::string flat(const std::vector<OverlayLine> &lines) {
  std::string text;
  for (const auto &line : lines) {
    bool right = false;
    for (const auto &run : line) {
      if (run.right && !right) text += '\t';
      right |= run.right;
      text += run.text;
    }
    text += '\n';
  }
  return text;
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
  assert(detail::glyph_columns('g')[0] & 0x100 && !(detail::glyph_columns('G')[0] & 0x180));  // descenders
  OverlayStats stats;
  stats.version = "2.0.0-alpha.2"; stats.app = "Subway Surfers"; stats.backend = "Android EGL";
  stats.fps = 60; stats.active = true; stats.width = 1440; stats.height = 1384; stats.frames = 123456;
  stats.sensors.cpu_percent = 34.4; stats.sensors.process_percent = 12.6; stats.sensors.cpu_temp = 72;
  stats.sensors.gpu_percent = 58; stats.sensors.gpu_temp = 64;
  stats.sensors.ram_used = 12300000000ULL; stats.sensors.ram_total = 31300000000ULL; stats.sensors.ram_known = true;
  stats.process_rss = 2100000000ULL; stats.vram_used = 1200000000ULL; stats.vram_total = 8176000000ULL;
  stats.vram_known = true;
  stats.cpu_name = "AMD Ryzen 7 5800X3D 8-Core Processor";
  stats.gpu_name = "AMD Radeon RX 7600 (radeonsi, navi33)";
  // A row is a 4-character label, a 4-character percentage, a space, an 8-cell meter and a detail at the right edge.
  const auto row = [](const char *label, const char *percent, const std::string &detail) {
    return std::string("\n") + label + percent + std::string(9, ' ') + "\t" + detail + "\n";
  };
  const std::string deg = "\xC2\xB0" "C";
  const auto text = flat(overlay_lines(stats));
  assert(text.find("BIFROST-EMU\t2.0.0-alpha.2\nSubway Surfers\n60 FPS  16.7 ms\tAndroid EGL\n\n") == 0);
  assert(overlay_lines(stats)[2][0].size == 2);  // the frame rate is the big figure
  assert(text.find(row("CPU ", " 34%", "72" + deg)) != std::string::npos);
  assert(text.find(row("GPU ", " 58%", "64" + deg)) != std::string::npos);
  assert(text.find(row("VRAM", " 15%", "1.2/8.2 GB")) != std::string::npos);
  assert(text.find(row("RAM ", " 39%", "12.3/31.3 GB")) != std::string::npos);
  assert(text.find("\n\nEmulator 13% CPU \xC2\xB7 2.1 GB\nAMD Radeon RX 7600\n") != std::string::npos);
  {
    // A long title is cut to the panel's 31 characters with an ellipsis; one with nothing printable is left out.
    OverlayStats named = stats;
    named.app = "The Quite Unreasonably Long Game Title Of Doom";
    assert(flat(overlay_lines(named)).find("\nThe Quite Unreasonably Long Ga\xE2\x80\xA6\n") != std::string::npos);
    named.app = "\xE3\x82\xA6\xE3\x83\x9E\xE5\xA8\x98";
    assert(overlay_lines(named).size() == overlay_lines(stats).size() - 1);
  }
  stats.active = false; stats.sensors = SensorReading{}; stats.vram_known = false; stats.app.clear();
  stats.process_rss = 0; stats.gpu_name = "llvmpipe (LLVM 20.1.2, 256 bits)";
  const auto idle = flat(overlay_lines(stats));
  assert(idle.find("\nIDLE\tAndroid EGL\n") != std::string::npos);
  assert(idle.find(row("CPU ", " --%", "--" + deg)) != std::string::npos);
  assert(idle.find(row("VRAM", " --%", "--")) != std::string::npos);
  assert(idle.find(row("RAM ", " --%", "--")) != std::string::npos);
  assert(idle.find("\n\nEmulator --% CPU \xC2\xB7 --\nllvmpipe\n") != std::string::npos);
  // A meter row draws a bar (the track plus a fill in the colour for its load), not glyphs.
  {
    stats.sensors.cpu_percent = 50; stats.sensors.gpu_percent = 96;
    const auto rows = overlay_lines(stats);
    const OverlayLine *cpu = nullptr, *gpu = nullptr;
    for (const auto &line : rows) {
      if (line.size() > 1 && line[0].text.rfind("CPU", 0) == 0) cpu = &line;
      if (line.size() > 1 && line[0].text.rfind("GPU", 0) == 0) gpu = &line;
    }
    assert(cpu && gpu && cpu->at(3).meter > 0.49 && cpu->at(3).meter < 0.51 && gpu->at(3).color != cpu->at(3).color);
    assert(cpu->at(3).text.size() == 8 && cpu->at(3).meter <= 1);
  }

  // Rendering: small, translucent, rounded, text opaque.
  assert(overlay_scale(720) == 2 && overlay_scale(1384) == 2 && overlay_scale(2160) == 3 && overlay_scale(99999) == 5);
  const std::vector<OverlayLine> lines = {{{"AB", 0xFFFFFFFF}}, {{"C", 0xFF00FF00}}};
  const auto image = render_overlay(lines, 2);
  assert(image.w == 2 * 12 - 2 + 20 && image.h == 2 * 20 - 6 + 16 && image.argb.size() == size_t(image.w) * image.h);
  assert(image.argb[0] == 0 && image.argb[image.w - 1] == 0 && image.argb.back() == 0);  // rounded corners
  const uint32_t panel = image.argb[size_t(image.h / 2) * image.w + image.w - 2];
  assert((panel >> 24) > 0x40 && (panel >> 24) < 0xE0);  // translucent, not clear and not solid
  bool white = false, green = false;
  for (auto pixel : image.argb) { white |= pixel == 0xFFFFFFFF; green |= pixel == 0xFF00FF00; }
  assert(white && green);
  // Unknown characters still draw: no out-of-range glyph access.
  assert(render_overlay({{{"a~\xC3\xA9\x01", 0xFFFFFFFF}}}, 1).w == 4 * 6 - 1 + 10);
  {
    // A right run ends at the right padding; a double-size run makes its line taller by one capital height.
    OverlayRun b{"B", 0xFF00FF00};
    b.right = true;
    const auto split = render_overlay({{{"A", 0xFFFFFFFF}, b}}, 1);
    assert(split.w == 6 + 12 + 6 - 1 + 10);
    for (int y = 4; y < 4 + 7; ++y) assert(split.argb[size_t(y) * split.w + split.w - 5 - 5] == 0xFF00FF00);
    OverlayRun big{"8", 0xFFFFFFFF};
    big.size = 2;
    assert(render_overlay({{big}}, 1).h == render_overlay({{{"8", 0xFFFFFFFF}}}, 1).h + 7);
  }
  return 0;
}
