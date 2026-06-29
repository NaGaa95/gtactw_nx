/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB for the .so load region (~15MB of mapped libraries); the newlib heap gets
// the rest and backs the game's malloc + mesa's GPU bos.
#define MEMORY_SO_MB 256

// CTW 4.4.243 ships the game as libGame.so. The APK has no libc++_shared.so:
// on Android the C++ runtime resolves from the APK's own libopenal.so, which
// statically links libc++ with default visibility. We load that same
// libopenal.so as a second module purely as the C++ runtime donor; the
// game's OpenAL imports still resolve to native openal-soft because the
// import table takes priority over module exports (see so_resolve_symbol).
#define SO_NAME "libGame.so"
#define CXX_DONOR_SO_NAME "libopenal.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"
// NvUtil get/setAppLocalValue key/value store lives here
#define APPSTATE_NAME "appstate.txt"

// per-line SD card writes make this cost several seconds of boot time;
// re-enable only when investigating a crash or misbehavior
// #define DEBUG_LOG 1

// actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int disable_mipmaps;
  int language;
  int touchscreen;
  int hide_touch_hud;
  int show_fps;
  int xbox_layout;   // 0 = Nintendo face buttons (default), 1 = legacy Xbox
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
