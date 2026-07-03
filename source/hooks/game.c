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
  // Clamp so the CCamera stub's 70/fake_fov can't go negative or blow up at
  // extreme zoom (sniper scope / camera at max zoom): fake_fov drops below zero
  // for a tiny fov, which flips the frustum and makes the whole world vanish.
  // (JPatch clamps the FOV to [1,170] for the same reason.)
  if (fake_fov < 1.0f) fake_fov = 1.0f;
  else if (fake_fov > 170.0f) fake_fov = 170.0f;
  return fake_fov;
}

// Half 2 of the FOV fix.
__attribute__((visibility("hidden"))) uintptr_t ccamera_fov_ret;
extern void CCamera__Process_fov_stub(void); // fov_stub.s

// Hydra camera fix
__attribute__((visibility("hidden"))) uintptr_t ccam_ymov_ret;
extern void CCam__FollowCar_ymov_stub(void); // ccam_ymov_stub.s

// Hydra manual nozzle control
extern volatile float g_right_stick_y; // main.c: right stick Y in [-1,1], up<0
__attribute__((visibility("hidden"))) uintptr_t cplane_nozzle_ret;
extern void CPlane__nozzle_stub(void); // cplane_nozzle_stub.s

// Rotate the Hydra thrust nozzles from the right-stick vertical axis.
void CPlane__nozzle_manual(void *self, int pad) {
  (void)pad;
  const float sy = g_right_stick_y;
  int16_t *noz = (int16_t *)((char *)self + 0xA88);
  int16_t *prev = (int16_t *)((char *)self + 0xA8A);
  int v = *noz;
  const int step = 80;      // per frame; ~1-2 s for the full 0..5000 sweep
  const float dz = 0.35f;   // stick deadzone (fraction of full deflection)
  if (sy < -dz)             // stick up -> open nozzles (VTOL/hover)
    v -= step;
  else if (sy > dz)         // stick down -> close nozzles (forward flight)
    v += step;
  else
    return; // inside deadzone: hold current angle
  if (v < 0) v = 0;
  else if (v > 5000) v = 5000;
  *prev = *noz;
  *noz = (int16_t)v;
}


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

// Controller remap. The engine maps game actions (HIDMapping) to buttons via
// CHIDJoystick::AddMapping(this, buttonId, action). Its subclass ctors build a
// default (Xbox-ish) layout with overlaps; we reimplement AddMapping so every
// action's buttonId is taken from a remap table (built-in PSV layout, overridable
// by an optional text file). buttonId space (engine ButtonID): CROSS=0 CIRCLE=1
// SQUARE=2 TRIANGLE=3 START=4 SELECT=5 L1=6 R1=7 DPAD_UP=8 DPAD_DOWN=9 DPAD_LEFT=10
// DPAD_RIGHT=11 L3=12 R3=13 L2=68 R2=69 (L2/R2 = analog triggers on Switch).
#define REMAP_UNSET (-2)
static int g_remap[256]; // [HIDMapping] -> buttonId, or REMAP_UNSET to keep default

static void controls_defaults(void) {
  for (int i = 0; i < 256; i++) g_remap[i] = REMAP_UNSET;
  // PSV layout (gtasa_vita/gamefiles/controls.txt), joystick-mapped actions only:
  g_remap[1]  = 1;   // ATTACK       -> CIRCLE
  g_remap[2]  = 0;   // SPRINT       -> CROSS
  g_remap[3]  = 2;   // JUMP         -> SQUARE
  g_remap[4]  = 12;  // CROUCH       -> L3
  g_remap[5]  = 3;   // ENTER_CAR    -> TRIANGLE
  g_remap[6]  = 68;  // BRAKE        -> L2 (ZL analog trigger)
  g_remap[7]  = 2;   // HANDBRAKE    -> SQUARE
  g_remap[8]  = 69;  // ACCELERATE   -> R2 (ZR analog trigger)
  g_remap[9]  = 5;   // CAMERA_CLOSER-> SELECT
  g_remap[10] = 5;   // CAMERA_FARTHER-> SELECT
  g_remap[11] = 12;  // HORN         -> L3
  g_remap[12] = 8;   // RADIO_PREV   -> DPAD_UP
  g_remap[13] = 9;   // RADIO_NEXT   -> DPAD_DOWN
  g_remap[14] = 8;   // VITAL_STATS  -> DPAD_UP
  g_remap[15] = 69;  // NEXT_WEAPON  -> R2
  g_remap[16] = 68;  // PREV_WEAPON  -> L2
  g_remap[17] = 4;   // RADAR        -> START
  g_remap[18] = 13;  // PED_LOOK_BACK-> R3
  g_remap[19] = 6;   // VEH_LOOK_LEFT-> L1 (off the ZL trigger so aim/R1 has no analog)
  g_remap[20] = 7;   // VEH_LOOK_RIGHT-> R1 (context with TARGETING: aim on foot, look in car)
  g_remap[22] = 3;   // MISSION      -> TRIANGLE
  g_remap[23] = 13;  // VIGILANTE    -> R3
  g_remap[33] = 6;   // SWAP_WEAPONS  -> L1
  g_remap[34] = 2;   // WEAPON_ZOOM_IN-> SQUARE
  g_remap[35] = 0;   // WEAPON_ZOOM_OUT-> CROSS
  g_remap[36] = 7;   // TARGETING(aim)-> R1
  g_remap[37] = 10;  // VEHICLE_BOMB  -> DPAD_LEFT
}

