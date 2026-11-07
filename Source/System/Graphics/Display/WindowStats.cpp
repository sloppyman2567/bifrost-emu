#include "System/Graphics/Display/WindowStats.hpp"
#include "Bifrost/Version.hpp"
#include "System/Graphics/Display/HostSensors.hpp"
#include "System/Core/DebugFlags.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <spawn.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <system_error>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "../../../../ThirdParty/VulkanHeaders/include/vulkan/vulkan_core.h"
#if defined(BIFROST_USE_SDL2)
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#endif
extern char **environ;
namespace arm64emu::window_stats {
static uint64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void Clock::present(uint64_t now) {
  if (!frames || now - last_ns > 2000000000ULL) {
    sample_ns = now;
    sample_frames = frames + 1;
    fps = 0;
  }
  last_ns = now;
  ++frames;
  const uint64_t elapsed = now - sample_ns;
  if (elapsed >= 1000000000ULL) {
    // The first presentation starts the sample, rather than contributing
    // an extra interval. Subsequent windows count every new presentation.
    fps = (frames - sample_frames) * 1e9 / elapsed;
    sample_frames = frames;
    sample_ns = now;
  }
}
double Clock::active_fps(uint64_t now) const {
  return frames && now - last_ns <= 2000000000ULL ? fps : 0;
}
struct Packet {
  uint64_t frames{}, last_ns{}, xwindow{};
  uint64_t ram{}, total_ram{}, vram{}, used_vram{}, used_ram{};
  bool vram_known{};
  double fps{};
  double cpu_percent{-1}, cpu_temp{-1}, gpu_percent{-1}, gpu_temp{-1};
  int x{}, y{}, w{}, h{}, visible{}, separate{}, overlay{};
  char backend[32]{};
  char cpu[512]{}, gpu[512]{};
  // The rest of the panel's figures (StatsOverlay), so the desktop HUD shows what a native window draws.
  double process_percent{-1};
  uint64_t panel_vram_used{}, panel_vram_total{};
  bool panel_vram_known{}, ram_known{};
  char app[160]{}, panel_gpu[160]{};
};

static OverlayStats overlay_stats(const Packet &p, bool active) {
  OverlayStats stats;
  stats.version = VERSION; stats.app = p.app; stats.backend = p.backend; stats.gpu_name = p.panel_gpu;
  stats.fps = p.fps; stats.active = active;
  stats.width = p.w; stats.height = p.h; stats.frames = p.frames;
  stats.sensors.cpu_percent = p.cpu_percent; stats.sensors.cpu_temp = p.cpu_temp;
  stats.sensors.gpu_percent = p.gpu_percent; stats.sensors.gpu_temp = p.gpu_temp;
  stats.sensors.process_percent = p.process_percent;
  stats.sensors.ram_known = p.ram_known; stats.sensors.ram_used = p.used_ram; stats.sensors.ram_total = p.total_ram;
  stats.emulator_ram = p.ram;
  stats.vram_known = p.panel_vram_known; stats.vram_used = p.panel_vram_used; stats.vram_total = p.panel_vram_total;
  return stats;
}

detail::DrmClient detail::read_drm_client(std::istream &input) {
  DrmClient result;
  std::map<std::string, uint64_t> resident, legacy;
  std::string line;
  while (std::getline(input, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    const auto key = line.substr(0, colon);
    std::istringstream value(line.substr(colon + 1));
    if (key == "drm-client-id") { value >> result.id; continue; }
    if (key == "drm-pdev") { value >> result.device; continue; }
    const bool old = key.compare(0, 11, "drm-memory-") == 0;
    const bool current = key.compare(0, 13, "drm-resident-") == 0;
    if (!old && !current) continue;
    const auto region = key.substr(old ? 11 : 13);
    if (region.compare(0, 4, "vram") ||
        region.find_first_not_of("0123456789", 4) != std::string::npos) continue;
    std::string number, unit, extra;
    value >> number;
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos) continue;
    errno = 0;
    char *end = nullptr;
    const auto count = strtoull(number.c_str(), &end, 10);
    if (errno || *end) continue;
    value >> unit;
    if (value >> extra) continue;
    uint64_t scale = unit.empty() ? 1 : unit == "KiB" ? 1024 : unit == "MiB" ? 1048576 : 0;
    if (!scale || count > std::numeric_limits<uint64_t>::max() / scale) continue;
    (old ? legacy : resident)[region] = count * scale;
  }
  // The deprecated amdgpu field aliases resident storage; never add both.
  for (const auto &[region, bytes] : legacy) resident.emplace(region, bytes);
  result.known = !resident.empty();
  for (const auto &[region, bytes] : resident) {
    if (bytes > std::numeric_limits<uint64_t>::max() - result.resident) {
      result.known = false; break;
    }
    result.resident += bytes;
  }
  return result;
}

bool detail::sum_drm_clients(const std::vector<DrmClient> &clients,
                            const std::string &device, uint64_t &bytes) {
  std::set<std::string> devices, seen;
  for (const auto &client : clients)
    if (!client.id.empty() && !client.device.empty()) devices.insert(client.device);
  // GLX may not expose its render node. Only infer it for a single-device process.
  const auto selected = !device.empty() ? device : devices.size() == 1 ? *devices.begin() : "";
  if (selected.empty()) return false;
  uint64_t total = 0;
  for (const auto &client : clients) {
    if (client.device != selected) continue;
    if (client.id.empty() || !client.known) return false;
    if (!seen.insert(client.id).second) continue;
    if (client.resident > std::numeric_limits<uint64_t>::max() - total) return false;
    total += client.resident;
  }
  bytes = total;
  return !seen.empty();
}

std::string detail::pick_gpu_device(const std::vector<DrmClient> &clients) {
  std::map<std::string, uint64_t> resident;
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto &client : clients) {
    if (client.id.empty() || client.device.empty() || !seen.insert({client.device, client.id}).second) continue;
    resident[client.device] += client.known ? client.resident : 0;
  }
  std::string best;
  uint64_t most = 0;
  for (const auto &[device, bytes] : resident)
    if (best.empty() || bytes > most) { best = device; most = bytes; }
  return best;
}

