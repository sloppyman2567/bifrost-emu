#include "System/Graphics/Display/HostSensors.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <unistd.h>

namespace arm64emu::window_stats {
namespace detail {
bool parse_cpu_stat(const std::string &text, CpuTimes &times) {
  std::istringstream input(text);
  std::string label;
  if (!(input >> label) || label != "cpu") return false;
  uint64_t value[8] = {};
  int count = 0;
  while (count < 8 && input >> value[count]) ++count;
  if (count < 4) return false;  // user nice system idle are the minimum
  uint64_t total = 0;
  for (int i = 0; i < count; ++i) total += value[i];
  const uint64_t idle = value[3] + (count > 4 ? value[4] : 0);  // idle + iowait
  times.total = total;
  times.busy = total - std::min(total, idle);
  return true;
}
bool parse_process_ticks(const std::string &text, uint64_t &ticks) {
  // "pid (comm) state ppid ... utime stime": comm may hold anything, so count from the last ')'.
  const auto close = text.rfind(')');
  if (close == std::string::npos) return false;
  std::istringstream input(text.substr(close + 1));
  std::string field;
  uint64_t utime = 0, stime = 0;
  for (int index = 3; input >> field; ++index) {  // the first field after ')' is field 3, state
    if (index == 14 || index == 15) {
      char *end = nullptr;
      const auto value = strtoull(field.c_str(), &end, 10);
      if (end == field.c_str() || *end) return false;
      (index == 14 ? utime : stime) = value;
      if (index == 15) { ticks = utime + stime; return true; }
    }
  }
  return false;
}
bool plausible_temperature(long long millidegrees, double &celsius) {
  if (millidegrees < 1000 || millidegrees > 150000) return false;  // 0 means not powered or unsupported
  celsius = millidegrees / 1000.0;
  return true;
}
}  // namespace detail

static std::vector<std::string> list(const std::string &directory) {
  std::vector<std::string> names;
  if (DIR *dir = opendir(directory.c_str())) {
    while (const auto *entry = readdir(dir))
      if (entry->d_name[0] != '.') names.push_back(entry->d_name);
    closedir(dir);
  }
  std::sort(names.begin(), names.end());
  return names;
}
static std::string trimmed(std::string text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ' || text.back() == '\r')) text.pop_back();
  return text;
}
static bool has_prefix(const std::string &text, const char *prefix) { return text.rfind(prefix, 0) == 0; }

HostSensors::HostSensors(std::string proc_root, std::string sys_root)
    : proc_(std::move(proc_root)), sys_(std::move(sys_root)) {}

std::string HostSensors::read(const std::string &path) const {
  std::ifstream file(path);
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (text.size() > 65536) text.resize(65536);
  return text;
}

// The temperature file of a hwmon directory: the first label in `labels` that exists, else the first input.
static std::string pick_hwmon_input(const std::string &hwmon, const std::vector<const char *> &labels,
                                    const std::function<std::string(const std::string &)> &read) {
  const auto files = list(hwmon);
  for (const char *wanted : labels)
    for (const auto &name : files) {
      if (!has_prefix(name, "temp") || name.size() < 11 || name.compare(name.size() - 6, 6, "_label")) continue;
      if (trimmed(read(hwmon + "/" + name)) == wanted)
        return hwmon + "/" + name.substr(0, name.size() - 6) + "_input";
    }
  for (const auto &name : files)
    if (has_prefix(name, "temp") && name.size() > 6 && !name.compare(name.size() - 6, 6, "_input"))
      return hwmon + "/" + name;
  return {};
}

std::string HostSensors::cpu_sensor_path() {
  const auto reader = [this](const std::string &path) { return read(path); };
  // Package-level sensors by driver: AMD (k10temp, zenpower), Intel (coretemp), common ARM boards.
  static const char *drivers[] = {"k10temp", "zenpower", "coretemp", "cpu_thermal", "soc_thermal",
                                  "cpu-thermal", "acpitz"};
  static const std::vector<const char *> labels = {"Tdie", "Package id 0", "Tctl", "CPU", "SoC"};
  const auto hwmons = list(sys_ + "/class/hwmon");
  for (const char *driver : drivers)
    for (const auto &hwmon : hwmons) {
      const std::string base = sys_ + "/class/hwmon/" + hwmon;
      if (trimmed(read(base + "/name")) != driver) continue;
      auto path = pick_hwmon_input(base, labels, reader);
      if (!path.empty()) return path;
    }
  for (const auto &zone : list(sys_ + "/class/thermal")) {
    if (!has_prefix(zone, "thermal_zone")) continue;
    const std::string type = trimmed(read(sys_ + "/class/thermal/" + zone + "/type"));
    if (type == "x86_pkg_temp" || type == "cpu-thermal" || type == "soc-thermal")
      return sys_ + "/class/thermal/" + zone + "/temp";
  }
  return {};
}

std::string HostSensors::gpu_sensor_path(const std::string &device) {
  const auto reader = [this](const std::string &path) { return read(path); };
  static const std::vector<const char *> labels = {"edge"};
  for (const auto &hwmon : list(device + "/hwmon")) {
    auto path = pick_hwmon_input(device + "/hwmon/" + hwmon, labels, reader);
    if (!path.empty()) return path;
  }
  return {};
}

double HostSensors::temperature(const std::string &path) {
  if (path.empty()) return SensorReading::unavailable;
  double celsius = 0;
  const auto text = trimmed(read(path));
  char *end = nullptr;
  const long long value = strtoll(text.c_str(), &end, 10);
  if (!text.empty() && !*end && detail::plausible_temperature(value, celsius)) return celsius;
  return SensorReading::unavailable;
}

const SensorReading &HostSensors::sample(uint64_t now_ns, const std::string &gpu_device) {
  if (sample_ns_ && now_ns - sample_ns_ < 1000000000ULL) return reading_;
  SensorReading next;
  detail::CpuTimes cpu;
  if (detail::parse_cpu_stat(read(proc_ + "/stat"), cpu)) {
    if (have_cpu_ && cpu.total > last_cpu_.total)
      next.cpu_percent = 100.0 * double(cpu.busy - std::min(cpu.busy, last_cpu_.busy)) / double(cpu.total - last_cpu_.total);
    last_cpu_ = cpu;
    have_cpu_ = true;
  }
  uint64_t ticks = 0;
  if (detail::parse_process_ticks(read(proc_ + "/self/stat"), ticks)) {
    const long rate = sysconf(_SC_CLK_TCK), cpus = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    if (have_ticks_ && now_ns > last_ticks_ns_ && rate > 0 && ticks >= last_ticks_)
      next.process_percent = 100.0 * (double(ticks - last_ticks_) / double(rate)) /
                             ((now_ns - last_ticks_ns_) / 1e9) / double(cpus);
    last_ticks_ = ticks; last_ticks_ns_ = now_ns; have_ticks_ = true;
  }
  uint64_t total = 0, available = 0;
  std::istringstream meminfo(read(proc_ + "/meminfo"));
  for (std::string key, unit; meminfo >> key;) {
    uint64_t kb = 0;
    if (!(meminfo >> kb)) { meminfo.clear(); }
    std::getline(meminfo, unit);
    if (key == "MemTotal:") total = kb * 1024;
    else if (key == "MemAvailable:") available = kb * 1024;
  }
  if (total && available <= total) { next.ram_total = total; next.ram_used = total - available; next.ram_known = true; }
  if (!cpu_scanned_) { cpu_path_ = cpu_sensor_path(); cpu_scanned_ = true; }
  next.cpu_temp = temperature(cpu_path_);
  if (gpu_device != gpu_for_) { gpu_path_ = gpu_device.empty() ? std::string() : gpu_sensor_path(gpu_device); gpu_for_ = gpu_device; }
  if (!gpu_device.empty()) {
    next.gpu_temp = temperature(gpu_path_);
    const auto busy = trimmed(read(gpu_device + "/gpu_busy_percent"));  // amdgpu
    char *end = nullptr;
    const long value = strtol(busy.c_str(), &end, 10);
    if (!busy.empty() && !*end && value >= 0 && value <= 100) next.gpu_percent = double(value);
  }
  reading_ = next;
  sample_ns_ = now_ns;
  return reading_;
}
}  // namespace arm64emu::window_stats
