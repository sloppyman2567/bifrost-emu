// Host sensors (against fixture /proc and /sys trees) and the in-window stats panel.
#include "System/Graphics/Display/HostSensors.hpp"
#include "System/Graphics/Display/StatsOverlay.hpp"
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
static std::string flat(const std::vector<OverlayLine> &lines) {
  std::string text;
  for (const auto &line : lines) {
    for (const auto &run : line) text += run.text;
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
  HostSensors sensors(proc, sys);
  auto first = sensors.sample(10000000000ULL, gpu);
  assert(first.cpu_percent < 0 && first.process_percent < 0);  // a rate needs two samples
  assert(near(first.cpu_temp, 65.5) && near(first.gpu_temp, 63) && near(first.gpu_percent, 41));
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

  // Text.
  assert(detail::short_cpu_name("AMD Ryzen 7 5800X3D 8-Core Processor") == "AMD Ryzen 7 5800X3D");
  assert(detail::short_cpu_name("Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz") == "Intel Core i7-9700K");
  assert(detail::short_gpu_name("AMD Radeon RX 7600 (radeonsi, navi33, LLVM 21.1.4, DRM 3.64)") == "AMD Radeon RX 7600");
  assert(detail::short_gpu_name("NVIDIA GeForce RTX 3060/PCIe/SSE2") == "NVIDIA GeForce RTX 3060");
  assert(detail::short_gpu_name("llvmpipe (LLVM 20.1.2, 256 bits)") == "llvmpipe");
  assert(detail::overlay_text("Pok\xC3\xA9mon \xC2\xB0 \xE2\x82\xAC x") == "POK?MON \xC2\xB0 ? X");
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
  const auto text = flat(overlay_lines(stats));
  assert(text.find("BIFROST-EMU 2.0.0-ALPHA.2\nSUBWAY SURFERS\nANDROID EGL  60 FPS  16.7 MS\n") == 0);
  assert(text.find("1440X1384  FRAME 123456\n") != std::string::npos);
  assert(text.find("CPU 34%  72\xC2\xB0" "C  EMU 13%\n") != std::string::npos);
  assert(text.find("GPU 58%  64\xC2\xB0" "C  VRAM 1.2/8.2 GB\n") != std::string::npos);
  assert(text.find("RAM 12.3/31.3 GB  EMU 2.1 GB\n") != std::string::npos);
  assert(text.find("AMD RYZEN 7 5800X3D\nAMD RADEON RX 7600\n") != std::string::npos);
  stats.active = false; stats.sensors = SensorReading{}; stats.vram_known = false; stats.app.clear();
  stats.gpu_name = "llvmpipe (LLVM 20.1.2, 256 bits)";
  const auto idle = flat(overlay_lines(stats));
  assert(idle.find("\nANDROID EGL  IDLE\n") != std::string::npos);
  assert(idle.find("CPU --%  --\xC2\xB0" "C  EMU --%\n") != std::string::npos);
  assert(idle.find("GPU --%  --\xC2\xB0" "C  VRAM --\n") != std::string::npos);
  assert(idle.find("RAM --  EMU 2.1 GB\n") != std::string::npos && idle.find("\nLLVMPIPE\n") != std::string::npos);

  // Rendering: small, translucent, rounded, text opaque.
  assert(overlay_scale(720) == 2 && overlay_scale(1384) == 2 && overlay_scale(2160) == 3 && overlay_scale(99999) == 5);
  const std::vector<OverlayLine> lines = {{{"AB", 0xFFFFFFFF}}, {{"C", 0xFF00FF00}}};
  const auto image = render_overlay(lines, 2);
  assert(image.w == 2 * 12 - 2 + 12 && image.h == 2 * 18 - 4 + 12 && image.argb.size() == size_t(image.w) * image.h);
  assert(image.argb[0] == 0 && image.argb[image.w - 1] == 0 && image.argb.back() == 0);  // rounded corners
  const uint32_t panel = image.argb[size_t(image.h / 2) * image.w + image.w - 2];
  assert((panel >> 24) > 0x40 && (panel >> 24) < 0xE0);  // translucent, not clear and not solid
  bool white = false, green = false;
  for (auto pixel : image.argb) { white |= pixel == 0xFFFFFFFF; green |= pixel == 0xFF00FF00; }
  assert(white && green);
  // Unknown and lower-case characters still draw: no out-of-range glyph access.
  assert(render_overlay({{{"a~\xC3\xA9\x01", 0xFFFFFFFF}}}, 1).w == 4 * 6 - 1 + 6);
  return 0;
}