std::string detail::vram_text(uint64_t used, uint64_t total, bool known) {
  if (!total) return "unavailable/shared";
  char text[96];
  if (known) snprintf(text, sizeof(text), "%.0f/%.0f MB", used / 1e6, total / 1e6);
  else snprintf(text, sizeof(text), "unavailable/%.0f MB", total / 1e6);
  return text;
}

static std::string drm_device(dev_t device) {
  char path[128];
  snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device", major(device), minor(device));
  char *resolved = realpath(path, nullptr);
  std::string result = resolved ? resolved : "";
  free(resolved);
  return result;
}

static std::vector<detail::DrmClient> drm_clients() {
  std::vector<detail::DrmClient> clients;
  DIR *dir = opendir("/proc/self/fdinfo");
  if (!dir) return clients;
  while (const auto *entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    const std::string number = entry->d_name;
    struct stat st{};
    const auto fd = "/proc/self/fd/" + number;
    if (stat(fd.c_str(), &st) || !S_ISCHR(st.st_mode)) continue;
    std::ifstream file("/proc/self/fdinfo/" + number);
    auto client = detail::read_drm_client(file);
    if (client.id.empty()) continue;
    const auto canonical = drm_device(st.st_rdev);
    if (!canonical.empty()) client.device = canonical;
    clients.push_back(std::move(client));
  }
  closedir(dir);
  return clients;
}
static bool used_vram(const std::string &device, uint64_t &bytes) {
  return detail::sum_drm_clients(drm_clients(), device, bytes);
}

// These are host measurements, never guest /proc emulation. Resident memory includes
// the emulator and guest's pages, not the reserved virtual address window.
static uint64_t proc_kb(const char *path, const char *field) {
  std::ifstream file(path);
  std::string key, line;
  while (file >> key) {
    if (key == field) {
      uint64_t kb = 0;
      if (file >> kb) return kb * 1024;
      return 0;
    }
    std::getline(file, line);
  }
  return 0;
}
static std::string cpu_name() {
  std::ifstream file("/proc/cpuinfo");
  std::string line;
  while (std::getline(file, line)) {
    auto colon = line.find(':');
    if (colon != std::string::npos && line.substr(0, colon).find("model name") == 0) {
      auto start = line.find_first_not_of(" \t", colon + 1);
      if (start != std::string::npos) return line.substr(start);
    }
  }
  return "unavailable";
}

