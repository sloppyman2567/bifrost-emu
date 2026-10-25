#pragma once
#include <cstdint>
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
};
namespace detail {
struct CpuTimes {
  uint64_t busy = 0, total = 0;
};
// First "cpu " line of /proc/stat: busy excludes idle and iowait.
bool parse_cpu_stat(const std::string &text, CpuTimes &times);
// utime + stime in clock ticks from /proc/PID/stat (the command name may contain spaces and parentheses).
bool parse_process_ticks(const std::string &text, uint64_t &ticks);
// Whole degrees plausible for a die sensor, from a hwmon/thermal value in millidegrees.
bool plausible_temperature(long long millidegrees, double &celsius);
}  // namespace detail
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
  std::string cpu_path_, gpu_path_, gpu_for_;
  bool cpu_scanned_ = false;
};
}  // namespace arm64emu::window_stats
