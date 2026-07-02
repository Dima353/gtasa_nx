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
#include <math.h>
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

// Water physics fix
typedef struct { float x, y, z; } SwVec;

// arm64 field offsets (ARMv7 -> arm64):
#define TASK_STATE(t)  (*(uint16_t *)((char *)(t) + 0x12)) // m_nSwimState   0x0A
#define TASK_ROTX(t)   (*(float *)((char *)(t) + 0x30))    // m_fRotationX   0x24
#define TASK_SCHG(t)   (*(float *)((char *)(t) + 0x40))    // m_fStateChanger 0x34
#define PED_CLUMP(p)   (*(void **)((char *)(p) + 0x20))    // m_pRwClump     0x18
#define PED_MAT(p)     (*(void **)((char *)(p) + 0x18))    // matrix ptr     0x14
#define PED_SHIFTX(p)  (*(float *)((char *)(p) + 0x634))   // m_vecAnimMovingShiftLocal.x 0x4E4
#define PED_SHIFTY(p)  (*(float *)((char *)(p) + 0x638))   // .y             0x4E8
#define PED_VEL(p)     (*(SwVec *)((char *)(p) + 0x68))    // m_vecMoveSpeed 0x48
#define PED_MASS(p)    (*(float *)((char *)(p) + 0xB0))    // m_fMass        0x8C
#define ANIM_BLEND(a)  (*(float *)((char *)(a) + 0x30))    // m_fBlendAmount 0x18
#define ANIM_TIME(a)   (*(float *)((char *)(a) + 0x38))    // m_fCurrentTime 0x20
#define ANIM_DELTA(a)  (*(float *)((char *)(a) + 0x34))    // m_fBlendDelta  0x1C
#define ANIM_HIER(a)   (*(void **)((char *)(a) + 0x28))    // m_pAnimBlendHierarchy 0x14
#define HIER_TOTAL(h)  (*(float *)((char *)(h) + 0x18))    // m_fTotalTime   0x10
#define MAT_RIGHT(m)   (*(SwVec *)((char *)(m) + 0x00))    // GetRight
#define MAT_FWD(m)     (*(SwVec *)((char *)(m) + 0x10))    // GetForward
#define MAT_POS(m)     (*(SwVec *)((char *)(m) + 0x30))    // GetPosition

enum { SWIM_TREAD, SWIM_SPRINT, SWIM_SPRINTING, SWIM_DIVE_UNDERWATER,
       SWIM_UNDERWATER_SPRINTING, SWIM_BACK_TO_SURFACE };
enum { ANIM_ID_SWIM_BREAST = 311, ANIM_ID_SWIM_CRAWL = 312,
       ANIM_ID_SWIM_DIVE_UNDER = 313, ANIM_ID_SWIM_JUMPOUT = 316,
       ANIM_ID_CLIMB_JUMP = 128 };

static void *(*sw_GetAnim)(void *clump, uint32_t animId);
static void  (*sw_ApplyMoveForce)(void *ped, float x, float y, float z);
static uint8_t (*sw_IsPlayer)(void *ped);
static uint8_t (*sw_GetWaterLevel)(float x, float y, float z, float *out,
                                   uint8_t touching, void *normals);
static float *sw_ms_fTimeStep;

static SwVec sw_mul(SwVec v, float s) { SwVec r = { v.x*s, v.y*s, v.z*s }; return r; }
static SwVec sw_add(SwVec a, SwVec b) { SwVec r = { a.x+b.x, a.y+b.y, a.z+b.z }; return r; }
static float sw_min(float a, float b) { return a < b ? a : b; }
static float sw_max(float a, float b) { return a > b ? a : b; }
static float sw_clamp(float v, float lo, float hi) { return sw_min(sw_max(v, lo), hi); }
static float sw_StepSeconds(void) { return *sw_ms_fTimeStep / 50.0f; }
static float sw_StepMagic(void)   { return *sw_ms_fTimeStep / (50.0f / 30.0f); }
static float sw_StepInvMagic(void){ return 50.0f / 30.0f / *sw_ms_fTimeStep; }

