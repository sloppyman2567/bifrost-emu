#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace arm64emu::window_stats {
// Host load and temperature readings for the stats overlay. Everything comes
// from generic Linux interfaces (/proc, hwmon, thermal zones, DRM sysfs), with
// no vendor tools or libraries; a missing source stays unavailable.
struct SensorReading {
  static constexpr double unavailable = -1;
  double cpu_percent = unavailable;      // all CPUs, from /proc/stat
  double process_percent = unavailable;  // this process, as a share of all CPUs
  double cpu_temp = unavailable;         // degrees Celsius
  double gpu_percent = unavailable;
  double gpu_temp = unavailable;
  uint64_t ram_used = 0, ram_total = 0;  // bytes; used = MemTotal - MemAvailable
  bool ram_known = false;
  // The GPU's own memory counters (amdgpu: mem_info_vram_*), for the whole machine rather than this process.
  uint64_t vram_used = 0, vram_total = 0;
  bool vram_known = false;
  std::string gpu_driver;  // kernel driver of the GPU device ("amdgpu", "i915", ...), empty when unknown
};
namespace detail {
struct CpuTimes {
  uint64_t busy = 0, total = 0;
};
// First "cpu " line of /proc/stat: busy excludes idle and iowait.
bool parse_cpu_stat(const std::string &text, CpuTimes &times);
// utime + stime in clock ticks from /proc/PID/stat (the command name may contain spaces and parentheses).
bool parse_process_ticks(const std::string &text, uint64_t &ticks);
// One DRM client's engine counters from /proc/PID/fdinfo (Linux "DRM client usage stats"): cumulative busy
// nanoseconds per engine (`drm-engine-<name>`) and how many engines share a name (`drm-engine-capacity-<name>`).
struct DrmEngines {
  std::string client, pdev;
  std::map<std::string, uint64_t> busy_ns, capacity;
};
// False when the text is not a DRM client (it has no drm-client-id).
bool parse_drm_engines(const std::string &fdinfo, DrmEngines &engines);
// Whole degrees plausible for a die sensor, from a hwmon/thermal value in millidegrees.
bool plausible_temperature(long long millidegrees, double &celsius);
// Parent pid from /proc/PID/stat.
bool parse_parent_pid(const std::string &stat, long &parent);
// A "Name: N kB" field of /proc/PID/smaps_rollup or /proc/PID/status, in bytes. `field` includes the colon.
bool parse_kb_field(const std::string &text, const char *field, uint64_t &bytes);
// The Binder realm socket named by a /proc/PID/cmdline (NUL-separated arguments); empty outside a realm.
std::string realm_socket(const std::string &cmdline);
}  // namespace detail
// Memory of the emulator session `self` belongs to, in bytes: its Binder realm (broker, services, every app and
// helper) when it has one, and each member's descendants. Each process counts its proportional share (PSS), so
// pages they share are counted once; VmRSS stands in where smaps_rollup cannot be read. Zero when nothing can be
// read. Walks every member's page tables (tens of ms for a large game), so never call it on a render path.
uint64_t session_memory(const std::string &proc_root, long self);
class HostSensors {
public:
  // Roots exist so tests can read a fixture tree.
  explicit HostSensors(std::string proc_root = "/proc", std::string sys_root = "/sys");
  // Readings at most once per second. `gpu_device` is the rendering GPU's sysfs
  // device directory (empty when unknown, which leaves the GPU readings unavailable).
  const SensorReading &sample(uint64_t now_ns, const std::string &gpu_device);

private:
  std::string read(const std::string &path) const;
  std::string cpu_sensor_path();
  std::string gpu_sensor_path(const std::string &device);
  double temperature(const std::string &path);
  std::string proc_, sys_;
  SensorReading reading_;
  uint64_t sample_ns_ = 0;
  detail::CpuTimes last_cpu_;
  uint64_t last_ticks_ = 0, last_ticks_ns_ = 0;
  bool have_cpu_ = false, have_ticks_ = false;
  double engine_percent(uint64_t now_ns, const std::string &gpu_device);
  std::string cpu_path_, gpu_path_, gpu_for_;
  bool cpu_scanned_ = false;
  std::map<std::string, uint64_t> last_engine_ns_;
  uint64_t last_engine_time_ = 0;
};
}  // namespace arm64emu::window_stats
