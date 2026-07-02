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

// make resume load the latest save
static int  (*CGenericGameStorage__CheckSlotDataValid)(int slot, int del);
static void (*C_PcSave__GenerateGameFilename)(void *this, int slot, char *out);
static uint64_t (*OS_FileGetDate)(int area, const char *path);
static void *PcSaveHelper;   // points into game data segment at runtime
static int  *lastSaveForResume;

static int MainMenuScreen__HasCPSave(void) {
  if (*lastSaveForResume == -1) {
    uint64_t latest = 0;
    for (int i = 0; i < 10; i++) {
      char filename[256];
      C_PcSave__GenerateGameFilename(&PcSaveHelper, i, filename);
      uint64_t date = OS_FileGetDate(1, filename);
      if (latest < date) {
        latest = date;
        *lastSaveForResume = i;
      }
    }
  }
  return CGenericGameStorage__CheckSlotDataValid(*lastSaveForResume, 1);
}

// support graceful exit
static int (*SaveGameForPause)(int type, char *cmd);

static int MainMenuScreen__OnExit(void) {
  SaveGameForPause(3, NULL);
  jni_quit_requested = 1;
  return 0;
}

// Emergency-vehicle / widescreen FOV fix.
static float *CDraw__ms_fFOV;
static float *CDraw__ms_fAspectRatio;
// hidden (not static): referenced by fov_stub.s; hidden keeps adrp/add PIC-valid
__attribute__((visibility("hidden"))) float fake_fov;

static float CDraw__SetFOV(float fov) {
  *CDraw__ms_fFOV =
      (((*CDraw__ms_fAspectRatio - 1.3333f) * 11.0f) / 0.44444f) + fov;
  fake_fov =
      (((1.0f / *CDraw__ms_fAspectRatio - 1.3333f) * 11.0f) / 0.44444f) + fov;
  return fake_fov;
}