static void ProcessSwimmingResistance(void *task, void *ped) {
  float fSubmergeZ = -1.0f;
  SwVec vel = { 0, 0, 0 };

  switch (TASK_STATE(task)) {
    case SWIM_TREAD:
    case SWIM_SPRINT:
    case SWIM_SPRINTING: {
      float sum = 0.0f, diff = 1.0f;
      void *aBreast = sw_GetAnim(PED_CLUMP(ped), ANIM_ID_SWIM_BREAST);
      if (aBreast) { sum = 0.4f * ANIM_BLEND(aBreast); diff = 1.0f - ANIM_BLEND(aBreast); }
      void *aCrawl = sw_GetAnim(PED_CLUMP(ped), ANIM_ID_SWIM_CRAWL);
      if (aCrawl) { sum += 0.2f * ANIM_BLEND(aCrawl); diff -= ANIM_BLEND(aCrawl); }
      if (diff < 0.0f) diff = 0.0f;
      fSubmergeZ = diff * 0.55f + sum;
      vel = sw_mul(MAT_RIGHT(PED_MAT(ped)), PED_SHIFTX(ped));
      vel = sw_add(vel, sw_mul(MAT_FWD(PED_MAT(ped)), PED_SHIFTY(ped)));
      break;
    }
    case SWIM_DIVE_UNDERWATER: {
      vel = sw_mul(MAT_RIGHT(PED_MAT(ped)), PED_SHIFTX(ped));
      vel = sw_add(vel, sw_mul(MAT_FWD(PED_MAT(ped)), PED_SHIFTY(ped)));
      void *aDive = sw_GetAnim(PED_CLUMP(ped), ANIM_ID_SWIM_DIVE_UNDER);
      if (aDive)
        vel.z = ANIM_TIME(aDive) / HIER_TOTAL(ANIM_HIER(aDive)) * (-0.1f * sw_StepMagic());
      break;
    }
    case SWIM_UNDERWATER_SPRINTING: {
      vel = sw_mul(MAT_RIGHT(PED_MAT(ped)), PED_SHIFTX(ped));
      vel = sw_add(vel, sw_mul(MAT_FWD(PED_MAT(ped)), cosf(TASK_ROTX(task)) * PED_SHIFTY(ped)));
      vel.z += (sinf(TASK_ROTX(task)) * PED_SHIFTY(ped) + 0.01f) / sw_StepMagic();
      break;
    }
    case SWIM_BACK_TO_SURFACE: {
      void *aClimb = sw_GetAnim(PED_CLUMP(ped), ANIM_ID_CLIMB_JUMP);
      if (!aClimb) aClimb = sw_GetAnim(PED_CLUMP(ped), ANIM_ID_SWIM_JUMPOUT);
      if (aClimb && HIER_TOTAL(ANIM_HIER(aClimb)) > ANIM_TIME(aClimb) &&
          (ANIM_BLEND(aClimb) >= 1.0f || ANIM_DELTA(aClimb) > 0.0f)) {
        float fMoveForceZ = *sw_ms_fTimeStep * PED_MASS(ped) * 0.3f * 0.008f;
        sw_ApplyMoveForce(ped, 0.0f, 0.0f, fMoveForceZ);
      }
      return;
    }
    default:
      return;
  }

  // NX tuning: trim non-sprint surface swim (TREAD/SPRINT) ~20% so the sprint
  // state (SWIM_SPRINTING) reads as a clear speed-up. Sprint/dive states untouched.
  if (TASK_STATE(task) <= SWIM_SPRINT)
    vel = sw_mul(vel, 0.8f);

  float step = powf(0.9f, *sw_ms_fTimeStep);
  vel = sw_mul(vel, (1.0f - step) * sw_StepInvMagic());
  PED_VEL(ped) = sw_mul(PED_VEL(ped), step);
  if (sw_IsPlayer(ped)) vel = sw_mul(vel, 1.25f);
  PED_VEL(ped) = sw_add(PED_VEL(ped), vel);

  SwVec pedPos = MAT_POS(PED_MAT(ped));
  int bUpdateRot = 1;
  SwVec chk = sw_add(pedPos, sw_mul(PED_VEL(ped), *sw_ms_fTimeStep));
  float water = 0.0f;
  if (!sw_GetWaterLevel(chk.x, chk.y, chk.z, &water, 1, NULL)) {
    fSubmergeZ = -1.0f;
    bUpdateRot = 0;
  } else if (TASK_STATE(task) != SWIM_UNDERWATER_SPRINTING || TASK_SCHG(task) < 0.0f) {
    bUpdateRot = 0;
  } else if (pedPos.z + 0.65f > water && TASK_ROTX(task) > 0.7854f) {
    TASK_STATE(task) = SWIM_TREAD;
    TASK_SCHG(task) = 0.0f;
    bUpdateRot = 0;
  }

  if (bUpdateRot) {
    if (TASK_ROTX(task) >= 0.0f) {
      if (pedPos.z + 0.65f <= water) {
        if (TASK_SCHG(task) <= 0.001f) TASK_SCHG(task) = 0.0f;
        else TASK_SCHG(task) *= 0.95f;
      } else {
        const float fMin = 0.05f * 0.5f;
        if (TASK_SCHG(task) > fMin) TASK_SCHG(task) *= 0.95f;
        if (TASK_SCHG(task) < fMin) {
          TASK_SCHG(task) += sw_StepSeconds() / 10.0f;
          TASK_SCHG(task) = sw_min(fMin, TASK_SCHG(task));
        }
        TASK_ROTX(task) += *sw_ms_fTimeStep * TASK_SCHG(task);
        fSubmergeZ = (0.55f - 0.2f) * (TASK_ROTX(task) * 4.0f / (float)M_PI) * 0.75f + 0.2f;
      }
    } else {
      if (pedPos.z - sinf(TASK_ROTX(task)) + 0.65f <= water) {
        if (TASK_SCHG(task) > 0.001f) TASK_SCHG(task) *= 0.95f;
        else TASK_SCHG(task) = 0.0f;
      } else {
        TASK_SCHG(task) += sw_StepSeconds() / 10.0f;
        TASK_SCHG(task) = sw_min(TASK_SCHG(task), 0.05f);
      }
      TASK_ROTX(task) += *sw_ms_fTimeStep * TASK_SCHG(task);
    }
  }

  if (fSubmergeZ > 0.0f) {
    water -= fSubmergeZ + pedPos.z;
    float mz = water / *sw_ms_fTimeStep;
    float ts = *sw_ms_fTimeStep * 0.1f;
    mz = sw_clamp(mz, -ts, ts);
    mz -= PED_VEL(ped).z;
    ts = sw_StepSeconds();
    mz = sw_clamp(mz, -ts, ts);
    PED_VEL(ped).z += mz;
  }

  if (pedPos.z < -69.0f)
    PED_VEL(ped).z = sw_max(PED_VEL(ped).z, 0.0f);
}


