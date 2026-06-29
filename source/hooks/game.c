/* game.c -- hooks and patches for everything other than AL and GL
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <threads.h>
#include <switch.h>

#include "../config.h"
#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../jni_fake.h"

extern so_module game_mod; // defined in main.c

// fake TLS for the AArch64 stack guard: guarded functions read their cookie at
// [TPIDR_EL0, #0x28], but libnx leaves TPIDR_EL0 unusable for that, so point it
// at a static buffer (spawned threads get the same in pthread_create_fake).
static uint8_t main_fake_tls[0x100];

static void init_fake_tls(uint8_t *tls) {
  memset(tls, 0, 0x100);
  armSetTlsRw(tls);
}

// Always hand out the fake JNIEnv: our engine threads aren't attached through a
// real JavaVM, so the stock TLS-cached env lookup would return garbage.
void *NVThreadGetCurrentJNIEnv(void) {
  return fake_env;
}

// NVThreadSpawnJNIThread replacement: the stock trampoline NULL-faults on its
// JNI-attach path for named threads (it derefs a per-thread JNIEnv only the real
// JavaVM attach would set). Just spawn the thread with the fake stack-guard TLS;
// NVThreadGetCurrentJNIEnv (above) hands it the fake env.
typedef struct {
  void *(*func)(void *);
  void *arg;
  int core;
  uint8_t tls[0x100];
} NVThreadStart;

static int nv_thread_trampoline(void *arg) {
  NVThreadStart *start = arg;
  void *(*func)(void *) = start->func;
  void *user_arg = start->arg;
  set_thread_core(start->core);     // spread engine threads across cores
  init_fake_tls(start->tls);
  void *rc = func(user_arg);
  // tls stays in TPIDR_EL0 through teardown, so the block is leaked on purpose
  return (int)(intptr_t)rc;
}

// _Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_
// NVThreadSpawnJNIThread(long*, pthread_attr_t const*, char const*,
//                        void* (*)(void*), void*)
int NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name,
                           void *(*fn)(void *), void *arg) {
  (void)attr;
  debugPrintf("NVThreadSpawnJNIThread: %s\n", name ? name : "(unnamed)");
  NVThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return -1;
  start->func = fn;
  start->arg = arg;
  // CPU split: core 0 = logic, core 1 = GL render thread, core 2 = everything else
  start->core = (name && strcmp(name, "RenderQueue") == 0) ? 1 : 2;
  thrd_t thrd;
  if (thrd_create(&thrd, nv_thread_trampoline, start) != thrd_success) {
    free(start);
    return -1;
  }
  if (tid)
    *tid = (long)thrd;
  return 0;
}

// OS_ScreenGetWidth/Height feed the engine's render-target sizing; report our
// configured render resolution directly.
static int os_screen_get_width(void)  { return screen_width; }
static int os_screen_get_height(void) { return screen_height; }

// '+' -> pause menu: CPad::GetEscapeJustDown otherwise only consults the
// keyboard/touch widget, so a controller can't open the pause menu. Fire it on
// the '+' edge main.c records in g_escape_pressed.
extern volatile int g_escape_pressed; // main.c
static int GetEscapeJustDown_hook(void) {
  if (g_escape_pressed) {
    g_escape_pressed = 0;
    return 1;
  }
  return 0;
}

void patch_game(void) {
  // replace the NVThread JNI-thread spawner (NULL-faults on named threads)
  if (so_try_find_addr_rx(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"))
    hook_arm64(so_find_addr(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"),
               (uintptr_t)NVThreadSpawnJNIThread);

  // route per-thread JNIEnv lookups to the fake environment
  if (so_try_find_addr_rx(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv"))
    hook_arm64(so_find_addr(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv"),
               (uintptr_t)NVThreadGetCurrentJNIEnv);

  // report our render resolution to the engine
  if (so_try_find_addr_rx(&game_mod, "_Z17OS_ScreenGetWidthv"))
    hook_arm64(so_find_addr(&game_mod, "_Z17OS_ScreenGetWidthv"),
               (uintptr_t)os_screen_get_width);
  if (so_try_find_addr_rx(&game_mod, "_Z18OS_ScreenGetHeightv"))
    hook_arm64(so_find_addr(&game_mod, "_Z18OS_ScreenGetHeightv"),
               (uintptr_t)os_screen_get_height);

  // route '+' to the pause menu via GetEscapeJustDown (see g_escape_pressed)
  if (so_try_find_addr_rx(&game_mod, "_ZN4CPad17GetEscapeJustDownEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN4CPad17GetEscapeJustDownEv"),
               (uintptr_t)GetEscapeJustDown_hook);

  // main-thread stack-guard TLS
  init_fake_tls(main_fake_tls);

  // pin the main/logic thread to core 0 (render = core 1, streaming/audio = core 2)
  set_thread_core(0);
}
