/* game.c -- hooks and patches for everything other than AL and GL
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * CTW 4.4.243: the platform layer is serviced through the fake JNI
 * environment (jni_fake.c); only engine-level patches remain here. The
 * thread/TLS plumbing is identical to Max Payne 2.1.131 -- the mangled
 * names below were verified against this exact libGame.so build.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <threads.h>
#include <switch.h>

#include "../config.h"
#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../jni_fake.h"

extern so_module game_mod; // defined in main.c

// handle the game gets back from OS_ThreadLaunch; opaque to the game, but
// unlike Max Payne, CTW also exports OS_ThreadWait/Close/IsRunning, so we
// hook the whole lifecycle and keep everything the hooks need in here
typedef struct {
  int (*func)(void *);
  void *arg;
  thrd_t thrd;
  volatile int done;
  int joined;
  uint8_t tls[0x100];
} OSThreadHandle;

typedef struct {
  void *(*func)(void *);
  void *arg;
  uint8_t tls[0x100];
} NVThreadStart;

static uint8_t main_fake_tls[0x100];

static void init_fake_tls(uint8_t *tls) {
  memset(tls, 0, 0x100);
  armSetTlsRw(tls);
}

// OS_ScreenGetWidth/Height are 12-byte stubs sitting back to back (and
// AND_GameCanRender starts right after them), so a 16-byte hook_arm64 on
// either overwrites its neighbor -- this crashed the first boot. Their
// bodies get rewritten in place instead: MOVZ W0, #value; RET. The screen
// size is fixed for the lifetime of the process, so baking in the constant
// is equivalent to the old OS_ScreenGet* hook functions.
static void patch_return_int(uintptr_t addr, int value) {
  uint32_t *insn = (uint32_t *)addr;
  insn[0] = 0x52800000u | ((uint32_t)(value & 0xFFFF) << 5); // movz w0, #value
  insn[1] = 0xd65f03c0u; // ret
}

static int os_thread_trampoline(void *arg) {
  OSThreadHandle *h = arg;
  init_fake_tls(h->tls);
  const int rc = h->func(h->arg);
  h->done = 1;
  return rc;
}

static int nv_thread_trampoline(void *arg) {
  NVThreadStart *start = arg;
  void *(*func)(void *) = start->func;
  void *user_arg = start->arg;
  init_fake_tls(start->tls);
  void *rc = func(user_arg);
  // start->tls stays installed in TPIDR_EL0 through thread teardown, so the
  // block is leaked on purpose (0x100+ bytes per spawned thread)
  return (int)(intptr_t)rc;
}

void *OS_ThreadLaunch(int (* func)(void *), void *arg, int r2, char *name, int r4, int priority) {
  (void)r2; (void)r4; (void)priority;
  debugPrintf("OS_ThreadLaunch: %s\n", name ? name : "(unnamed)");
  OSThreadHandle *h = calloc(1, sizeof(*h));
  if (!h)
    return NULL;
  h->func = func;
  h->arg = arg;
  if (thrd_create(&h->thrd, os_thread_trampoline, h) != thrd_success) {
    free(h);
    return NULL;
  }
  return h;
}

int OS_ThreadWait(void *handle) {
  OSThreadHandle *h = handle;
  if (h && !h->joined) {
    int res;
    thrd_join(h->thrd, &res);
    h->joined = 1;
  }
  return 0;
}

void OS_ThreadClose(void *handle) {
  OSThreadHandle *h = handle;
  if (h) {
    if (!h->joined)
      thrd_detach(h->thrd);
    free(h);
  }
}

int OS_ThreadIsRunning(void *handle) {
  OSThreadHandle *h = handle;
  return h && !h->done;
}

// the game spawns its loader/sound threads through this NVThread wrapper
int NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name, void *(*fn)(void *), void *arg) {
  (void)attr;
  debugPrintf("NVThreadSpawnJNIThread: %s\n", name ? name : "(unnamed)");
  NVThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return -1;
  start->func = fn;
  start->arg = arg;
  thrd_t thrd;
  if (thrd_create(&thrd, nv_thread_trampoline, start) != thrd_success) {
    free(start);
    return -1;
  }
  if (tid)
    *tid = (long)thrd;
  return 0;
}

// always hand out our fake JNIEnv; the real one TLS-caches a env pointer
// that we can't provide
void *NVThreadGetCurrentJNIEnv(void) {
  return fake_env;
}

void patch_game(void) {
  // route JNIEnv access through the fake environment
  hook_arm64(so_find_addr(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv"), (uintptr_t)NVThreadGetCurrentJNIEnv);
  hook_arm64(so_find_addr(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"), (uintptr_t)NVThreadSpawnJNIThread);

  hook_arm64(so_find_addr(&game_mod, "_Z15OS_ThreadLaunchPFjPvES_jPKci16OSThreadPriority"), (uintptr_t)OS_ThreadLaunch);
  hook_arm64(so_find_addr(&game_mod, "_Z13OS_ThreadWaitPv"), (uintptr_t)OS_ThreadWait);
  hook_arm64(so_find_addr(&game_mod, "_Z14OS_ThreadClosePv"), (uintptr_t)OS_ThreadClose);
  hook_arm64(so_find_addr(&game_mod, "_Z18OS_ThreadIsRunningPv"), (uintptr_t)OS_ThreadIsRunning);

  patch_return_int(so_find_addr(&game_mod, "_Z17OS_ScreenGetWidthv"), screen_width);
  patch_return_int(so_find_addr(&game_mod, "_Z18OS_ScreenGetHeightv"), screen_height);

  // HACK: THIS IS POSSIBLY VERY BAD
  // the game uses some sort of a stack guard mechanism that reads an offset from TPIDR_EL0,
  // reads a cookie value from that offset and then uses it to check stack frame integrity
  // however on the Switch TPIDR_EL0 seems to just return 0 (armGetTls() uses TPIDRRO_EL0)
  // I don't know whether this will cause any issues or not, but we just write a pointer to
  // a static buffer to TPIDR_EL0 and let the game use that
  init_fake_tls(main_fake_tls);
}