// Cheats -- restore PC-style cheat codes typed on the Switch on-screen keyboard.
static const uint32_t cheat_hash_keys[] = {
    0xDE4B237D, 0xB22A28D1, 0x5A783FAE,
    0x5A1B5E9A, 0x00000000, 0x00000000, 0x00000000, // WEAPON4,TIMETRAVEL,SCRIPTBYPASS,SHOWMAPPINGS
    0x7B64E263, 0x00000000, 0x00000000,             // INVINCIBILITY,SHOWTAPTOTARGET,SHOWTARGETING
    0xEECCEA2B, 0x42AF1E28, 0x555FC201, 0x2A845345, 0xE1EF01EA,
    0x771B83FC, 0x5BF12848, 0x44453A17, 0x00000000, 0xB69E8532,
    0x8B828076, 0xDD6ED9E9, 0xA290FD8C, 0x00000000, 0x43DB914E,
    0xDBC0DD65, 0x00000000, 0xD08A30FE, 0x37BF1B4E, 0xB5D40866,
    0xE63B0D99, 0x675B8945, 0x4987D5EE, 0x2E8F84E8, 0x00000000,
    0x00000000, 0x0D5C6A4E, 0x00000000, 0x00000000, 0x66516EBC,
    0x4B137E45, 0x00000000, 0x00000000, 0x3A577325, 0xD4966D59,
    0x00000000, 0x5FD1B49D, 0xA7613F99, 0x1792D871, 0xCBC579DF, // THEGAMBLER
    0x4FEDCCFF, 0x44B34866, 0x2EF877DB, 0x2781E797, 0x2BC1A045,
    0xB2AFE368, 0x00000000, 0x00000000, 0x1A5526BC, 0xA48A770B,
    0x00000000, 0x00000000, 0x00000000, 0x7F80B950, 0x6C0FA650,
    0xF46F2FA4, 0x70164385, 0x00000000, 0x885D0B50, 0x151BDCB3,
    0xADFA640A, 0xE57F96CE, 0x040CF761, 0xE1B33EB9, 0xFEDA77F7,
    0x00000000, 0x00000000, 0xF53EF5A5, 0xF2AA0C1D, 0xF36345A8,
    0x00000000, 0xB7013B1B, 0x00000000, 0x31F0C3CC, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xF01286E9,
    0xA841CC0A, 0x31EA09CF, 0xE958788A, 0x02C83A7C, 0xE49C3ED4,
    0x171BA8CC, 0x86988DAE, 0x2BDD2FA1, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    0x00000000,
};