static const struct { const char *n; int v; } remap_actions[] = {
  {"MAPPING_ATTACK",1},{"MAPPING_SPRINT",2},{"MAPPING_JUMP",3},{"MAPPING_CROUCH",4},
  {"MAPPING_ENTER_CAR",5},{"MAPPING_BRAKE",6},{"MAPPING_HANDBRAKE",7},{"MAPPING_ACCELERATE",8},
  {"MAPPING_CAMERA_CLOSER",9},{"MAPPING_CAMERA_FARTHER",10},{"MAPPING_HORN",11},
  {"MAPPING_RADIO_PREV_STATION",12},{"MAPPING_RADIO_NEXT_STATION",13},{"MAPPING_VITAL_STATS",14},
  {"MAPPING_NEXT_WEAPON",15},{"MAPPING_PREV_WEAPON",16},{"MAPPING_RADAR",17},
  {"MAPPING_PED_LOOK_BACK",18},{"MAPPING_VEHICLE_LOOK_LEFT",19},{"MAPPING_VEHICLE_LOOK_RIGHT",20},
  {"MAPPING_MISSION_START_AND_CANCEL",22},{"MAPPING_MISSION_START_AND_CANCEL_VIGILANTE",23},
  {"MAPPING_SWAP_WEAPONS_AND_PURCHASE",33},{"MAPPING_WEAPON_ZOOM_IN",34},{"MAPPING_WEAPON_ZOOM_OUT",35},
  {"MAPPING_ENTER_AND_EXIT_TARGETING",36},{"MAPPING_VEHICLE_BOMB",37},
};
static const struct { const char *n; int v; } remap_buttons[] = {
  {"BUTTON_UNUSED",-1},{"BUTTON_CROSS",0},{"BUTTON_CIRCLE",1},{"BUTTON_SQUARE",2},
  {"BUTTON_TRIANGLE",3},{"BUTTON_START",4},{"BUTTON_SELECT",5},{"BUTTON_L1",6},{"BUTTON_R1",7},
  {"DPAD_UP",8},{"DPAD_DOWN",9},{"DPAD_LEFT",10},{"DPAD_RIGHT",11},{"BUTTON_L3",12},
  {"BUTTON_R3",13},{"BUTTON_L2",68},{"BUTTON_R2",69},
};

// Load overrides from a text file: "MAPPING_XXX BUTTON_YYY" or "MAPPING_XXX=BUTTON_YYY"
// per line (';'/'#' comments). Absent file -> built-in PSV defaults stand.
static void controls_load(const char *path) {
  controls_defaults();
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[256], a[64], b[64];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == ';' || line[0] == '#') continue;
    for (char *p = line; *p; p++) if (*p == '=') *p = ' ';
    a[0] = b[0] = 0;
    if (sscanf(line, "%63s %63s", a, b) != 2) continue;
    int mv = -1, bv = REMAP_UNSET;
    for (unsigned i = 0; i < sizeof(remap_actions)/sizeof(*remap_actions); i++)
      if (!strcmp(a, remap_actions[i].n)) { mv = remap_actions[i].v; break; }
    for (unsigned i = 0; i < sizeof(remap_buttons)/sizeof(*remap_buttons); i++)
      if (!strcmp(b, remap_buttons[i].n)) { bv = remap_buttons[i].v; break; }
    if (mv >= 0 && mv < 256 && bv != REMAP_UNSET) g_remap[mv] = bv;
  }
  fclose(f);
}