// Match an EGL renderer's DRM render node to Vulkan's physical device. Never
// report the first GPU's memory on a multi-GPU machine. Device-local heaps on
// integrated GPUs are shared system RAM, so they are not reported as VRAM.
static uint64_t egl_vram(void *egl, std::string &drm) {
  using Current = void *(*)();
  using Query = unsigned (*)(void *, int, intptr_t *);
  using String = const char *(*)(void *, int);
  auto current = reinterpret_cast<Current>(dlsym(egl, "eglGetCurrentDisplay"));
  using Proc = void *(*)(const char *);
  auto proc = reinterpret_cast<Proc>(dlsym(egl, "eglGetProcAddress"));
  auto query = reinterpret_cast<Query>(proc ? proc("eglQueryDisplayAttribEXT") : nullptr);
  auto string = reinterpret_cast<String>(proc ? proc("eglQueryDeviceStringEXT") : nullptr);
  intptr_t device = 0;
  void *display = current ? current() : nullptr;
  if (!display || !query || !string) return 0;
  using DisplayString = const char *(*)(void *, int);
  auto display_string = reinterpret_cast<DisplayString>(dlsym(egl, "eglQueryString"));
  const char *client_extensions = display_string ? display_string(nullptr, 0x3055) : nullptr;
  if (!client_extensions || (!strstr(client_extensions, "EGL_EXT_device_query") &&
                             !strstr(client_extensions, "EGL_EXT_device_base"))) return 0;
  if (!query(display, 0x322c, &device)) return 0;
  const char *device_extensions = string(reinterpret_cast<void *>(device), 0x3055);
  if (!device_extensions || !strstr(device_extensions, "EGL_EXT_device_drm_render_node")) return 0;
  const char *node = string(reinterpret_cast<void *>(device), 0x3377);
  struct stat st{};
  if (!node || stat(node, &st) || !S_ISCHR(st.st_mode)) return 0;
  drm = drm_device(st.st_rdev);
  void *vk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!vk) return 0;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(dlsym(vk, "vkCreateInstance"));
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(vk, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(dlsym(vk, "vkEnumeratePhysicalDevices"));
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dlsym(vk, "vkGetPhysicalDeviceProperties2"));
  auto memory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(dlsym(vk, "vkGetPhysicalDeviceMemoryProperties"));
  auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(dlsym(vk, "vkEnumerateDeviceExtensionProperties"));
  uint64_t bytes = 0;
  VkInstance instance{};
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo = &app;
  if (create && destroy && enumerate && properties && memory && extensions &&
      create(&info, nullptr, &instance) == VK_SUCCESS) {
    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) == VK_SUCCESS && count && count <= 64) {
      std::vector<VkPhysicalDevice> devices(count);
      if (enumerate(instance, &count, devices.data()) == VK_SUCCESS) for (auto gpu : devices) {
        uint32_t n = 0;
        if (extensions(gpu, nullptr, &n, nullptr) != VK_SUCCESS || n > 4096) continue;
        std::vector<VkExtensionProperties> names(n);
        if (extensions(gpu, nullptr, &n, names.data()) != VK_SUCCESS) continue;
        bool drm = false;
        for (const auto &name : names) drm |= !strcmp(name.extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
        if (!drm) continue;
        VkPhysicalDeviceDrmPropertiesEXT node_info{};
        node_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; props.pNext = &node_info;
        properties(gpu, &props);
        if (!node_info.hasRender || node_info.renderMajor != major(st.st_rdev) ||
            node_info.renderMinor != minor(st.st_rdev) ||
            props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        VkPhysicalDeviceMemoryProperties heaps{};
        memory(gpu, &heaps);
        for (unsigned i = 0; i < heaps.memoryHeapCount; ++i)
          if (heaps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            bytes += heaps.memoryHeaps[i].size;
        break;
      }
    }
    destroy(instance, nullptr);
  }
  dlclose(vk);
  return bytes;
}

static bool gpu_info(std::string &name, uint64_t &vram, std::string &device, bool want_vram) {
  // This runs on a presenting thread with its GL context current. Every entry
  // is resolved from the host DSO, never from guest thunks or SDL2/SDL3 globals.
  static void *gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  static void *egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  using Current = void *(*)();
  auto egl_current = egl ? reinterpret_cast<Current>(dlsym(egl, "eglGetCurrentContext")) : nullptr;
  auto glx_current = gl ? reinterpret_cast<Current>(dlsym(gl, "glXGetCurrentContext")) : nullptr;
  if (!(egl_current && egl_current()) && !(glx_current && glx_current())) return false;
  using GetString = const unsigned char *(*)(unsigned);
  auto string = gl ? reinterpret_cast<GetString>(dlsym(gl, "glGetString")) : nullptr;
  const auto *renderer = string ? string(0x1f01) : nullptr; // GL_RENDERER
  if (!renderer) return false;
  name = reinterpret_cast<const char *>(renderer);
  if (!want_vram) return true;
  if (egl) vram = egl_vram(egl, device);
  using MesaQuery = int (*)(int, unsigned *);
  auto query = gl ? reinterpret_cast<MesaQuery>(dlsym(gl, "glXQueryCurrentRendererIntegerMESA")) : nullptr;
  unsigned mb = 0, unified = 1;
  if (!vram && query && query(0x8188, &unified) && !unified && query(0x8187, &mb))
    vram = uint64_t(mb) * 1024 * 1024;
  return true;
}
// The host device a Vulkan-only guest renders through, named as its driver names it
// ("AMD Radeon RX 9060 XT (RADV GFX1200)"). Such a guest has no GL context to report a
// renderer, and the kernel driver alone ("amdgpu") does not name the card. One instance is
// created the first time a present path needs it; a host without the loader returns empty.
static std::string host_vulkan_name() {
  void *vk = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!vk) return {};
  auto create = reinterpret_cast<PFN_vkCreateInstance>(dlsym(vk, "vkCreateInstance"));
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(vk, "vkDestroyInstance"));
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(dlsym(vk, "vkEnumeratePhysicalDevices"));
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(dlsym(vk, "vkGetPhysicalDeviceProperties"));
  std::string name, first;
  VkInstance instance{};
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; info.pApplicationInfo = &app;
  if (create && destroy && enumerate && properties && create(&info, nullptr, &instance) == VK_SUCCESS) {
    uint32_t count = 0;
    if (enumerate(instance, &count, nullptr) == VK_SUCCESS && count && count <= 64) {
      std::vector<VkPhysicalDevice> devices(count);
      if (enumerate(instance, &count, devices.data()) == VK_SUCCESS) {
        // A game renders on the discrete GPU; any other host falls back to its first device.
        VkPhysicalDeviceProperties props{};
        for (auto gpu : devices) {
          properties(gpu, &props);
          if (first.empty()) first = props.deviceName;
          if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { name = props.deviceName; break; }
        }
        if (name.empty()) name = first;
      }
    }
    destroy(instance, nullptr);
  }
  dlclose(vk);
  return name;
}