static void (*CCheat__AddToCheatString)(char c);
static volatile int cheat_ready = 0;
static char cheat_text[64];

void cheats_enqueue(const char *s) {
  if (!s) return;
  strncpy(cheat_text, s, sizeof(cheat_text) - 1);
  cheat_text[sizeof(cheat_text) - 1] = 0;
  cheat_ready = 1;
}

static void CCheat__DoCheats(void) {
  if (!cheat_ready) return;
  for (int i = 0; cheat_text[i]; i++) {
    char c = cheat_text[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (CCheat__AddToCheatString) CCheat__AddToCheatString(c);
  }
  cheat_ready = 0;
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

  // Fix Second Siren
  if (so_try_find_addr_rx(&game_mod, "_ZN8CVehicle19ProcessSirenAndHornEb")) {
    uint32_t *fn =
        (uint32_t *)so_find_addr(&game_mod, "_ZN8CVehicle19ProcessSirenAndHornEb");
    fn[0xA0 / 4] = 0xD503201F; // was: tbnz w8, #7 (skip [veh+1700]=1)
    fn[0xD8 / 4] = 0xD503201F; // was: and x8, x8, #0xffff7fffffffffff (clear bit 47)
  }

  // Water physics fix
  if (so_try_find_addr_rx(&game_mod, "_ZN15CTaskSimpleSwim25ProcessSwimmingResistanceEP4CPed")) {
    sw_GetAnim = (void *)so_find_addr_rx(&game_mod, "_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
    sw_ApplyMoveForce = (void *)so_find_addr_rx(&game_mod, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    sw_IsPlayer = (void *)so_find_addr_rx(&game_mod, "_ZNK4CPed8IsPlayerEv");
    sw_GetWaterLevel = (void *)so_find_addr_rx(&game_mod, "_ZN11CWaterLevel13GetWaterLevelEfffPfbP7CVector");
    sw_ms_fTimeStep = (float *)so_find_addr_rx(&game_mod, "_ZN6CTimer12ms_fTimeStepE");
    hook_arm64(so_find_addr(&game_mod, "_ZN15CTaskSimpleSwim25ProcessSwimmingResistanceEP4CPed"),
               (uintptr_t)ProcessSwimmingResistance);
  }

  // Cheats: overwrite the hash-key table with the PC codes and replace DoCheats
  // with the queue pump (input from main.c's on-screen keyboard via cheats_enqueue).
  if (so_try_find_addr_rx(&game_mod, "_ZN6CCheat8DoCheatsEv")) {
    CCheat__AddToCheatString =
        (void *)so_find_addr_rx(&game_mod, "_ZN6CCheat16AddToCheatStringEc");
    memcpy((void *)so_find_addr(&game_mod, "_ZN6CCheat16m_aCheatHashKeysE"),
           cheat_hash_keys, sizeof(cheat_hash_keys));
    hook_arm64(so_find_addr(&game_mod, "_ZN6CCheat8DoCheatsEv"),
               (uintptr_t)CCheat__DoCheats);
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
