// SDL2 ABI and Neverball fullscreen recreation regression.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t thunk_dlopen(const char *path) {
  register uint64_t x0 __asm__("x0") = (uintptr_t)path;
  register uint64_t x1 __asm__("x1") = 1;
  register uint64_t x8 __asm__("x8") = 0x1002;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}
static void *sym(uint64_t lib, const char *name) {
  register uint64_t x0 __asm__("x0") = lib;
  register uint64_t x1 __asm__("x1") = (uintptr_t)name;
  register uint64_t x8 __asm__("x8") = 0x1003;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  if (!x0) {
    fprintf(stderr, "missing symbol: %s\n", name);
    exit(2);
  }
  return (void *)(uintptr_t)x0;
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef union {
  uint64_t align;
  unsigned char bytes[56];
} Event;
int main(int argc, char **argv) {
  const int stats = argc > 1 && strcmp(argv[1], "title") == 0;
  uint64_t lib = thunk_dlopen("libSDL2.so");
  CHECK(lib);
  int (*init)(uint32_t) = sym(lib, "SDL_Init");
  void *(*create)(const char *, int, int, int, int, uint32_t) =
      sym(lib, "SDL_CreateWindow");
  void (*destroy)(void *) = sym(lib, "SDL_DestroyWindow");
  void *(*context)(void *) = sym(lib, "SDL_GL_CreateContext");
  void (*delete_context)(void *) = sym(lib, "SDL_GL_DeleteContext");
  void (*swap)(void *) = sym(lib, "SDL_GL_SwapWindow");
  const char *(*title)(void *) = sym(lib, "SDL_GetWindowTitle");
  void (*set_title)(void *, const char *) = sym(lib, "SDL_SetWindowTitle");
  void (*delay)(uint32_t) = sym(lib, "SDL_Delay");
  void (*raise_window)(void *) = sym(lib, "SDL_RaiseWindow");
  int (*poll)(Event *) = sym(lib, "SDL_PollEvent");
  void (*quit)(void) = sym(lib, "SDL_Quit");
  if (init(0x20)) {
    puts("SKIP: SDL video unavailable");
    return 77;
  }
  for (int cycle = 0; cycle < 2; ++cycle) {
    void *w = create("stats regression", 100, 100, 640, 480, 2);
    CHECK(w);
    void *c = context(w);
    CHECK(c);
    for (int i = 0; i < 45; ++i) {
      Event e;
      while (poll(&e)) {
      }
      if (i == 10)
        raise_window(w);
      swap(w);
      delay(30);
    }
    printf("TITLE %s\n", title(w));
    if (stats) {
      CHECK(strstr(title(w), "bifrost-emu "));
      CHECK(strstr(title(w), "FPS"));
      CHECK(strstr(title(w), "stats regression |"));
    } else
      CHECK(strcmp(title(w), "stats regression") == 0);
    set_title(w, "renamed game");
    delay(300);
    swap(w);
    CHECK(strstr(title(w), "renamed game"));
    if (stats) {
      delay(2300);
      Event e;
      while (poll(&e)) {
      }
      CHECK(strstr(title(w), "idle 0.0 FPS"));
      printf("IDLE %s\n", title(w));
    }
    delete_context(c);
    destroy(w);
  }
  quit();
  puts("window_stats: ALL PASS");
  return 0;
}