struct State {
  Clock clock;
  std::string title, decorated;
  uint64_t update_ns = 0;
  int socket = -1;
  pid_t child = -1;
  bool helper_attempted = false;
  bool sdl = true;
  bool native = false;
  std::thread::id owner;
  std::string backend;
  std::string gpu;
  uint64_t vram = 0;
  std::string device;
  uint64_t used_vram = 0, vram_sample = 0;
  bool vram_known = false;
  unsigned vram_attempts = 0;  // capacity lookups (they create a Vulkan instance), bounded
  uint64_t device_ns = 0;
  OverlayImage overlay;
  uint64_t overlay_seq = 0, overlay_taken = 0;
  ~State() {
    if (socket >= 0)
      close(socket);
  }
};
static std::mutex mu;
static HostSensors host_sensors;
static std::map<void *, State> windows;
static std::map<uint64_t, void *> surfaces, swapchains;
static std::map<void *, void *> renderers;
static bool enabled() {
  const auto &f = dbg();
  return f.window_stats_title || f.window_stats_window ||
         f.window_stats_overlay;
}
static void start_helper(State &state) {
  if (state.helper_attempted)
    return;
  state.helper_attempted = true;
  // The helper is this program run again. It never starts another, so a program that is not the emulator
  // (a test binary, where /proc/self/exe ignores the option) cannot start copies of itself without end.
  if (getenv("BIFROST_WINDOW_STATS_HELPER"))
    return;
  int fd[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                 fd)) {
    perror("[window-stats] socketpair");
    return;
  }
  // posix_spawn avoids executing C++/SDL code after fork in a threaded guest.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  constexpr int child_fd = 3;
  posix_spawn_file_actions_adddup2(&actions, fd[1], child_fd);
  posix_spawn_file_actions_addclosefrom_np(&actions, 4);
  char exe[] = "/proc/self/exe", option[] = "--window-stats-ui", number[] = "3";
  char *argv[] = {exe, option, number, nullptr};
  std::vector<char *> helper_environment;
  for (char **entry = environ; entry && *entry; ++entry) helper_environment.push_back(*entry);
  char marker[] = "BIFROST_WINDOW_STATS_HELPER=1";
  helper_environment.push_back(marker);
  helper_environment.push_back(nullptr);
  int err = posix_spawn(&state.child, exe, &actions, nullptr, argv, helper_environment.data());
  posix_spawn_file_actions_destroy(&actions);
  close(fd[1]);
  if (err) {
    close(fd[0]);
    state.child = -1;
    fprintf(stderr, "[window-stats] UI launch: %s\n", strerror(err));
  } else {
    state.socket = fd[0];
    const pid_t child = state.child;
    std::thread([child] {
      while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
      }
    }).detach();
  }
}
static void record_present(State &state, const char *backend, uint64_t now) {
  state.backend = backend;
  if (!state.clock.frames && dbg().render_log)
    fprintf(stderr, "[render] bifrost-emu %s: rendering started (%s)\n",
            VERSION, backend);
  state.clock.present(now);
  // The renderer name only needs a current context, so ask until one is; the capacity lookup is bounded.
  const bool want_vram = !state.vram && state.vram_attempts < 4;
  if (enabled() && (state.gpu.empty() || want_vram) &&
      gpu_info(state.gpu, state.vram, state.device, want_vram) && want_vram)
    ++state.vram_attempts;
}

