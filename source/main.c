/* main.c
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * GTA: Chinatown Wars 4.4.243 uses the same oswrapper framework as Max
 * Payne 2.1.131: the Java side owns the main loop and pushes everything
 * through JNI entry points (GameNative.impl*); we replicate that loop here
 * with a fake JNI environment. Entry point signatures below were extracted
 * from the APK's classes2.dex (see PORT_NOTES.md).
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module donor_mod; // libopenal.so, C++ runtime donor (see config.h)
so_module game_mod;  // libGame.so

// provide replacement heap init function to separate newlib heap from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // only allocate a fixed amount for the newlib heap
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = umin(size, MEMORY_MB * 1024 * 1024);
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000); // align to page size
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;
}

static void check_data(void) {
  // 4.4.243 ships all game data in the APK's assets folder; both .so files
  // come from the APK's lib/arm64-v8a folder
  const char *files[] = {
    SO_NAME,
    CXX_DONOR_SO_NAME,
    "game.pak",
    "dxt.bin",
    "buttonconfig",
    // one representative file per asset class
    "e_ckna01.gxt",
    "ctw_iphone_intro.mp4",
  };
  struct stat st;
  const unsigned int numfiles = sizeof(files) / sizeof(*files);
  for (unsigned int i = 0; i < numfiles; ++i) {
    if (stat(files[i], &st) < 0) {
      fatal_error("Could not find\n%s.\nCheck your data files.", files[i]);
      break;
    }
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    // auto; pick resolution based on docked mode
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// ---------------------------------------------------------------------------
// gamepad: input is PUSHED into the game through JNI entry points.
// Android's Java InputHandler converts KeyEvent codes before calling native:
// BUTTON_A/B/X/Y -> 0/1/2/3, START/MODE -> 4, L1/R1 -> 6/7,
// THUMBL/THUMBR -> 12/13, BACK/MENU -> 14/15. Same InputHandler class as
// Max Payne 2.1.131; assumed identical until runtime proves otherwise.
// ---------------------------------------------------------------------------

#define GPAD_BUTTON_A 0
#define GPAD_BUTTON_B 1
#define GPAD_BUTTON_X 2
#define GPAD_BUTTON_Y 3
#define GPAD_BUTTON_START 4
#define GPAD_BUTTON_L1 6
#define GPAD_BUTTON_R1 7
#define GPAD_BUTTON_THUMBL 12
#define GPAD_BUTTON_THUMBR 13
#define GPAD_BUTTON_BACK 14
#define GPAD_BUTTON_MENU 15

typedef struct {
  u64 hid;
  int button;
} PadMap;

// direct buttons mapping (Switch A -> Android A, Switch B -> Android B, etc.)
static const PadMap pad_map[] = {
  { HidNpadButton_A, GPAD_BUTTON_A },
  { HidNpadButton_B, GPAD_BUTTON_B },
  { HidNpadButton_X, GPAD_BUTTON_X },
  { HidNpadButton_Y, GPAD_BUTTON_Y },
  { HidNpadButton_L, GPAD_BUTTON_L1 },
  { HidNpadButton_R, GPAD_BUTTON_R1 },
  { HidNpadButton_StickL, GPAD_BUTTON_THUMBL },
  { HidNpadButton_StickR, GPAD_BUTTON_THUMBR },
  { HidNpadButton_Plus, GPAD_BUTTON_START },
  // if pause/back ends up dead, route Minus through implOnBackButtonPressed
  // instead (the Java side has both paths)
  { HidNpadButton_Minus, GPAD_BUTTON_BACK },
};

// JNI entry points of libGame.so; Java-side signatures from classes2.dex
static void (* implOnActivityCreated)(void *env, void *cls, void *services, int isFirstRun);
static void (* implOnActivityDestroyed)(void *env, void *cls);
static void (* implOnInitialSetup)(void *env, void *cls, void *deviceInfo, void *assetMgr, void *paths, void *args);
static void (* implOnSurfaceCreated)(void *env, void *cls);
static void (* implOnSurfaceChanged)(void *env, void *cls, void *surface, int w, int h);
static void (* implOnSurfaceDestroyed)(void *env, void *cls);
static void (* implOnGameResume)(void *env, void *cls);
static void (* implOnGamePause)(void *env, void *cls);
static void (* implOnLowMemory)(void *env, void *cls);
static void (* implOnBackButtonPressed)(void *env, void *cls);
static void (* implOnDrawFrame)(void *env, void *cls, float dt);
static void (* implOnGamepadConnected)(void *env, void *cls, int id);
static void (* implOnGamepadDisconnected)(void *env, void *cls, int id);
static void (* implOnGamepadResume)(void *env, void *cls, void *ids);
static void (* implOnGamepadButtonDown)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadButtonUp)(void *env, void *cls, int id, int keycode);
static void (* implOnGamepadAxesChanged)(void *env, void *cls, int id, float lx, float ly, float rx, float ry, float lt, float rt);
static void (* implOnAccelerometerChanged)(void *env, void *cls, float x, float y, float z);
static void (* implOnTouchStart)(void *env, void *cls, int id, float x, float y);
static void (* implOnTouchMove)(void *env, void *cls, int id, float x, float y);
static void (* implOnTouchEnd)(void *env, void *cls, int id, float x, float y);
static void (* implOnNetworkChanged)(void *env, void *cls, int type);
static void (* implOnPlaylistOpenComplete)(void *env, void *cls, int success, int count);
static void (* implOnRockstarCloudDisabledComplete)(void *env, void *cls);
static void (* implOnRockstarGateComplete)(void *env, void *cls, int accepted);
static void (* implOnRockstarIdChanged)(void *env, void *cls, void *id);
static void (* implOnRockstarInitialComplete)(void *env, void *cls);
static void (* implOnRockstarSetup)(void *env, void *cls, void *environment, void *title_id);
static void (* implOnRockstarSignInComplete)(void *env, void *cls);
static void (* implOnRockstarSignOutComplete)(void *env, void *cls);
static void (* implOnRockstarStateChanged)(void *env, void *cls, int signed_in);
static void (* implOnRockstarTicketChanged)(void *env, void *cls, void *ticket);
static int  (* implIsInitialized)(void *env, void *cls);

static void resolve_entry_points(void) {
  #define ENTRY(var, sym) var = (void *)so_find_addr_rx(&game_mod, "Java_com_rockstargames_oswrapper_GameNative_" sym)
  ENTRY(implOnActivityCreated, "implOnActivityCreated");
  ENTRY(implOnActivityDestroyed, "implOnActivityDestroyed");
  ENTRY(implOnInitialSetup, "implOnInitialSetup");
  ENTRY(implOnSurfaceCreated, "implOnSurfaceCreated");
  ENTRY(implOnSurfaceChanged, "implOnSurfaceChanged");
  ENTRY(implOnSurfaceDestroyed, "implOnSurfaceDestroyed");
  ENTRY(implOnGameResume, "implOnGameResume");
  ENTRY(implOnGamePause, "implOnGamePause");
  ENTRY(implOnLowMemory, "implOnLowMemory");
  ENTRY(implOnBackButtonPressed, "implOnBackButtonPressed");
  ENTRY(implOnDrawFrame, "implOnDrawFrame");
  ENTRY(implOnGamepadConnected, "implOnGamepadConnected");
  ENTRY(implOnGamepadDisconnected, "implOnGamepadDisconnected");
  ENTRY(implOnGamepadResume, "implOnGamepadResume");
  ENTRY(implOnGamepadButtonDown, "implOnGamepadButtonDown");
  ENTRY(implOnGamepadButtonUp, "implOnGamepadButtonUp");
  ENTRY(implOnGamepadAxesChanged, "implOnGamepadAxesChanged");
  ENTRY(implOnAccelerometerChanged, "implOnAccelerometerChanged");
  ENTRY(implOnTouchStart, "implOnTouchStart");
  ENTRY(implOnTouchMove, "implOnTouchMove");
  ENTRY(implOnTouchEnd, "implOnTouchEnd");
  ENTRY(implOnNetworkChanged, "implOnNetworkChanged");
  ENTRY(implOnPlaylistOpenComplete, "implOnPlaylistOpenComplete");
  ENTRY(implOnRockstarCloudDisabledComplete, "implOnRockstarCloudDisabledComplete");
  ENTRY(implOnRockstarGateComplete, "implOnRockstarGateComplete");
  ENTRY(implOnRockstarIdChanged, "implOnRockstarIdChanged");
  ENTRY(implOnRockstarInitialComplete, "implOnRockstarInitialComplete");
  ENTRY(implOnRockstarSetup, "implOnRockstarSetup");
  ENTRY(implOnRockstarSignInComplete, "implOnRockstarSignInComplete");
  ENTRY(implOnRockstarSignOutComplete, "implOnRockstarSignOutComplete");
  ENTRY(implOnRockstarStateChanged, "implOnRockstarStateChanged");
  ENTRY(implOnRockstarTicketChanged, "implOnRockstarTicketChanged");
  ENTRY(implIsInitialized, "implIsInitialized");
  #undef ENTRY
  // silence -Wunused for entry points the loop doesn't drive yet
  (void)implOnLowMemory;
  (void)implOnAccelerometerChanged;
  (void)implOnGamepadDisconnected;
  (void)implOnBackButtonPressed;
}

// NOTE: max_nx also boosted the CPU during loading screens (detected via
// long draw calls from the eglSwapBuffers hook); CTW streams its open world
// continuously and has no comparable loads, so only the boot boost remains.

// ---------------------------------------------------------------------------
// touch HUD hiding: the engine tracks "controller in use" itself in
// gOSWGamepad (UpdateGamepad sets byte [1] on pad input, UpdateTouch clears
// it on a touch press), and ~50 UI subsystems already adapt to it -- but
// unlike Max Payne it never hides the cIPhonePad touch buttons. We do that
// here by forcing their sprite alphas to 0 through the game's own
// cIPhonePad::SetAlpha while the flag is set. Re-applied every frame
// because the HUD app fades the alphas back in during its transitions.
// ---------------------------------------------------------------------------

static void (* IPhonePad_SetAlpha)(void *pad, float alpha);
static void *iphone_pad;                 // &gIPhonePad
static uint8_t *osw_gamepad_active;      // &gOSWGamepad[0]; byte [1] = active
static int touch_hud_hidden = 0;

static void update_touch_hud(void) {
  if (!IPhonePad_SetAlpha)
    return;
  if (osw_gamepad_active[1]) {
    IPhonePad_SetAlpha(iphone_pad, 0.0f);
    touch_hud_hidden = 1;
  } else if (touch_hud_hidden) {
    // back in touch mode: restore once and let the game's own fades take
    // over again from there
    IPhonePad_SetAlpha(iphone_pad, 1.0f);
    touch_hud_hidden = 0;
  }
}

// ---------------------------------------------------------------------------
// touchscreen (handheld mode): finger states from the panel are translated
// into the start/move/end events android's view layer used to deliver.
// libnx reports coordinates in the panel's native 1280x720 space.
// ---------------------------------------------------------------------------

// the engine's pointer array (`pointers` in libGame.so) only has FOUR slots
// and indexes them by the id we pass without bounds checks: an id above 3
// corrupts the neighboring globals (LIB_InputEvent writes a 0x78-stride
// entry) and eventually crashes in the event queue's grow path. libnx
// finger ids are not guaranteed to stay below 4 either, so active fingers
// get mapped onto stable engine slots 0..3 and any extra finger is ignored.
#define MAX_TOUCHES 4

typedef struct {
  int active;
  u32 finger_id;
  float x, y;
} TouchSlot;

static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 finger_id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == finger_id)
      return i;
  return -1;
}

static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active)
      return i;
  return -1;
}

static void update_touch(void) {
  if (!config.touchscreen)
    return;

  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;

  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;

  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0)
        continue; // more fingers than engine slots: ignore the extras
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
      debugPrintf("touch start %d (%.0f, %.0f)\n", slot, x, y);
      implOnTouchStart(fake_env, NULL, slot, x, y);
    } else if (x != touch_prev[slot].x || y != touch_prev[slot].y) {
      implOnTouchMove(fake_env, NULL, slot, x, y);
    }
    touch_prev[slot].x = x;
    touch_prev[slot].y = y;
    seen[slot] = 1;
  }

  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      debugPrintf("touch end %d\n", slot);
      implOnTouchEnd(fake_env, NULL, slot, touch_prev[slot].x, touch_prev[slot].y);
      touch_prev[slot].active = 0;
    }
  }
}

static PadState pad;
static u64 pad_prev = 0;

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  for (unsigned int i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
    if (changed & pad_map[i].hid) {
      if (down & pad_map[i].hid) {
        debugPrintf("gamepad button down %d\n", pad_map[i].button);
        implOnGamepadButtonDown(fake_env, NULL, 0, pad_map[i].button);
        // the game ignores input while waiting for a movie
        movie_skip();
      } else {
        debugPrintf("gamepad button up %d\n", pad_map[i].button);
        implOnGamepadButtonUp(fake_env, NULL, 0, pad_map[i].button);
      }
    }
  }
  pad_prev = down;

  const float scale = 1.f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  float lx = (float)ls.x * scale;
  float ly = (float)ls.y * -scale;
  if (down & HidNpadButton_Left)  lx = -1.0f;
  if (down & HidNpadButton_Right) lx =  1.0f;
  if (down & HidNpadButton_Up)    ly = -1.0f;
  if (down & HidNpadButton_Down)  ly =  1.0f;

  // Android Y axes point down.
  const float axes[6] = {
    lx, ly,
    (float)rs.x * scale, (float)rs.y * -scale,
    (down & HidNpadButton_ZL) ? 1.0f : 0.0f,
    (down & HidNpadButton_ZR) ? 1.0f : 0.0f,
  };

  // only dispatch into the engine when something actually moved
  static float prev_axes[6];
  if (memcmp(axes, prev_axes, sizeof(axes)) != 0) {
    memcpy(prev_axes, axes, sizeof(axes));
    implOnGamepadAxesChanged(fake_env, NULL, 0,
        axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
  }
}

static void dispatch_rockstar_events(void) {
  JniRockstarEvent event;
  int dispatched = 0;
  while (dispatched < 32 && jni_pop_pending_rockstar(&event)) {
    dispatched++;
    debugPrintf("JNI: dispatch rockstar event %d\n", event.type);
    switch (event.type) {
      case JNI_RS_INITIAL_COMPLETE:
        implOnRockstarInitialComplete(fake_env, NULL);
        break;
      case JNI_RS_SIGN_IN_COMPLETE:
        implOnRockstarSignInComplete(fake_env, NULL);
        break;
      case JNI_RS_SIGN_OUT_COMPLETE:
        implOnRockstarSignOutComplete(fake_env, NULL);
        break;
      case JNI_RS_CLOUD_DISABLED_COMPLETE:
        implOnRockstarCloudDisabledComplete(fake_env, NULL);
        break;
      case JNI_RS_GATE_COMPLETE:
        // unlike Max Payne, CTW's callback only takes the accepted flag
        implOnRockstarGateComplete(fake_env, NULL, event.accepted);
        break;
      case JNI_RS_STATE_CHANGED:
        implOnRockstarStateChanged(fake_env, NULL, event.signed_in);
        break;
      case JNI_RS_ID_CHANGED:
        implOnRockstarIdChanged(fake_env, NULL, event.value ? event.value : jni_make_string(""));
        break;
      case JNI_RS_TICKET_CHANGED:
        implOnRockstarTicketChanged(fake_env, NULL, event.value ? event.value : jni_make_string(""));
        break;
      case JNI_RS_SETUP:
        implOnRockstarSetup(fake_env, NULL, event.environment, event.title_id);
        break;
      default:
        debugPrintf("JNI: unknown rockstar event %d\n", event.type);
        break;
    }
  }
  if (dispatched == 32)
    debugPrintf("JNI: rockstar dispatch batch limit reached\n");
}

static void dispatch_playlist_events(void) {
  JniPlaylistEvent event;
  while (jni_pop_pending_playlist(&event)) {
    debugPrintf("JNI: dispatch playlistOpenComplete(%d, %d)\n", event.success, event.count);
    implOnPlaylistOpenComplete(fake_env, NULL, event.success, event.count);
  }
}

int main(void) {
  // run the CPU at full clock for the entire boot; dropped back to normal
  // once the menu has rendered a few frames (see the main loop)
  cpu_boost(1);

  // try to read the config file and create one with default values if it's missing
  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();

  // calculate actual screen size
  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("heap size = %u KB\n", MEMORY_MB * 1024);
  debugPrintf(" lib base = %p\n", heap_so_base);
  debugPrintf("  lib max = %u KB\n", heap_so_limit / 1024);

  // load the C++ runtime donor first, then the game; the game's std::/__cxa_
  // imports resolve into the donor's statically-linked libc++ (the import
  // table takes priority, so the donor never serves OpenAL)
  if (so_load(&donor_mod, CXX_DONOR_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", CXX_DONOR_SO_NAME);

  void *game_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + donor_mod.load_size, 0x1000);
  const size_t game_limit = heap_so_limit - ((uintptr_t)game_base - (uintptr_t)heap_so_base);
  if (so_load(&game_mod, SO_NAME, game_base, game_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();

  so_relocate(&donor_mod);
  so_relocate(&game_mod);
  so_resolve(&donor_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();

  // can't set it in the initializer because it's not constant
  stderr_fake = stderr;

  // resolve before finalize: lookups after free_temp still work, but
  // grabbing everything now keeps the failure point early and obvious
  resolve_entry_points();
  int (* JNI_OnLoad)(void *vm, void *reserved) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  void (* NVThreadInit)(void *vm) = (void *)so_find_addr_rx(&game_mod, "_Z12NVThreadInitP7_JavaVM");

  if (config.hide_touch_hud) {
    IPhonePad_SetAlpha = (void *)so_try_find_addr_rx(&game_mod, "_ZN10cIPhonePad8SetAlphaEf");
    iphone_pad = (void *)so_try_find_addr_rx(&game_mod, "gIPhonePad");
    osw_gamepad_active = (uint8_t *)so_try_find_addr_rx(&game_mod, "gOSWGamepad");
    if (!iphone_pad || !osw_gamepad_active)
      IPhonePad_SetAlpha = NULL; // feature off if anything is missing
  }

  so_finalize(&donor_mod);
  so_finalize(&game_mod);
  so_flush_caches(&donor_mod);
  so_flush_caches(&game_mod);

  // the donor's initializers (libc++ statics) must run before the game's
  so_execute_init_array(&donor_mod);
  so_execute_init_array(&game_mod);

  so_free_temp(&donor_mod);
  so_free_temp(&game_mod);

  jni_init();
  NVThreadInit(fake_vm);

  debugPrintf("calling JNI_OnLoad\n");
  JNI_OnLoad(fake_vm, NULL);

  // fake Java objects handed to the game
  void *services = jni_make_object("GamePlatformServices");
  void *device_info = jni_make_object("DeviceInfo");
  void *asset_mgr = jni_make_object("AssetManager");
  void *surface = jni_make_object("Surface");
  // TODO: contents/order of these two unknown; Android passes two string
  // arrays built by GameThread.onInitialSetup (likely file/cache dirs)
  const char *path_strs[] = { ".", "." };
  void *paths = jni_make_string_array(2, path_strs);
  const char *arg_strs[] = { "", "" };
  void *args = jni_make_string_array(2, arg_strs);

  debugPrintf("implOnActivityCreated\n");
  implOnActivityCreated(fake_env, NULL, services, 0);

  debugPrintf("implOnInitialSetup\n");
  implOnInitialSetup(fake_env, NULL, device_info, asset_mgr, paths, args);

  debugPrintf("implOnSurfaceCreated\n");
  implOnSurfaceCreated(fake_env, NULL);

  debugPrintf("implOnSurfaceChanged %dx%d\n", screen_width, screen_height);
  implOnSurfaceChanged(fake_env, NULL, surface, screen_width, screen_height);

  // tell the game we're offline so the Rockstar services don't wait around
  implOnNetworkChanged(fake_env, NULL, 0);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();
  implOnGamepadConnected(fake_env, NULL, 0);
  // the Java side also reports the already-connected pad set on resume
  const int pad_ids[] = { 0 };
  implOnGamepadResume(fake_env, NULL, jni_make_int_array(1, pad_ids));

  debugPrintf("implOnGameResume\n");
  implOnGameResume(fake_env, NULL);

  debugPrintf("implIsInitialized = %d\n", implIsInitialized(fake_env, NULL));

  // main loop, replicating what GameThread/GameView did on android
  const u64 tick_freq = armGetSystemTickFreq();
  u64 last_tick = armGetSystemTick();
  int boot_frames = 0;

  while (appletMainLoop() && !jni_quit_requested) {
    dispatch_rockstar_events();
    dispatch_playlist_events();

    update_gamepad();
    update_touch();
    update_touch_hud();

    const u64 now = armGetSystemTick();
    float dt = (float)(now - last_tick) / (float)tick_freq;
    last_tick = now;
    if (dt <= 0.0f || dt > 0.5f)
      dt = 1.0f / 60.0f;

    implOnDrawFrame(fake_env, NULL, dt);

    // keeps movie playback going when the game stops rendering during it
    movie_main_loop_tick();

    if (boot_frames < 10) {
      // the heavy boot loading happens inside the first few draw frames;
      // once the menu is up, return to normal clocks
      if (++boot_frames == 10)
        cpu_boost(0);
    }
  }

  debugPrintf("shutting down\n");
  implOnGamePause(fake_env, NULL);
  implOnSurfaceDestroyed(fake_env, NULL);
  implOnActivityDestroyed(fake_env, NULL);

  movie_stop();
  deinit_openal();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);

  return 0;
}
