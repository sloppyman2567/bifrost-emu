#include "frost/window_stats.hpp"
#include "bifrost/version.hpp"
#include "debug_flags.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
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
  double fps{};
  int x{}, y{}, w{}, h{}, visible{}, separate{}, overlay{};
  char backend[32]{};
};
struct State {
  Clock clock;
  std::string title, decorated;
  uint64_t update_ns = 0;
  int socket = -1;
  pid_t child = -1;
  bool helper_attempted = false;
  bool sdl = true;
  std::thread::id owner;
  std::string backend;
  ~State() {
    if (socket >= 0)
      close(socket);
  }
};
static std::mutex mu;
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
  int err = posix_spawn(&state.child, exe, &actions, nullptr, argv, environ);
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
#if defined(BIFROST_USE_SDL2)
static void publish(void *key, const char *backend, bool sdl,
                    bool presented = true) {
  std::lock_guard<std::mutex> lock(mu);
  auto &state = windows[key];
  const uint64_t now = now_ns();
  if (presented)
    state.backend = backend;
  backend = state.backend.c_str();
  state.sdl = sdl;
  state.owner = std::this_thread::get_id();
  if (presented && !state.clock.frames && dbg().render_log)
    fprintf(stderr, "[render] bifrost-emu %s: rendering started (%s)\n",
            VERSION, backend);
  if (presented)
    state.clock.present(now);
  if (!enabled() || (state.update_ns && now - state.update_ns < 250000000ULL))
    return;
  state.update_ns = now;
  Packet packet;
  packet.frames = state.clock.frames;
  packet.last_ns = state.clock.last_ns;
  packet.fps = state.clock.active_fps(now);
  snprintf(packet.backend, sizeof(packet.backend), "%s", backend);
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
      SDL_GetWindowSize(static_cast<SDL_Window *>(w), x, y);
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
      get_size = reinterpret_cast<GetPair>(dlsym(glfw, "glfwGetWindowSize"));
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
  if (get_title) {
    std::string current = get_title(key);
    if (current != state.decorated)
      state.title = current;
  }
  if (dbg().window_stats_title && set_title) {
    char suffix[192];
    snprintf(suffix, sizeof(suffix),
             " | bifrost-emu %s | %s %.1f FPS | %dx%d | frame %llu", VERSION,
             now - state.clock.last_ns > 2000000000ULL ? "idle" : backend,
             packet.fps, packet.w, packet.h, (unsigned long long)packet.frames);
    state.decorated = state.title + suffix;
    set_title(key, state.decorated.c_str());
  }
  packet.separate = dbg().window_stats_window;
  packet.overlay = dbg().window_stats_overlay;
  if (packet.separate || packet.overlay) {
    start_helper(state);
    if (state.socket >= 0 &&
        send(state.socket, &packet, sizeof(packet), MSG_NOSIGNAL) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK) {
      close(state.socket);
      state.socket = -1;
    }
  }
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
        if (state.clock.frames && state.owner == std::this_thread::get_id())
          live.emplace_back(key, state.sdl);
    }
    for (auto [key, sdl] : live)
      publish(key, sdl ? "OpenGL" : "OpenGL/GLFW", sdl, false);
  }
#endif
  if (name == "SDL_Quit" || name == "glfwTerminate") {
    std::lock_guard<std::mutex> lock(mu);
    for (auto it = windows.begin(); it != windows.end();) {
      if (it->second.sdl == (name == "SDL_Quit"))
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
// Built-in 5x7 font: no guest libraries, font files or SDL_ttf dependency.
static void text(SDL_Renderer *r, const char *s, int y) {
  static constexpr unsigned char glyphs[][5] = {
      {62, 81, 73, 69, 62},  {0, 66, 127, 64, 0},    {66, 97, 81, 73, 70},
      {33, 65, 69, 75, 49},  {24, 20, 18, 127, 16},  {39, 69, 69, 69, 57},
      {60, 74, 73, 73, 48},  {1, 113, 9, 5, 3},      {54, 73, 73, 73, 54},
      {6, 73, 73, 41, 30},   {126, 17, 17, 17, 126}, {127, 73, 73, 73, 54},
      {62, 65, 65, 65, 34},  {127, 65, 65, 34, 28},  {127, 73, 73, 73, 65},
      {127, 9, 9, 9, 1},     {62, 65, 73, 73, 122},  {127, 8, 8, 8, 127},
      {0, 65, 127, 65, 0},   {32, 64, 65, 63, 1},    {127, 8, 20, 34, 65},
      {127, 64, 64, 64, 64}, {127, 2, 12, 2, 127},   {127, 4, 8, 16, 127},
      {62, 65, 65, 65, 62},  {127, 9, 9, 9, 6},      {62, 65, 81, 33, 94},
      {127, 9, 25, 41, 70},  {70, 73, 73, 73, 49},   {1, 1, 127, 1, 1},
      {63, 64, 64, 64, 63},  {31, 32, 64, 32, 31},   {63, 64, 56, 64, 63},
      {99, 20, 8, 20, 99},   {7, 8, 112, 8, 7},      {97, 81, 73, 69, 67}};
  int x = 12;
  for (; *s && x < 540; ++s, x += 12) {
    unsigned char c = *s;
    if (c >= 'a' && c <= 'z')
      c -= 32;
    const unsigned char *g = c >= '0' && c <= '9'   ? glyphs[c - '0']
                             : c >= 'A' && c <= 'Z' ? glyphs[10 + c - 'A']
                                                    : nullptr;
    for (int col = 0; col < 5; ++col)
      for (int row = 0; row < 7; ++row) {
        bool bit = g          ? (g[col] & (1 << row))
                   : c == '.' ? (col == 2 && row == 6)
                   : c == '-' ? (row == 3)
                   : c == ':' ? (col == 2 && (row == 2 || row == 5))
                   : c == '/' ? (row == 6 - col)
                              : false;
        if (bit) {
          SDL_Rect rect{x + col * 2, y + row * 2, 2, 2};
          SDL_RenderFillRect(r, &rect);
        }
      }
  }
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
  using Shape =
      void (*)(Display *, unsigned long, int, int, int, void *, int, int, int);
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
            title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 560, 120,
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
        // Mapping/reconfiguring a window can restore SDL's input shape.
        // Reassert the empty region after positioning and showing it.
        shape(info.info.x11.display, info.info.x11.window, 2, 0, 0, nullptr, 0,
              0, 0);
        using Flush = int (*)(Display *);
        auto flush = reinterpret_cast<Flush>(dlsym(xlib, "XFlush"));
        if (flush)
          flush(info.info.x11.display);
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
