#pragma once
#include <Arduino.h>

namespace ui {

void begin();
void showBrowser(const char* const* filenames, int count, int selected);
void showNowPlaying(const char* filename, float speed, bool paused, bool keylock, bool cueSet);
void showMessage(const char* msg);

} // namespace ui