// Whole-session memory, read away from the render path: session_memory() walks page tables, and the mmap lock it
// holds there is one the guest's own mmap calls wait for, so it also runs rarely. A forked child starts its own.
static std::atomic<uint64_t> session_ram{0};
static std::atomic<pid_t> session_sampler_pid{0};
static void start_session_sampler() {
  const pid_t pid = getpid();
  if (session_sampler_pid.exchange(pid) == pid) return;
  session_ram = 0;
  // Signals sent to the process are for the guest's threads, so this one never takes them.
  sigset_t all, previous;
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &previous);
  try {
    std::thread([pid] {
      for (;;) {
        session_ram = session_memory("/proc", pid);
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    }).detach();
  } catch (const std::system_error &) {
    // No thread available: a statistic must not end the game, and the display keeps showing this process's RSS.
  }
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

static std::string update(State &state, const WindowInfo &info, uint64_t now) {
  if (!enabled() || !state.clock.frames ||
      (state.update_ns && now - state.update_ns < 250000000ULL))
    return {};
  state.update_ns = now;
  Packet packet;
  static const auto cpu = cpu_name();
  static const uint64_t total = proc_kb("/proc/meminfo", "MemTotal:");
  start_session_sampler();
  static uint64_t rss = 0, rss_sample = 0;
  uint64_t ram = session_ram;
  if (!ram) {  // until the first session sample arrives
    if (!rss_sample || now - rss_sample >= 1000000000ULL) {
      rss = proc_kb("/proc/self/status", "VmRSS:"); rss_sample = now;
    }
    ram = rss;
  }
  packet.ram = ram; packet.total_ram = total; packet.vram = state.vram;
  if (!state.vram_sample || now - state.vram_sample >= 1000000000ULL) {
    state.vram_known = state.vram && used_vram(state.device, state.used_vram);
    state.vram_sample = now;
  }
  packet.used_vram = state.used_vram; packet.vram_known = state.vram_known;
  if (state.device.empty() && (!state.device_ns || now - state.device_ns >= 1000000000ULL)) {
    state.device_ns = now;
    state.device = detail::pick_gpu_device(drm_clients());
  }
  const SensorReading &sensors = host_sensors.sample(now, state.device);
  packet.cpu_percent = sensors.cpu_percent; packet.cpu_temp = sensors.cpu_temp;
  packet.gpu_percent = sensors.gpu_percent; packet.gpu_temp = sensors.gpu_temp;
  packet.used_ram = sensors.ram_known ? sensors.ram_used : 0;
  packet.ram_known = sensors.ram_known; packet.process_percent = sensors.process_percent;
  // The GPU's own counters cover the whole machine, as other monitors show; else this process's share.
  packet.panel_vram_known = sensors.vram_known || packet.vram_known;
  packet.panel_vram_used = sensors.vram_known ? sensors.vram_used : packet.used_vram;
  packet.panel_vram_total = sensors.vram_known ? sensors.vram_total : packet.vram;
  // A Vulkan-only guest has no GL renderer string, so ask the host Vulkan loader once: the
  // device it reports is the one the emulator forwards the guest's rendering to. The kernel
  // driver name stays the last resort.
  static const std::string host_gpu = host_vulkan_name();
  const std::string &gpu_name = !state.gpu.empty() ? state.gpu
                              : !host_gpu.empty() ? host_gpu : sensors.gpu_driver;
  snprintf(packet.panel_gpu, sizeof(packet.panel_gpu), "%s", gpu_name.c_str());
  snprintf(packet.cpu, sizeof(packet.cpu), "%s", cpu.c_str());
  snprintf(packet.gpu, sizeof(packet.gpu), "%s", gpu_name.empty() ? "unavailable" : gpu_name.c_str());
  packet.frames = state.clock.frames;
  packet.last_ns = state.clock.last_ns;
  packet.fps = state.clock.active_fps(now);
  packet.x = info.x; packet.y = info.y;
  packet.w = info.w; packet.h = info.h;
  packet.visible = info.visible;
  packet.xwindow = info.xwindow;
  snprintf(packet.backend, sizeof(packet.backend), "%s", state.backend.c_str());
  if (info.title != state.decorated)
    state.title = info.title;
  snprintf(packet.app, sizeof(packet.app), "%s", state.title.c_str());
  if (state.native && dbg().window_stats_overlay) {
    state.overlay = render_overlay(overlay_lines(overlay_stats(packet, now - state.clock.last_ns <= 2000000000ULL)),
                                   overlay_scale(info.h));
    ++state.overlay_seq;
  }
  std::string title;
  if (dbg().window_stats_title) {
    char suffix[256];
    snprintf(suffix, sizeof(suffix),
             " | bifrost-emu %s | %s %.1f FPS | %dx%d | frame %llu | RAM %.0f/%.0f MB", VERSION,
             now - state.clock.last_ns > 2000000000ULL ? "idle" : state.backend.c_str(),
             packet.fps, packet.w, packet.h, (unsigned long long)packet.frames,
             packet.ram / 1e6, packet.total_ram / 1e6);
    title = state.decorated = state.title + suffix + " | " + cpu + " | " +
        (gpu_name.empty() ? "unavailable" : gpu_name) + " | VRAM " +
        detail::vram_text(packet.used_vram, packet.vram, packet.vram_known);
  }
  packet.separate = dbg().window_stats_window;
  // A native window draws its own overlay (overlay_native); the desktop HUD helper is for the others.
  packet.overlay = dbg().window_stats_overlay && !state.native;
  if (packet.separate || packet.overlay) {
    start_helper(state);
    if (state.socket >= 0 &&
        send(state.socket, &packet, sizeof(packet), MSG_NOSIGNAL) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
      close(state.socket);
      state.socket = -1;
    }
  }
  return title;
}

void note_present(void *key, const char *backend) {
  if (!key) return;
  std::lock_guard<std::mutex> lock(mu);
  auto &state = windows[key];
  state.native = true;
  record_present(state, backend, now_ns());
}

std::string update_native(void *key, const WindowInfo &info) {
  if (!key) return {};
  std::lock_guard<std::mutex> lock(mu);
  auto it = windows.find(key);
  if (it == windows.end()) return {};
  it->second.native = true;
  return update(it->second, info, now_ns());
}

bool overlay_native(void *key, OverlayImage &image) {
  if (!key) return false;
  std::lock_guard<std::mutex> lock(mu);
  auto it = windows.find(key);
  if (it == windows.end() || it->second.overlay_seq == it->second.overlay_taken) return false;
  it->second.overlay_taken = it->second.overlay_seq;
  image = it->second.overlay;
  return true;
}

#if defined(BIFROST_USE_SDL2)
static void publish(void *key, const char *backend, bool sdl,
                    bool presented = true) {
  std::lock_guard<std::mutex> lock(mu);
  auto &state = windows[key];
  const uint64_t now = now_ns();
  state.sdl = sdl;
  state.owner = std::this_thread::get_id();
  if (presented)
    record_present(state, backend, now);
  if (!enabled() || (state.update_ns && now - state.update_ns < 250000000ULL))
    return;
  WindowInfo packet;
  packet.title = state.title;
  auto *window = static_cast<SDL_Window *>(key);
  using SetTitle = void (*)(void *, const char *);
  using GetTitle = const char *(*)(void *);
  using GetPair = void (*)(void *, int *, int *);
  SetTitle set_title = nullptr;
  GetTitle get_title = nullptr;
  GetPair get_pos = nullptr, get_size = nullptr;
  if (sdl) {
    get_title = [](void *w) {
      return SDL_GetWindowTitle(static_cast<SDL_Window *>(w));
    };
    set_title = [](void *w, const char *t) {
      SDL_SetWindowTitle(static_cast<SDL_Window *>(w), t);
    };
    get_pos = [](void *w, int *x, int *y) {
      SDL_GetWindowPosition(static_cast<SDL_Window *>(w), x, y);
    };
    get_size = [](void *w, int *x, int *y) {
      // Physical pixels: logical size is smaller on scaled displays.
#if SDL_VERSION_ATLEAST(2, 26, 0)
      SDL_GetWindowSizeInPixels(static_cast<SDL_Window *>(w), x, y);
#else
      SDL_GetWindowSize(static_cast<SDL_Window *>(w), x, y);
#endif
    };
    packet.visible = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) &&
                     !(SDL_GetWindowFlags(window) &
                       (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) && info.subsystem == SDL_SYSWM_X11)
      packet.xwindow = info.info.x11.window;
  } else {
    static void *glfw = dlopen("libglfw.so.3", RTLD_LAZY | RTLD_NOLOAD);
    if (glfw) {
      set_title = reinterpret_cast<SetTitle>(dlsym(glfw, "glfwSetWindowTitle"));
      get_pos = reinterpret_cast<GetPair>(dlsym(glfw, "glfwGetWindowPos"));
      get_size = reinterpret_cast<GetPair>(dlsym(glfw, "glfwGetFramebufferSize"));
      using GetX = unsigned long (*)(void *);
      auto get_x = reinterpret_cast<GetX>(dlsym(glfw, "glfwGetX11Window"));
      if (get_x)
        packet.xwindow = get_x(key);
    }
    packet.visible = 1;
  }
  if (get_pos)
    get_pos(key, &packet.x, &packet.y);
  if (get_size)
    get_size(key, &packet.w, &packet.h);
  if (get_title) packet.title = get_title(key);
  std::string title = update(state, packet, now);
  if (!title.empty() && set_title) set_title(key, title.c_str());
}
#endif
void sdl_present(void *window, const char *backend) {
#if defined(BIFROST_USE_SDL2)
  if (window)
    publish(window, backend, true);
#else
  (void)window;
  (void)backend;
#endif
}
void forget(void *window) {
  std::lock_guard<std::mutex> lock(mu);
  windows.erase(window);
  for (auto it = surfaces.begin(); it != surfaces.end();) {
    if (it->second == window)
      it = surfaces.erase(it);
    else
      ++it;
  }
  for (auto it = swapchains.begin(); it != swapchains.end();) {
    if (it->second == window)
      it = swapchains.erase(it);
    else
      ++it;
  }
  for (auto it = renderers.begin(); it != renderers.end();) {
    if (it->second == window)
      it = renderers.erase(it);
    else
      ++it;
  }
}
void api(const std::string &name, const uint64_t *a, uint64_t result) {
#if defined(BIFROST_USE_SDL2)
  if (enabled() && (name == "SDL_PollEvent" || name == "SDL_PumpEvents" ||
                    name == "glfwPollEvents")) {
    std::vector<std::pair<void *, bool>> live;
    {
      std::lock_guard<std::mutex> lock(mu);
      for (auto &[key, state] : windows)
        if (!state.native && state.clock.frames && state.owner == std::this_thread::get_id())
          live.emplace_back(key, state.sdl);
    }
    for (auto [key, sdl] : live)
      publish(key, sdl ? "OpenGL" : "OpenGL/GLFW", sdl, false);
  }
#endif
  if (name == "SDL_Quit" || name == "glfwTerminate") {
    std::lock_guard<std::mutex> lock(mu);
    for (auto it = windows.begin(); it != windows.end();) {
      if (!it->second.native && it->second.sdl == (name == "SDL_Quit"))
        it = windows.erase(it);
      else
        ++it;
    }
    if (name == "SDL_Quit") {
      renderers.clear();
      surfaces.clear();
      swapchains.clear();
    }
    return;
  }
  if (name == "SDL_DestroyWindow" || name == "glfwDestroyWindow") {
    forget(reinterpret_cast<void *>(a[0]));
    return;
  }
  if (name == "SDL_CreateRenderer" && result) {
    std::lock_guard<std::mutex> lock(mu);
    renderers[reinterpret_cast<void *>(result)] =
        reinterpret_cast<void *>(a[0]);
  } else if (name == "SDL_DestroyRenderer") {
    std::lock_guard<std::mutex> lock(mu);
    renderers.erase(reinterpret_cast<void *>(a[0]));
  } else if (name == "SDL_Vulkan_CreateSurface" && result && a[2]) {
    uint64_t surface;
    memcpy(&surface, reinterpret_cast<void *>(a[2]), 8);
    std::lock_guard<std::mutex> lock(mu);
    surfaces[surface] = reinterpret_cast<void *>(a[0]);
    if (dbg().frame_trace) fprintf(stderr,"[window-stats] surface %llx -> window %llx\n",(unsigned long long)surface,(unsigned long long)a[0]);
  } else if (name == "vkCreateSwapchainKHR" &&
             static_cast<int32_t>(result) == 0 && a[1] && a[3]) {
    // LP64 VkSwapchainCreateInfoKHR: surface follows sType/pNext/flags.
    uint64_t surface, chain;
    memcpy(&surface, reinterpret_cast<char *>(a[1]) + 24, 8);
    memcpy(&chain, reinterpret_cast<void *>(a[3]), 8);
    std::lock_guard<std::mutex> lock(mu);
    if (dbg().frame_trace) fprintf(stderr,"[window-stats] swapchain %llx -> surface %llx\n",(unsigned long long)chain,(unsigned long long)surface);
    auto it = surfaces.find(surface);
    if (it != surfaces.end())
      swapchains[chain] = it->second;
  } else if (name == "vkDestroySwapchainKHR") {
    std::lock_guard<std::mutex> lock(mu);
    swapchains.erase(a[1]);
  } else if (name == "SDL_GL_SwapWindow")
    sdl_present(reinterpret_cast<void *>(a[0]), "OpenGL");
  else if (name == "SDL_RenderPresent") {
    void *window = nullptr;
    {
      std::lock_guard<std::mutex> lock(mu);
      auto it = renderers.find(reinterpret_cast<void *>(a[0]));
      if (it != renderers.end())
        window = it->second;
    }
    sdl_present(window, "SDL renderer");
  } else if (name == "glfwCreateWindow" && result) {
    std::lock_guard<std::mutex> lock(mu);
    windows[reinterpret_cast<void *>(result)].title =
        reinterpret_cast<const char *>(a[2]);
  } else if (name == "glfwSetWindowTitle") {
    std::lock_guard<std::mutex> lock(mu);
    auto &s = windows[reinterpret_cast<void *>(a[0])];
    s.title = reinterpret_cast<const char *>(a[1]);
    s.update_ns = 0;
  } else if (name == "glfwSwapBuffers") {
#if defined(BIFROST_USE_SDL2)
    publish(reinterpret_cast<void *>(a[0]), "OpenGL/GLFW", false);
#endif
  }
}
void vk_present(uint64_t chain) {
  void *window = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = swapchains.find(chain);
    if (it != swapchains.end())
      window = it->second;
  }
  sdl_present(window, "Vulkan");
}
#if defined(BIFROST_USE_SDL2)
// Built-in 5x7 font (StatsOverlay.cpp): no guest libraries, font files or SDL_ttf dependency.
static void text(SDL_Renderer *r, const char *s, int y) {
  int x = 12;
  for (; *s && x < 880; ++s, x += 12) {
    const uint16_t *g = detail::glyph_columns(static_cast<unsigned char>(*s));
    for (int col = 0; col < 5; ++col)
      for (int row = 0; row < 9; ++row)
        if (g[col] & (1 << row)) {
          SDL_Rect rect{x + col * 2, y + row * 2, 2, 2};
          SDL_RenderFillRect(r, &rect);
        }
  }
}
using Shape = void (*)(Display *, unsigned long, int, int, int, void *, int, int, int);
// The desktop HUD shows the in-window panel. An X11 window has no alpha channel, so the panel is composed over a
// dark base and, given an X11 display, the window's bounding shape trims the rounded corners.
static void draw_hud(SDL_Window *window, SDL_Renderer *renderer, const OverlayImage &image, Display *display,
                     unsigned long xid, Shape shape) {
  int width = 0, height = 0;
  SDL_GetWindowSize(window, &width, &height);
  if (width != image.w || height != image.h) SDL_SetWindowSize(window, image.w, image.h);
  if (display && shape) {
    std::vector<XRectangle> rows;  // one span per row: the pixels at least half covered
    for (int y = 0; y < image.h; ++y) {
      const uint32_t *row = &image.argb[static_cast<size_t>(y) * image.w];
      int first = 0, last = image.w;
      while (first < last && (row[first] >> 24) < 0x80) ++first;
      while (last > first && (row[last - 1] >> 24) < 0x80) --last;
      if (first < last)
        rows.push_back({static_cast<short>(first), static_cast<short>(y), static_cast<unsigned short>(last - first), 1});
    }
    shape(display, xid, 0, 0, 0, rows.data(), static_cast<int>(rows.size()), 0, 0);  // ShapeBounding, ShapeSet
  }
  SDL_SetRenderDrawColor(renderer, 0x0B, 0x0F, 0x16, 255);
  SDL_RenderClear(renderer);
  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(const_cast<uint32_t *>(image.argb.data()), image.w,
                                                            image.h, 32, image.w * 4, SDL_PIXELFORMAT_ARGB8888);
  if (SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr) {
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_DestroyTexture(texture);
  }
  SDL_FreeSurface(surface);
  SDL_RenderPresent(renderer);
}
#endif
int ui_main(int fd) {
#if defined(BIFROST_USE_SDL2)
  Packet p;
  // Choose the same X server as the game, even when the desktop's default
  // SDL driver is Wayland. This is local to the re-exec UI process.
  for (;;) {
    ssize_t n = recv(fd, &p, sizeof(p), MSG_DONTWAIT);
    if (n == sizeof(p))
      break;
    if (n == 0 ||
        (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return 0;
    usleep(10000);
  }
  if (p.xwindow)
    setenv("SDL_VIDEODRIVER", "x11", 1);
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_Init(SDL_INIT_VIDEO)) {
    fprintf(stderr, "[window-stats] UI: %s\n", SDL_GetError());
    return 1;
  }
  bool received = true, stop = false, closed[2] = {false, false};
  SDL_Window *windows[2]{};
  SDL_Renderer *renderers[2]{};
  void *xlib = dlopen("libX11.so.6", RTLD_LAZY);
  void *xext = dlopen("libXext.so.6", RTLD_LAZY);
  auto shape =
      xext ? reinterpret_cast<Shape>(dlsym(xext, "XShapeCombineRectangles"))
           : nullptr;
  using Translate = int (*)(Display *, unsigned long, unsigned long, int, int,
                            int *, int *, unsigned long *);
  auto translate =
      xlib ? reinterpret_cast<Translate>(dlsym(xlib, "XTranslateCoordinates"))
           : nullptr;
  // Ignore disappearing-window X errors; guest window lifetime is independent.
  using ErrorHandler = int (*)(Display *, XErrorEvent *);
  using SetErrorHandler = ErrorHandler (*)(ErrorHandler);
  auto set_error =
      xlib ? reinterpret_cast<SetErrorHandler>(dlsym(xlib, "XSetErrorHandler"))
           : nullptr;
  if (set_error)
    set_error([](Display *, XErrorEvent *) { return 0; });
  while (!stop) {
    Packet next;
    for (;;) {
      ssize_t n = recv(fd, &next, sizeof(next), MSG_DONTWAIT);
      if (n == 0) {
        stop = true;
        break;
      }
      if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          stop = true;
        break;
      }
      if (n == sizeof(next)) {
        p = next;
        received = true;
      }
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        stop = true;
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_CLOSE)
        for (int i = 0; i < 2; ++i)
          if (windows[i] &&
              event.window.windowID == SDL_GetWindowID(windows[i])) {
            closed[i] = true;
            SDL_DestroyRenderer(renderers[i]);
            SDL_DestroyWindow(windows[i]);
            windows[i] = nullptr;
          }
    }
    if (stop)
      break;
    if (!received) {
      SDL_Delay(50);
      continue;
    }
    for (int i = 0; i < 2; ++i) {
      if (closed[i] || !(i ? p.overlay : p.separate))
        continue;
      if (!windows[i]) {
        if (i && (!p.xwindow || !shape || !translate ||
                  std::string(SDL_GetCurrentVideoDriver()) != "x11")) {
          fprintf(stderr, "[window-stats] overlay requires an X11 game window; "
                          "separate stats window remains available\n");
          closed[i] = true;
          continue;
        }
        char title[96];
        snprintf(title, sizeof(title), "bifrost-emu %s - window stats",
                 VERSION);
        windows[i] = SDL_CreateWindow(
            title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 900, 240,
            i ? SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP |
                    SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_TOOLTIP |
                    SDL_WINDOW_HIDDEN
              : 0);
        if (!windows[i]) {
          fprintf(stderr, "[window-stats] create mode %d (%s): %s\n", i,
                  SDL_GetCurrentVideoDriver(), SDL_GetError());
          closed[i] = true;
          continue;
        }
        renderers[i] =
            SDL_CreateRenderer(windows[i], -1, SDL_RENDERER_SOFTWARE);
        if (!renderers[i]) {
          fprintf(stderr, "[window-stats] renderer: %s\n", SDL_GetError());
          SDL_DestroyWindow(windows[i]);
          windows[i] = nullptr;
          closed[i] = true;
          continue;
        }
        if (i) {
          SDL_SysWMinfo info;
          SDL_VERSION(&info.version);
          if (!p.xwindow || !shape || !translate ||
              !SDL_GetWindowWMInfo(windows[i], &info) ||
              info.subsystem != SDL_SYSWM_X11) {
            fprintf(stderr, "[window-stats] overlay requires X11 click-through "
                            "support; separate window remains available\n");
            SDL_DestroyRenderer(renderers[i]);
            SDL_DestroyWindow(windows[i]);
            windows[i] = nullptr;
            closed[i] = true;
            continue;
          }
          shape(info.info.x11.display, info.info.x11.window, 2, 0, 0, nullptr,
                0, 0, 0); // ShapeInput, ShapeSet
          using StoreName = int (*)(Display *, unsigned long, const char *);
          auto store_name =
              reinterpret_cast<StoreName>(dlsym(xlib, "XStoreName"));
          if (store_name)
            store_name(info.info.x11.display, info.info.x11.window, title);
        }
      }
      if (i) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        SDL_GetWindowWMInfo(windows[i], &info);
        int x = p.x, y = p.y;
        unsigned long child = 0;
        translate(info.info.x11.display, p.xwindow,
                  DefaultRootWindow(info.info.x11.display), 8, 8, &x, &y,
                  &child);
        SDL_SetWindowPosition(windows[i], x, y);
        if (p.visible)
          SDL_ShowWindow(windows[i]);
        else
          SDL_HideWindow(windows[i]);
        const bool active = now_ns() - p.last_ns <= 2000000000ULL;
        draw_hud(windows[i], renderers[i], render_overlay(overlay_lines(overlay_stats(p, active)), overlay_scale(p.h)),
                 info.info.x11.display, info.info.x11.window, shape);
        // Mapping/reconfiguring a window can restore SDL's input shape.
        // Reassert the empty region after positioning, showing and resizing it.
        shape(info.info.x11.display, info.info.x11.window, 2, 0, 0, nullptr, 0,
              0, 0);
        using Flush = int (*)(Display *);
        auto flush = reinterpret_cast<Flush>(dlsym(xlib, "XFlush"));
        if (flush)
          flush(info.info.x11.display);
        continue;
      }
      const bool active = now_ns() - p.last_ns <= 2000000000ULL;
      char line[192];
      SDL_SetRenderDrawColor(renderers[i], 16, 20, 28, 255);
      SDL_RenderClear(renderers[i]);
      SDL_SetRenderDrawColor(renderers[i], 210, 235, 255, 255);
      snprintf(line, sizeof(line), "BIFROST-EMU %s", VERSION);
      text(renderers[i], line, 12);
      snprintf(line, sizeof(line), "%s - %s", p.backend,
               active ? "RENDERING" : "IDLE");
      text(renderers[i], line, 36);
      snprintf(line, sizeof(line), "FPS %.1f - FRAMES %llu", active ? p.fps : 0,
               (unsigned long long)p.frames);
      text(renderers[i], line, 60);
      snprintf(line, sizeof(line), "SIZE %d X %d - LAST FRAME %.1f S", p.w, p.h,
               (now_ns() - p.last_ns) / 1e9);
      text(renderers[i], line, 84);
      int y = 108;
      for (const char *name : {p.cpu, p.gpu}) {
        const std::string full = name;
        for (size_t offset = 0; offset < full.size(); offset += 70) {
          text(renderers[i], full.substr(offset, 70).c_str(), y);
          y += 24;
        }
      }
      snprintf(line, sizeof(line), "EMULATOR RAM %.0f / %.0f MB SYSTEM TOTAL", p.ram / 1e6, p.total_ram / 1e6);
      text(renderers[i], line, y);
      snprintf(line, sizeof(line), "VRAM %s - EXPERIMENTAL",
               detail::vram_text(p.used_vram, p.vram, p.vram_known).c_str());
      text(renderers[i], line, y + 24);
      auto reading = [](double value, const char *unit) {
        char text[32];
        if (value < 0) return std::string("--");
        snprintf(text, sizeof(text), "%.0f%s", value, unit);
        return std::string(text);
      };
      snprintf(line, sizeof(line), "CPU %s %s - GPU %s %s", reading(p.cpu_percent, "%").c_str(),
               reading(p.cpu_temp, "C").c_str(), reading(p.gpu_percent, "%").c_str(),
               reading(p.gpu_temp, "C").c_str());
      text(renderers[i], line, y + 48);
      snprintf(line, sizeof(line), "SYSTEM RAM IN USE %.0f MB", p.used_ram / 1e6);
      text(renderers[i], line, y + 72);
      int width, height;
      SDL_GetWindowSize(windows[i], &width, &height);
      if (height != y + 108) SDL_SetWindowSize(windows[i], width, y + 108);
      SDL_RenderPresent(renderers[i]);
    }
    SDL_Delay(100);
  }
  for (int i = 0; i < 2; ++i)
    if (windows[i]) {
      SDL_DestroyRenderer(renderers[i]);
      SDL_DestroyWindow(windows[i]);
    }
  SDL_Quit();
  close(fd);
  return 0;
#else
  (void)fd;
  fprintf(stderr, "[window-stats] UI requires an SDL2 build\n");
  return 1;
#endif
}
} // namespace arm64emu::window_stats
