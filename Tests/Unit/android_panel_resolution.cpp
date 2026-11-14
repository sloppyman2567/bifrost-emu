// Host test for the Resolution setting's panel scaling (Source/System/Android/Window/PanelResolution.hpp).
#include "System/Android/Window/PanelResolution.hpp"
#include <cstdio>

static int failures;
static void check(bool ok, const char *what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

int main() {
  using namespace frost::panel_resolution;
  check(parse(nullptr) == 0 && parse("") == 0, "no setting follows the window");
  check(parse("1080") == 1080 && parse("2160") == 2160, "plain numbers");
  check(parse("window") == 0 && parse("1080p") == 0 && parse("-720") == 0 && parse(" 720") == 0, "not a plain number");
  check(parse("239") == 0 && parse("240") == 240 && parse("4320") == 4320 && parse("4321") == 0, "range");
  check(parse("99999999999999999999") == 0, "overflow");
  int32_t w = 584, h = 1038;
  scale(w, h, 1080);
  check(w == 1080 && h == 1920, "a 9:16 window panel at 1080");
  w = 1038; h = 584;
  scale(w, h, 1080);
  check(w == 1920 && h == 1080, "the landscape panel at 1080");
  w = 584; h = 1038;
  scale(w, h, 720);
  check(w == 720 && h == 1280, "scaled up to 720");
  w = 1920; h = 1080;
  scale(w, h, 720);
  check(w == 1280 && h == 720, "scaled down to 720");
  w = 584; h = 1038;
  scale(w, h, 0);
  check(w == 584 && h == 1038, "no short side leaves the panel");
  w = 0; h = 100;
  scale(w, h, 1080);
  check(w == 0 && h == 100, "an empty panel is left alone");
  if (failures) return 1;
  std::printf("android_panel_resolution: ALL PASS\n");
  return 0;
}