// Half 2 of the FOV fix.
__attribute__((visibility("hidden"))) uintptr_t ccamera_fov_ret;
extern void CCamera__Process_fov_stub(void); // fov_stub.s

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

  // Ignore app rating popup
  if (so_try_find_addr_rx(&game_mod, "_Z12Menu_ShowNagv"))
    hook_arm64(so_find_addr(&game_mod, "_Z12Menu_ShowNagv"), (uintptr_t)ret0);

  // Ignore side mission buttons (vigilante, paramedic, etc)
  if (so_try_find_addr_rx(&game_mod, "_ZN25CWidgetButtonMissionStart6UpdateEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN25CWidgetButtonMissionStart6UpdateEv"),
               (uintptr_t)ret0);
  if (so_try_find_addr_rx(&game_mod, "_ZN26CWidgetButtonMissionCancel6UpdateEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN26CWidgetButtonMissionCancel6UpdateEv"),
               (uintptr_t)ret0);
  
  // Ignore cloud saves
  if (so_try_find_addr_rx(&game_mod, "UseCloudSaves"))
    *(uint8_t *)so_find_addr(&game_mod, "UseCloudSaves") = 0;

  // make resume load the latest save
  CGenericGameStorage__CheckSlotDataValid =
    (void *)so_find_addr_rx(&game_mod, "_ZN19CGenericGameStorage18CheckSlotDataValidEib");
  C_PcSave__GenerateGameFilename =
    (void *)so_find_addr_rx(&game_mod, "_ZN8C_PcSave20GenerateGameFilenameEiPc");
  OS_FileGetDate =
    (void *)so_find_addr_rx(&game_mod, "_Z14OS_FileGetDate14OSFileDataAreaPKc");
  PcSaveHelper =
    (void *)so_find_addr_rx(&game_mod, "PcSaveHelper");
  lastSaveForResume =
    (int *)so_find_addr_rx(&game_mod, "lastSaveForResume");
  if (so_try_find_addr_rx(&game_mod, "_ZN14MainMenuScreen9HasCPSaveEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14MainMenuScreen9HasCPSaveEv"),
               (uintptr_t)MainMenuScreen__HasCPSave);

  // support graceful exit
  SaveGameForPause =
    (void *)so_find_addr_rx(&game_mod, "_Z16SaveGameForPause10eSaveTypesPc");
  if (so_try_find_addr_rx(&game_mod, "_ZN14MainMenuScreen6OnExitEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14MainMenuScreen6OnExitEv"),
               (uintptr_t)MainMenuScreen__OnExit);

  // Fixed broken facial expressions
  if (so_try_find_addr_rx(&game_mod,
        "_Z27_rpSkinOpenGLPipelineCreate10RpSkinTypePFvP10RwResEntryPvhjE")) {
    uint32_t *fn = (uint32_t *)so_find_addr(&game_mod,
        "_Z27_rpSkinOpenGLPipelineCreate10RpSkinTypePFvP10RwResEntryPvhjE");
    fn[0x328 / 4] = 0x52800081; // mov w1,  #4       (weight comps: 3 -> 4)
    fn[0x32c / 4] = 0x52828062; // mov w2,  #0x1403  (GL_UNSIGNED_BYTE -> _SHORT)
    fn[0x334 / 4] = 0x5280011c; // mov w28, #8       (stride advance: 4 -> 8)
    fn[0x338 / 4] = 0x52800099; // mov w25, #4       (index comps: 3 -> 4)
    fn[0x340 / 4] = 0x5280003a; // mov w26, #1       (short-format flag: 0 -> 1)
  }

  // Emergency-vehicle / widescreen FOV fix
  if (so_try_find_addr_rx(&game_mod, "_ZN5CDraw6SetFOVEfb")) {
    CDraw__ms_fFOV = (float *)so_find_addr_rx(&game_mod, "_ZN5CDraw7ms_fFOVE");
    CDraw__ms_fAspectRatio =
        (float *)so_find_addr_rx(&game_mod, "_ZN5CDraw15ms_fAspectRatioE");
    hook_arm64(so_find_addr(&game_mod, "_ZN5CDraw6SetFOVEfb"),
               (uintptr_t)CDraw__SetFOV);

    // half 2
    if (so_try_find_addr_rx(&game_mod, "_ZN7CCamera7ProcessEv")) {
      ccamera_fov_ret =
          so_find_addr_rx(&game_mod, "_ZN7CCamera7ProcessEv") + 0xDC8;
      hook_arm64(so_find_addr(&game_mod, "_ZN7CCamera7ProcessEv") + 0xDB8,
                 (uintptr_t)CCamera__Process_fov_stub);
    }
  }

  // NOTE: "Nuke telemetry"
  // Stop Android file/billing update callbacks
  if (so_try_find_addr_rx(&game_mod, "_Z14AND_FileUpdated"))
    hook_arm64(so_find_addr(&game_mod, "_Z14AND_FileUpdated"), (uintptr_t)ret0);
  if (so_try_find_addr_rx(&game_mod, "_Z17AND_BillingUpdateb"))
    hook_arm64(so_find_addr(&game_mod, "_Z17AND_BillingUpdateb"), (uintptr_t)ret0);

  // No adjustable HUD (skip saving/repositioning of movable touch widgets)
  if (so_try_find_addr_rx(&game_mod, "_ZN14CAdjustableHUD10SaveToDiskEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14CAdjustableHUD10SaveToDiskEv"),
               (uintptr_t)ret0);
  if (so_try_find_addr_rx(&game_mod, "_ZN15CTouchInterface27RepositionAdjustableWidgetsEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN15CTouchInterface27RepositionAdjustableWidgetsEv"),
               (uintptr_t)ret0);

  // IsRemovedTrack -> 0 (radio: don't treat tracks as removed)
  if (so_try_find_addr_rx(&game_mod, "_Z14IsRemovedTracki"))
    hook_arm64(so_find_addr(&game_mod, "_Z14IsRemovedTracki"), (uintptr_t)ret0);

  // Disable touch-sense (rumble-on-touch) flag; data symbol, write 0
  if (so_try_find_addr_rx(&game_mod, "UseTouchSense"))
    *(uint8_t *)so_find_addr(&game_mod, "UseTouchSense") = 0;

  // pin the main/logic thread to core 0 (render = core 1, streaming/audio = core 2)
  set_thread_core(0);
}