// Reimplements CHIDJoystick::AddMapping (arm64 layout: cap@+8, count@+12, array@+16;
// entry = 20 bytes {int buttonId; int action; 12 bytes analog cache}). Overrides the
// buttonId from g_remap so every subclass ctor gets our layout.
static void CHIDJoystick__AddMapping(void *self, int buttonId, int action) {
  if (action >= 0 && action < 256 && g_remap[action] != REMAP_UNSET)
    buttonId = g_remap[action];
  int cap = *(int *)((char *)self + 8);
  int cnt = *(int *)((char *)self + 12);
  char *arr = *(char **)((char *)self + 16);
  if (cnt + 1 > cap) {
    cap = cnt + 8;
    arr = realloc(arr, (size_t)cap * 20);
    if (!arr) return;
    *(int *)((char *)self + 8) = cap;
    *(char **)((char *)self + 16) = arr;
  }
  char *e = arr + (size_t)cnt * 20;
  *(int *)(e + 0) = buttonId;
  *(int *)(e + 4) = action;
  memset(e + 8, 0, 12);
  *(int *)((char *)self + 12) = cnt + 1;
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

  // Heli/plane camera fix
  {
    const uint32_t mov_x0_0 = 0xD2800000;
    if (so_try_find_addr_rx(&game_mod, "_ZN4CPad18AimWeaponLeftRightEP4CPedPb")) {
      uint32_t *fn = (uint32_t *)so_find_addr(
          &game_mod, "_ZN4CPad18AimWeaponLeftRightEP4CPedPb");
      fn[0x6c / 4] = mov_x0_0;
    }
    if (so_try_find_addr_rx(&game_mod, "_ZN4CPad15AimWeaponUpDownEP4CPedPb")) {
      uint32_t *fn = (uint32_t *)so_find_addr(
          &game_mod, "_ZN4CPad15AimWeaponUpDownEP4CPedPb");
      fn[0x60 / 4] = mov_x0_0;
    }
    if (so_try_find_addr_rx(&game_mod,
                            "_ZN4CCam20Process_FollowCar_SAERK7CVectorfffb")) {
      uint32_t *fn = (uint32_t *)so_find_addr(
          &game_mod, "_ZN4CCam20Process_FollowCar_SAERK7CVectorfffb");
      fn[0x62c / 4] = mov_x0_0;  // block @+0x62c
      fn[0x1138 / 4] = mov_x0_0; // block @+0x1138
      fn[0x156c / 4] = mov_x0_0; // block @+0x156c (skip the 4th @+0x1240)
    }
  }

  // Heli/plane camera fix
  if (so_try_find_addr_rx(&game_mod,
                          "_ZN4CCam20Process_FollowCar_SAERK7CVectorfffb")) {
    ccam_ymov_ret =
        so_find_addr_rx(&game_mod,
                        "_ZN4CCam20Process_FollowCar_SAERK7CVectorfffb") +
        0xF74;
    hook_arm64(so_find_addr(&game_mod,
                            "_ZN4CCam20Process_FollowCar_SAERK7CVectorfffb") +
                   0xF60,
               (uintptr_t)CCam__FollowCar_ymov_stub);
  }

  // Hydra manual nozzle control
  if (so_try_find_addr_rx(&game_mod, "_ZN6CPlane20ProcessControlInputsEh")) {
    uint32_t *pci =
        (uint32_t *)so_find_addr(&game_mod, "_ZN6CPlane20ProcessControlInputsEh");

    // Fully manual landing gear
    //   +0x4A0: `b.ne 0x6ccc50` (airborne, gear!=0 -> button) -> unconditional `b`,
    //           so gear-down airborne also needs the button (no auto-retract).
    pci[0x4A0 / 4] = 0x17FFFFFA; // 0x54FFFF41 (b.ne) -> b -0x18
    //   +0x484: `b.eq 0x6ccc6c` (near ground, gear==1.0 -> auto-deploy) -> NOP,
    //           so gear-up near-ground falls to the button path (no auto-deploy).
    pci[0x484 / 4] = 0xD503201F; // 0x54000100 (b.eq) -> NOP

    cplane_nozzle_ret =
        so_find_addr_rx(&game_mod, "_ZN6CPlane20ProcessControlInputsEh") + 0x784;
    hook_arm64(
        so_find_addr(&game_mod, "_ZN6CPlane20ProcessControlInputsEh") + 0x5D4,
        (uintptr_t)CPlane__nozzle_stub);
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

  // Controller remap: load the layout (built-in PSV default + optional controls.txt)
  // and replace CHIDJoystick::AddMapping so every joystick built at runtime uses it.
  controls_load("controls.txt");
  if (so_try_find_addr_rx(&game_mod, "_ZN12CHIDJoystick10AddMappingEi10HIDMapping"))
    hook_arm64(so_find_addr(&game_mod, "_ZN12CHIDJoystick10AddMappingEi10HIDMapping"),
               (uintptr_t)CHIDJoystick__AddMapping);

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
