// RememberedSize: the size a game window had when it last closed (BIFROST_ANDROID_WINDOW_STATE).
#include "System/Android/Window/RememberedSize.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace rs = frost::remembered_size;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "android_window_size check failed at %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void write(const std::string &path, const std::string &text) { std::ofstream(path, std::ios::trunc) << text; }

int main() {
  char directory[] = "/tmp/bifrost-window-size-XXXXXX";
  CHECK(mkdtemp(directory));
  const std::string path = std::string(directory) + "/launcher-window.txt";
  struct Cleanup {
    std::string directory, path;
    ~Cleanup() { unlink(path.c_str()); unlink((path + ".tmp").c_str()); rmdir(directory.c_str()); }
  } cleanup{directory, path};
  int32_t w = 7, h = 7;

  // No file, no size; a stored size comes back exactly and leaves no temporary file.
  CHECK(!rs::load(path, w, h) && w == 7 && h == 7);
  CHECK(rs::store(path, 1280, 720) && rs::load(path, w, h) && w == 1280 && h == 720);
  CHECK(access((path + ".tmp").c_str(), F_OK) != 0);
  CHECK(rs::store(path, 720, 1280) && rs::load(path, w, h) && w == 720 && h == 1280);  // replaced whole

  // Anything but "WIDTH HEIGHT" is a damaged file: the previous values stay untouched.
  w = h = 7;
  for (const char *bad : {"", "\n", "1280", "1280 ", "1280x720\n", "1280 720 5\n", "1280 720x\n", "abc def\n", "0 0\n",
                          "-640 480\n", "159 480\n", "640 159\n", "32769 480\n", "480 32769\n", "99999999999 480\n",
                          "4294967936 480\n", "480 4294967936\n", "99999999999999999999 480\n"}) {
    write(path, bad);
    CHECK(!rs::load(path, w, h) && w == 7 && h == 7);
  }
  write(path, "  640   480  \r\n");  // tolerated whitespace
  CHECK(rs::load(path, w, h) && w == 640 && h == 480);
  write(path, "160 32768");  // the limits themselves, no final newline
  CHECK(rs::load(path, w, h) && w == 160 && h == 32768);

  // A size outside the limits is not written, and an unwritable directory fails without a trace.
  write(path, "640 480\n");
  CHECK(!rs::store(path, 159, 480) && !rs::store(path, 640, 32769) && !rs::store(path, 0, 0));
  CHECK(rs::load(path, w, h) && w == 640 && h == 480);
  CHECK(!rs::store(std::string(directory) + "/missing/launcher-window.txt", 640, 480));

  // fit: held inside the room the desktop gave, never a sliver; a bound that is not positive is none.
  w = 3000; h = 2000;
  rs::fit(w, h, 1920, 1080);
  CHECK(w == 1920 && h == 1080);
  w = 800; h = 600;
  rs::fit(w, h, 1920, 1080);
  CHECK(w == 800 && h == 600);
  rs::fit(w, h, 0, -1);
  CHECK(w == 800 && h == 600);
  w = 800; h = 600;
  rs::fit(w, h, 100, 100);  // a room smaller than the minimum still gives a window
  CHECK(w == rs::kMin && h == rs::kMin);
  w = 800; h = 600;
  rs::fit(w, h, INT32_MAX, INT32_MAX);  // the work area not learned yet
  CHECK(w == 800 && h == 600);

  // fit_shape: the largest window of the current shape inside the remembered box.
  w = 720; h = 1280;
  rs::fit_shape(w, h, 1000, 1000);
  CHECK(w == 563 && h == 1000);
  w = 1280; h = 720;
  rs::fit_shape(w, h, 1000, 1000);
  CHECK(w == 1000 && h == 563);
  w = 720; h = 1280;
  rs::fit_shape(w, h, 3000, 3000);  // a box larger than the shape's room only scales up inside the box
  CHECK(w == 1688 && h == 3000);  // 1687.5 rounds up
  w = 0; h = 0;
  rs::fit_shape(w, h, 1000, 1000);  // no current shape: unchanged
  CHECK(w == 0 && h == 0);
  std::puts("android_window_size: PASS");
  return 0;
}
