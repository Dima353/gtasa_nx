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
  thread_registry_add();            // track for freeze-on-exit
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
//
// Pause-menu Start/Select fix (see mobilemenu_update_stub.s).
extern volatile int g_escape_pressed; // main.c ('+' edge)
extern volatile int g_select_pressed; // main.c ('-' edge)
static char *g_mobilemenu;            // &gMobileMenu (the instance)
static void (*g_menu_popall)(void *self);  // MobileMenu::PopAllScreens (resume)
extern void MobileMenu__Update_orig(void *self); // mobilemenu_update_stub.s
__attribute__((visibility("hidden"))) uintptr_t mm_update_cont;

static int menu_is_open(void) {
  return g_mobilemenu && *(int *)(g_mobilemenu + 36) != 0;
}

static int GetEscapeJustDown_hook(void) {
  if (menu_is_open())
    return 0; // '+' resumes (handled in the Update hook), never re-opens the pause
  if (g_escape_pressed) {
    g_escape_pressed = 0;
    return 1;
  }
  return 0;
}

void MobileMenu__Update_hook(void *self) {
  if (self && *(int *)((char *)self + 36) != 0) { // a menu screen is open
    if (g_escape_pressed) {                        // '+' -> resume
      g_escape_pressed = 0;
      if (g_menu_popall)
        g_menu_popall(self);
    } else if (g_select_pressed) {                 // '-' -> toggle the pause map
      g_select_pressed = 0;
      char *open = (char *)self + 141;
      *open = *open ? 0 : 1;
    }
  } else {
    g_select_pressed = 0; // ignore '-' outside the pause menu
  }
  MobileMenu__Update_orig(self);
}

// Hydraulics camera fix.
extern volatile int g_r3_down, g_l3_down, g_dpad_down; // main.c
__attribute__((visibility("hidden"))) int g_hydraulics_camera = 0; // 0=tilt 1=camera
__attribute__((visibility("hidden"))) uintptr_t hyd_freeze_ret, hyd_cont_ret;
__attribute__((visibility("hidden"))) uintptr_t aim_lr_early, aim_lr_cont;
__attribute__((visibility("hidden"))) uintptr_t aim_ud_early, aim_ud_cont;
__attribute__((visibility("hidden"))) uintptr_t chid_getinputtype;
extern void hyd_freeze_stub(void); // hydraulics_stub.s
extern void aim_lr_stub(void);
extern void aim_ud_stub(void);

static int CPad__GetHydraulicJump(void *this) {
  if (*(uint16_t *)((uint8_t *)this + 0x110) != 0)
    return 0;
  // R3 (without L3, so the L3+R3 cheat combo doesn't also flip it) toggles the mode.
  // Edge-detect here: HydraulicControl is the only caller, so this runs each frame
  // only while in a hydraulics car -- R3 elsewhere is untouched.
  static int prev = 0;
  int now = g_r3_down && !g_l3_down;
  if (now && !prev)
    g_hydraulics_camera = !g_hydraulics_camera;
  prev = now;
  return g_hydraulics_camera; // 1 -> HydraulicControl skips the tilt read
}

// Free-aim binding.
static int g_free_aim = 0;
__attribute__((visibility("hidden"))) uintptr_t faim_ret_shift, faim_ret_else;
__attribute__((visibility("hidden"))) uintptr_t findlock_cont;
extern void free_aim_stub(void); // free_aim_stub.s
extern void FindWeaponLockOnTarget_orig(void *this); // free_aim_stub.s trampoline
static void (*CPlayerPed__ClearWeaponTarget)(void *playerPed);
static void (*CEntity__CleanUpOldReference)(void *entity, void **ref);
static int  (*CGameLogic__IsCoopGameGoingOn)(void);
static int  (*CHID__GetInputType)(void);
static void *MobileSettings_settings;

// Called from free_aim_stub.s when a lock-on target exists but isn't being cycled.
__attribute__((visibility("hidden"))) void free_aim_maybe(void *playerPed) {
  static int prev = 0;
  int now = g_dpad_down;
  if (now && !prev) {
    g_free_aim = 1;
    CPlayerPed__ClearWeaponTarget(playerPed);
  }
  prev = now;
}

static int MobileSettings__IsFreeAimMode(void *this) {
  (void)this;
  if (g_free_aim)
    return 1;
  if (CHID__GetInputType() == 1) // stock: input type 1 -> not free aim
    return 0;
  if (*(int *)((uint8_t *)MobileSettings_settings + 256) != 1)
    return 0;
  return CGameLogic__IsCoopGameGoingOn() ? 0 : 1;
}

static int MobileSettings__IsLockOnMode(void *this) {
  (void)this;
  if (g_free_aim)
    return 0;
  if (CHID__GetInputType() == 1) // stock: input type 1 -> lock on
    return 1;
  if (*(int *)((uint8_t *)MobileSettings_settings + 256) == 0)
    return 1;
  return CGameLogic__IsCoopGameGoingOn();
}

// While free aim is active, don't let the engine re-acquire a lock target.
static void CPlayerPed__FindWeaponLockOnTarget(void *this) {
  if (g_free_aim)
    return;
  FindWeaponLockOnTarget_orig(this);
}

// Clears the free-aim latch when aiming ends, then does the stock cleanup (release
// the 3rd-person target reference at this+0x988).
static void CPlayerPed__Clear3rdPersonMouseTarget(void *this) {
  g_free_aim = 0;
  void **ref = (void **)((uint8_t *)this + 0x988);
  if (*ref != NULL) {
    CEntity__CleanUpOldReference(*ref, ref);
    *ref = NULL;
  }
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
  // Stop the OpenAL mixer while it is still healthy so alcCloseDevice cleanly joins
  // it (before __libnx_exit's __appExit tears down the audio service under it), then
  // write the pause-save, then hand off to hard_exit (freeze remaining workers +
  // return to the loader). Never returns.
  deinit_openal();
  SaveGameForPause(3, NULL);
  hard_exit();
  return 0;
}

// Add a "Quit" item to the pause menu.
static void (*FlowScreen__AddItem)(void *self, const char *tex, const char *label,
                                   void (*cb)(void));
__attribute__((visibility("hidden"))) uintptr_t addallitems_cont;
extern void MainMenuScreen__AddAllItems_orig(void *self); // mainmenu_exit_stub.s

static void menu_exit_cb(void) { MainMenuScreen__OnExit(); }

static void MainMenuScreen__AddAllItems(void *self) {
  MainMenuScreen__AddAllItems_orig(self); // original items
  FlowScreen__AddItem(self, "menu_mainquit", "FEP_QUI", menu_exit_cb);
}

// Emergency-vehicle / widescreen FOV fix.
static float *CDraw__ms_fFOV;
static float *CDraw__ms_fAspectRatio;
// hidden (not static): referenced by fov_stub.s; hidden keeps adrp/add PIC-valid
__attribute__((visibility("hidden"))) float fake_fov;

// Radar fix.
static uintptr_t g_radar_mpw_base;

static float CDraw__SetFOV(float fov) {
  if (g_radar_mpw_base) {
    void *w = *(void **)(g_radar_mpw_base + 1288);
    if (w) {
      // Radar layout from Adjustable.cfg (640x448 virtual space): centre (76,370),
      // half-extent (44,44). DrawRadar reads two opposite corners at widget+44
      // (d2 = x,y) and +52 (d3 = x,y) in screen pixels, deriving centre=(d2+d3)/2
      // and half-extent=|d2-d3|/2. Scale to the render resolution.
      const float sx = (float)screen_width / 640.0f;
      const float sy = (float)screen_height / 448.0f;
      const float cx = 76.0f * sx, cy = 370.0f * sy;
      const float hx = 44.0f * sx, hy = 44.0f * sy;
      float *r = (float *)((char *)w + 44);
      r[0] = cx - hx; // d2.x
      r[1] = cy - hy; // d2.y
      r[2] = cx + hx; // d3.x
      r[3] = cy + hy; // d3.y
    }
  }
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

// Radar alpha fix.
__attribute__((visibility("hidden"))) uintptr_t drawradar_cont;
extern void CHud__DrawRadar_stub(void); // chud_drawradar_stub.s
void radar_setup(void) {
  if (!g_radar_mpw_base)
    return;
  void *w = *(void **)(g_radar_mpw_base + 1288);
  if (w)
    *((unsigned char *)w + 88) = 255;
}

// Half 2 of the FOV fix.
__attribute__((visibility("hidden"))) uintptr_t ccamera_fov_ret;
extern void CCamera__Process_fov_stub(void); // fov_stub.s

// Taxi "for hire" roof light (JPatch TaxiLights), removed on mobile.
extern void CAutomobile__Render_orig(void *self); // taxi_light_stub.s
__attribute__((visibility("hidden"))) uintptr_t caut_render_cont;
static void (*g_set_taxi_light)(void *self, int on); // CAutomobile::SetTaxiLight(bool)
void CAutomobile__Render_hook(void *self) {
  CAutomobile__Render_orig(self);
  uint16_t model = *(uint16_t *)((char *)self + 0x32);
  if (model == 420 || model == 438) {
    void *driver = *(void **)((char *)self + 1440);
    unsigned char numPass = *(unsigned char *)((char *)self + 1512);
    float health = *(float *)((char *)self + 1588);
    g_set_taxi_light(self, (driver && numPass == 0 && health > 0.0f) ? 1 : 0);
  }
}

// Night traffic-light + taxi-light glow on Medium/Low visual FX (originally the JPatch
// "Fix traffic lights" intent).

// PS2-style corona rotation (see corona_ps2_stub.s). Ported from JPatch
__attribute__((visibility("hidden"))) uintptr_t corona_ps2_ret;
extern void CCoronas__Render_ps2corona_stub(void); // corona_ps2_stub.s

// PS2 color filter (see colorfilter_ps2_stub.s).
__attribute__((visibility("hidden"))) uintptr_t colorfilter_ret_fall; // +0x624
__attribute__((visibility("hidden"))) uintptr_t colorfilter_ret_ne;   // +0x6b0
extern void CPostEffects__MobileRender_ps2filter_stub(void); // colorfilter_ps2_stub.s

// Hydra camera fix.
__attribute__((visibility("hidden"))) uintptr_t ccam_ymov_ret;
extern void CCam__FollowCar_ymov_stub(void); // ccam_ymov_stub.s

// Hydra manual nozzle control.
extern volatile float g_right_stick_y; // main.c: right stick Y in [-1,1], up<0
__attribute__((visibility("hidden"))) uintptr_t cplane_nozzle_ret;
extern void CPlane__nozzle_stub(void); // cplane_nozzle_stub.s

// Hydra/plane yaw on L1/R1.
__attribute__((visibility("hidden"))) uintptr_t cplane_rudder_ret;
extern void CPlane__rudder_turret_stub(void); // cplane_rudder_stub.s
extern volatile int g_l1_down, g_r1_down;     // main.c: physical L1/R1 held
int CPlane__rudder_turret_input(void) {
  if (g_r1_down)
    return 128; // R1 -> yaw right
  if (g_l1_down)
    return -128; // L1 -> yaw left
  return 0;
}

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
  g_remap[38] = 6;   // TURRET_LEFT (yaw left)   ZL(68) -> L1(6)
  g_remap[39] = 7;   // TURRET_RIGHT (yaw right) ZR(69) -> R1(7)
  g_remap[95] = 0;   // vehicle gun fire         L1(6)  -> CROSS(0)
  g_remap[88] = 0;   // aircraft flare/2nd wpn   R1(7)  -> CROSS(0)
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
  {"MAPPING_TURRET_LEFT",38},{"MAPPING_TURRET_RIGHT",39},
  {"MAPPING_VEHICLE_GUN_FIRE",95},{"MAPPING_AIRCRAFT_FLARE",88},
};
static const struct { const char *n; int v; } remap_buttons[] = {
  {"BUTTON_UNUSED",-1},{"BUTTON_CROSS",0},{"BUTTON_CIRCLE",1},{"BUTTON_SQUARE",2},
  {"BUTTON_TRIANGLE",3},{"BUTTON_START",4},{"BUTTON_SELECT",5},{"BUTTON_L1",6},{"BUTTON_R1",7},
  {"DPAD_UP",8},{"DPAD_DOWN",9},{"DPAD_LEFT",10},{"DPAD_RIGHT",11},{"BUTTON_L3",12},
  {"BUTTON_R3",13},{"BUTTON_L2",68},{"BUTTON_R2",69},
};

// Write the current g_remap (built-in PSV defaults) out as an editable controls.txt,
// in the exact format controls_load parses, so users get a self-documenting template.
static void controls_write(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs("# GTA:SA NX controller remap -- auto-generated defaults.\n"
        "# Format: MAPPING_ACTION BUTTON_NAME  (one per line; ';' or '#' = comment).\n"
        "# Buttons: BUTTON_CROSS CIRCLE SQUARE TRIANGLE START SELECT L1 R1 L3 R3 L2 R2,\n"
        "#          DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT, BUTTON_UNUSED.\n"
        "# Edit a line to remap; delete it to keep the built-in default.\n\n",
        f);
  for (unsigned i = 0; i < sizeof(remap_actions) / sizeof(*remap_actions); i++) {
    int bv = g_remap[remap_actions[i].v];
    if (bv == REMAP_UNSET)
      continue;
    const char *bn = NULL;
    for (unsigned j = 0; j < sizeof(remap_buttons) / sizeof(*remap_buttons); j++)
      if (remap_buttons[j].v == bv) { bn = remap_buttons[j].n; break; }
    if (bn)
      fprintf(f, "%-40s %s\n", remap_actions[i].n, bn);
  }
  fclose(f);
}

// Load overrides from a text file: "MAPPING_XXX BUTTON_YYY" or "MAPPING_XXX=BUTTON_YYY"
// per line (';'/'#' comments). Absent file -> write out the built-in PSV defaults as
// an editable template, then use those defaults.
static void controls_load(const char *path) {
  controls_defaults();
  FILE *f = fopen(path, "r");
  if (!f) {
    controls_write(path);
    return;
  }
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

// Return the fog.
static float *g_emu_fogdistances;  // [min, max, 1/(max-min)]
static float *g_emu_fogcolor;      // [r, g, b]
static uint8_t *g_emu_fogdirty;
static void emu_DistanceFogSetup_fogwall(float minD, float maxD, float r, float g,
                                         float b) {
  minD *= 0.8f;
  maxD *= 0.95f;
  if (g_emu_fogdistances[0] != minD || g_emu_fogdistances[1] != maxD) {
    g_emu_fogdistances[0] = minD;
    g_emu_fogdistances[1] = maxD;
    g_emu_fogdistances[2] = 1.0f / (maxD - minD);
    *g_emu_fogdirty = 1;
  }
  if (g_emu_fogcolor[0] != r || g_emu_fogcolor[1] != g || g_emu_fogcolor[2] != b) {
    g_emu_fogcolor[0] = r;
    g_emu_fogcolor[1] = g;
    g_emu_fogcolor[2] = b;
    *g_emu_fogdirty = 1;
  }
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

  // Pause-menu Start/Select fix: hook MobileMenu::Update to route '+'->resume and
  // '-'->map when a menu is open (see MobileMenu__Update_hook / GetEscapeJustDown_hook).
  if (so_try_find_addr_rx(&game_mod, "_ZN10MobileMenu6UpdateEv") &&
      so_try_find_addr_rx(&game_mod, "gMobileMenu")) {
    g_mobilemenu = (char *)so_find_addr_rx(&game_mod, "gMobileMenu");
    g_menu_popall = (void (*)(void *))so_find_addr_rx(
        &game_mod, "_ZN10MobileMenu13PopAllScreensEv");
    mm_update_cont = so_find_addr_rx(&game_mod, "_ZN10MobileMenu6UpdateEv") + 16;
    hook_arm64(so_find_addr(&game_mod, "_ZN10MobileMenu6UpdateEv"),
               (uintptr_t)MobileMenu__Update_hook);
  }

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

  // Add a "Quit" item to the pause menu.
  if (so_try_find_addr_rx(&game_mod, "_ZN14MainMenuScreen11AddAllItemsEv") &&
      so_try_find_addr_rx(&game_mod, "_ZN10FlowScreen7AddItemEPKcS1_PFvvE")) {
    FlowScreen__AddItem =
        (void *)so_find_addr_rx(&game_mod, "_ZN10FlowScreen7AddItemEPKcS1_PFvvE");
    addallitems_cont =
        so_find_addr_rx(&game_mod, "_ZN14MainMenuScreen11AddAllItemsEv") + 16;
    hook_arm64(so_find_addr(&game_mod, "_ZN14MainMenuScreen11AddAllItemsEv"),
               (uintptr_t)MainMenuScreen__AddAllItems);
  }

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

  // Fix Second Siren.
  if (so_try_find_addr_rx(&game_mod, "_ZN8CVehicle19ProcessSirenAndHornEb")) {
    uint32_t *fn =
        (uint32_t *)so_find_addr(&game_mod, "_ZN8CVehicle19ProcessSirenAndHornEb");
    fn[0xA0 / 4] = 0xD503201F; // was: tbnz w8, #7 (skip [veh+1700]=1)
    fn[0xD8 / 4] = 0xD503201F; // was: and x8, x8, #0xffff7fffffffffff (clear bit 47)
  }

  // Heli/plane camera fix.
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

  // Hydraulics camera fix.
  if (so_try_find_addr_rx(&game_mod, "_ZN4CPad16GetHydraulicJumpEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN4CPad16GetHydraulicJumpEv"),
               (uintptr_t)CPad__GetHydraulicJump);
  chid_getinputtype = so_try_find_addr_rx(&game_mod, "_ZN4CHID12GetInputTypeEv");
  if (so_try_find_addr_rx(&game_mod, "_ZN11CAutomobile16HydraulicControlEv")) {
    uintptr_t rx = so_find_addr_rx(&game_mod, "_ZN11CAutomobile16HydraulicControlEv");
    hyd_freeze_ret = rx + 0xb50; // 0x6a2adc, the "not a hydraulics car" return path
    hyd_cont_ret = rx + 0x4dc;   // 0x6a2468 (hook +16), rejoin the normal apply
    hook_arm64(so_find_addr(&game_mod, "_ZN11CAutomobile16HydraulicControlEv") + 0x4cc,
               (uintptr_t)hyd_freeze_stub);
  }
  if (so_try_find_addr_rx(&game_mod, "_ZN4CPad18AimWeaponLeftRightEP4CPedPb")) {
    uintptr_t rx = so_find_addr_rx(&game_mod, "_ZN4CPad18AimWeaponLeftRightEP4CPedPb");
    uint32_t *fn = (uint32_t *)so_find_addr(&game_mod,
                                            "_ZN4CPad18AimWeaponLeftRightEP4CPedPb");
    aim_lr_early = rx + 0x30;
    aim_lr_cont = rx + 0xb0;
    fn[0x8c / 4] = 0xD503201F; // NOP `b.ne +0xa8` (would jump into the trampoline)
    fn[0x9c / 4] = 0xD503201F; // NOP `cbz x0, +0xa8`
    hook_arm64((uintptr_t)fn + 0xa0, (uintptr_t)aim_lr_stub);
  }
  if (so_try_find_addr_rx(&game_mod, "_ZN4CPad15AimWeaponUpDownEP4CPedPb")) {
    uintptr_t rx = so_find_addr_rx(&game_mod, "_ZN4CPad15AimWeaponUpDownEP4CPedPb");
    uint32_t *fn = (uint32_t *)so_find_addr(&game_mod,
                                            "_ZN4CPad15AimWeaponUpDownEP4CPedPb");
    aim_ud_early = rx + 0x28;
    aim_ud_cont = rx + 0xa4;
    fn[0x80 / 4] = 0xD503201F; // NOP `b.ne +0x9c` (would jump into the trampoline)
    fn[0x90 / 4] = 0xD503201F; // NOP `cbz x0, +0x9c`
    hook_arm64((uintptr_t)fn + 0x94, (uintptr_t)aim_ud_stub);
  }

  // Free-aim binding.
  CPlayerPed__ClearWeaponTarget =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN10CPlayerPed17ClearWeaponTargetEv");
  CEntity__CleanUpOldReference =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN7CEntity19CleanUpOldReferenceEPPS_");
  CGameLogic__IsCoopGameGoingOn =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN10CGameLogic17IsCoopGameGoingOnEv");
  CHID__GetInputType = (void *)chid_getinputtype; // resolved above
  MobileSettings_settings =
      (void *)so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings8settingsE");
  if (so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings13IsFreeAimModeEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14MobileSettings13IsFreeAimModeEv"),
               (uintptr_t)MobileSettings__IsFreeAimMode);
  if (so_try_find_addr_rx(&game_mod, "_ZN14MobileSettings12IsLockOnModeEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14MobileSettings12IsLockOnModeEv"),
               (uintptr_t)MobileSettings__IsLockOnMode);
  if (so_try_find_addr_rx(&game_mod, "_ZN10CPlayerPed22FindWeaponLockOnTargetEv")) {
    findlock_cont =
        so_find_addr_rx(&game_mod, "_ZN10CPlayerPed22FindWeaponLockOnTargetEv") + 0x10;
    hook_arm64(so_find_addr(&game_mod, "_ZN10CPlayerPed22FindWeaponLockOnTargetEv"),
               (uintptr_t)CPlayerPed__FindWeaponLockOnTarget);
  }
  if (so_try_find_addr_rx(&game_mod, "_ZN10CPlayerPed25Clear3rdPersonMouseTargetEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN10CPlayerPed25Clear3rdPersonMouseTargetEv"),
               (uintptr_t)CPlayerPed__Clear3rdPersonMouseTarget);
  if (so_try_find_addr_rx(&game_mod,
                          "_ZN23CTaskSimplePlayerOnFoot19ProcessPlayerWeaponEP10CPlayerPed")) {
    uintptr_t rx = so_find_addr_rx(
        &game_mod, "_ZN23CTaskSimplePlayerOnFoot19ProcessPlayerWeaponEP10CPlayerPed");
    faim_ret_shift = rx + 0x1588; // 0x68a7ac (bl FindNextWeaponLockOnTarget)
    faim_ret_else = rx + 0x158c;  // 0x68a7b0 (the fall-through path)
    hook_arm64(so_find_addr(
                   &game_mod,
                   "_ZN23CTaskSimplePlayerOnFoot19ProcessPlayerWeaponEP10CPlayerPed") +
                   0x14ac,
               (uintptr_t)free_aim_stub);
  }

  // Heli/plane camera fix.
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

  // Hydra manual nozzle control.
  if (so_try_find_addr_rx(&game_mod, "_ZN6CPlane20ProcessControlInputsEh")) {
    uint32_t *pci =
        (uint32_t *)so_find_addr(&game_mod, "_ZN6CPlane20ProcessControlInputsEh");

    // Fully manual landing gear: only the R3 toggle.
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

    // Yaw on L1/R1: redirect the plane rudder's stick read (+0x220) to the physical
    // L1/R1 buttons (g_l1_down/g_r1_down). Rejoins at +0x230. Stick keeps roll.
    cplane_rudder_ret =
        so_find_addr_rx(&game_mod, "_ZN6CPlane20ProcessControlInputsEh") + 0x230;
    hook_arm64(
        so_find_addr(&game_mod, "_ZN6CPlane20ProcessControlInputsEh") + 0x220,
        (uintptr_t)CPlane__rudder_turret_stub);
  }

  // Water physics fix.
  if (so_try_find_addr_rx(&game_mod, "_ZN15CTaskSimpleSwim25ProcessSwimmingResistanceEP4CPed")) {
    sw_GetAnim = (void *)so_find_addr_rx(&game_mod, "_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
    sw_ApplyMoveForce = (void *)so_find_addr_rx(&game_mod, "_ZN9CPhysical14ApplyMoveForceE7CVector");
    sw_IsPlayer = (void *)so_find_addr_rx(&game_mod, "_ZNK4CPed8IsPlayerEv");
    sw_GetWaterLevel = (void *)so_find_addr_rx(&game_mod, "_ZN11CWaterLevel13GetWaterLevelEfffPfbP7CVector");
    sw_ms_fTimeStep = (float *)so_find_addr_rx(&game_mod, "_ZN6CTimer12ms_fTimeStepE");
    hook_arm64(so_find_addr(&game_mod, "_ZN15CTaskSimpleSwim25ProcessSwimmingResistanceEP4CPed"),
               (uintptr_t)ProcessSwimmingResistance);
  }

  // Cheats.
  if (so_try_find_addr_rx(&game_mod, "_ZN6CCheat8DoCheatsEv")) {
    CCheat__AddToCheatString =
        (void *)so_find_addr_rx(&game_mod, "_ZN6CCheat16AddToCheatStringEc");
    memcpy((void *)so_find_addr(&game_mod, "_ZN6CCheat16m_aCheatHashKeysE"),
           cheat_hash_keys, sizeof(cheat_hash_keys));
    hook_arm64(so_find_addr(&game_mod, "_ZN6CCheat8DoCheatsEv"),
               (uintptr_t)CCheat__DoCheats);
  }

  // Controller remap.
  controls_load("controls.txt");
  if (so_try_find_addr_rx(&game_mod, "_ZN12CHIDJoystick10AddMappingEi10HIDMapping"))
    hook_arm64(so_find_addr(&game_mod, "_ZN12CHIDJoystick10AddMappingEi10HIDMapping"),
               (uintptr_t)CHIDJoystick__AddMapping);

  // Emergency-vehicle / widescreen FOV fix.
  if (so_try_find_addr_rx(&game_mod, "_ZN5CDraw6SetFOVEfb")) {
    CDraw__ms_fFOV = (float *)so_find_addr_rx(&game_mod, "_ZN5CDraw7ms_fFOVE");
    CDraw__ms_fAspectRatio =
        (float *)so_find_addr_rx(&game_mod, "_ZN5CDraw15ms_fAspectRatioE");
    hook_arm64(so_find_addr(&game_mod, "_ZN5CDraw6SetFOVEfb"),
               (uintptr_t)CDraw__SetFOV);

    // Radar fix.
    if (so_try_find_addr_rx(&game_mod, "_ZN15CTouchInterface10m_pWidgetsE"))
      g_radar_mpw_base =
          so_find_addr_rx(&game_mod, "_ZN15CTouchInterface10m_pWidgetsE");

    // Radar alpha fix.
    if (so_try_find_addr_rx(&game_mod, "_ZN4CHud9DrawRadarEv")) {
      drawradar_cont = so_find_addr_rx(&game_mod, "_ZN4CHud9DrawRadarEv") + 16;
      hook_arm64(so_find_addr(&game_mod, "_ZN4CHud9DrawRadarEv"),
                 (uintptr_t)CHud__DrawRadar_stub);
    }

    // half 2
    if (so_try_find_addr_rx(&game_mod, "_ZN7CCamera7ProcessEv")) {
      ccamera_fov_ret =
          so_find_addr_rx(&game_mod, "_ZN7CCamera7ProcessEv") + 0xDC8;
      hook_arm64(so_find_addr(&game_mod, "_ZN7CCamera7ProcessEv") + 0xDB8,
                 (uintptr_t)CCamera__Process_fov_stub);
    }
  }

  // PS2-style corona rotation (JPatch PS2CoronaRotation).
  if (config.ps2_corona_rotation &&
      so_try_find_addr_rx(&game_mod, "_ZN8CCoronas6RenderEv")) {
    corona_ps2_ret = so_find_addr_rx(&game_mod, "_ZN8CCoronas6RenderEv") + 0x414;
    hook_arm64(so_find_addr(&game_mod, "_ZN8CCoronas6RenderEv") + 0x404,
               (uintptr_t)CCoronas__Render_ps2corona_stub);
  }

  if (config.ps2_color_filter &&
      so_try_find_addr_rx(&game_mod, "_ZN12CPostEffects12MobileRenderEv")) {
    uintptr_t mr = so_find_addr_rx(&game_mod, "_ZN12CPostEffects12MobileRenderEv");
    colorfilter_ret_fall = mr + 0x624;
    colorfilter_ret_ne = mr + 0x6b0;
    hook_arm64(so_find_addr(&game_mod, "_ZN12CPostEffects12MobileRenderEv") + 0x614,
               (uintptr_t)CPostEffects__MobileRender_ps2filter_stub);
  }

  // Stop Android file/billing update callbacks
  if (so_try_find_addr_rx(&game_mod, "_Z14AND_FileUpdated"))
    hook_arm64(so_find_addr(&game_mod, "_Z14AND_FileUpdated"), (uintptr_t)ret0);
  if (so_try_find_addr_rx(&game_mod, "_Z17AND_BillingUpdateb"))
    hook_arm64(so_find_addr(&game_mod, "_Z17AND_BillingUpdateb"), (uintptr_t)ret0);

  // Sprinting on any surface is allowed.
  if (config.sprint_any_surface &&
      so_try_find_addr_rx(&game_mod, "_ZN14SurfaceInfos_c12CantSprintOnEj"))
    hook_arm64(so_find_addr(&game_mod, "_ZN14SurfaceInfos_c12CantSprintOnEj"),
               (uintptr_t)ret0);

  // Remove the "ExtraAirResistance" flag.
  if (config.remove_air_resistance &&
      so_try_find_addr_rx(&game_mod, "_ZN10CCullZones29DoExtraAirResistanceForPlayerEv"))
    hook_arm64(so_find_addr(&game_mod, "_ZN10CCullZones29DoExtraAirResistanceForPlayerEv"),
               (uintptr_t)ret0);

  // Aim the Country Rifle in 3rd person instead of the scope.
  if (so_try_find_addr_rx(&game_mod,
                          "_ZN23CTaskSimplePlayerOnFoot19ProcessPlayerWeaponEP10CPlayerPed")) {
    uint32_t *ppw = (uint32_t *)so_find_addr(
        &game_mod,
        "_ZN23CTaskSimplePlayerOnFoot19ProcessPlayerWeaponEP10CPlayerPed");
    ppw[0x194 / 4] = 0xD503201Fu; // NOP b.eq -> scope (site 1)
    ppw[0x784 / 4] = 0xD503201Fu; // NOP b.eq -> scope (site 2)
  }

  // CJ magnets to stealable objects only when very close.
  if (so_try_find_addr_rx(&game_mod, "_ZN11CPlayerInfo17FindObjectToStealEP4CPed")) {
    uint32_t *fots = (uint32_t *)so_find_addr(
        &game_mod, "_ZN11CPlayerInfo17FindObjectToStealEP4CPed");
    fots[0x15c / 4] = 0x1e2e1001u; // fmov s1, #1.0 (was #2.25)
  }

  // Cars and peds don't despawn when you look away (re3 ExtOffscreenDespRange).
  if (config.no_offscreen_despawn &&
      so_try_find_addr_rx(&game_mod, "_ZN8CCarCtrl21PossiblyRemoveVehicleEP8CVehicle")) {
    uint32_t *prv = (uint32_t *)so_find_addr(
        &game_mod, "_ZN8CCarCtrl21PossiblyRemoveVehicleEP8CVehicle");
    prv[0x10c / 4] = 0x1400001Bu; // +0x10c: tbnz w0,#0,+0x178 -> b +0x178 (skip off-screen block)
  }
  if (config.no_offscreen_despawn &&
      so_try_find_addr_rx(&game_mod, "_ZN11CPopulation9ManagePedEP4CPedRK7CVector")) {
    uint32_t *mp = (uint32_t *)so_find_addr(
        &game_mod, "_ZN11CPopulation9ManagePedEP4CPedRK7CVector");
    mp[0x1ec / 4] = 0x14000026u; // +0x1ec: b.le +0x284 -> b +0x284 (always keep)
  }

  // Fix near-clipping issues (JPatch FixNearClippingIssues, "WarDrum's dirty hands").
  if (so_try_find_addr_rx(&game_mod, "_Z11RenderSceneb")) {
    uint32_t *rs = (uint32_t *)so_find_addr(&game_mod, "_Z11RenderSceneb");
    rs[0x68 / 4] = 0x1E204120u; // fadd s0,s9,s10 -> fmov s0, s9
  }

  // Taxi "for hire" light: post-hook CAutomobile::Render.
  if (so_try_find_addr_rx(&game_mod, "_ZN11CAutomobile6RenderEv") &&
      so_try_find_addr_rx(&game_mod, "_ZN11CAutomobile12SetTaxiLightEb")) {
    g_set_taxi_light = (void (*)(void *, int))so_find_addr_rx(
        &game_mod, "_ZN11CAutomobile12SetTaxiLightEb");
    caut_render_cont =
        so_find_addr_rx(&game_mod, "_ZN11CAutomobile6RenderEv") + 16;
    hook_arm64(so_find_addr(&game_mod, "_ZN11CAutomobile6RenderEv"),
               (uintptr_t)CAutomobile__Render_hook);
  }

  // Night traffic-light + Taxi(420) light glow on Medium/Low visual FX.
  if (so_try_find_addr_rx(&game_mod,
                          "_ZN14CTrafficLights18DisplayActualLightEP7CEntity")) {
    uint32_t *fn = (uint32_t *)so_find_addr(
        &game_mod, "_ZN14CTrafficLights18DisplayActualLightEP7CEntity");
    fn[0x268 / 4] = 0xD503201F; // b.lt (+0x268) -> nop: corona on all FX levels
  }
  if (so_try_find_addr_rx(&game_mod, "_ZN11CAutomobile9PreRenderEv")) {
    uint32_t *fn =
        (uint32_t *)so_find_addr(&game_mod, "_ZN11CAutomobile9PreRenderEv");
    fn[0x864 / 4] = 0xD503201F; // b.lt (+0x864) -> nop: Taxi roof light on all FX
  }

  // Fix underwater airbubbles coming from CJ's jaw instead of his mouth (JPatch
  // AirBubblesFromJaw).
  if (so_try_find_addr_rx(&game_mod, "_ZN15CTaskSimpleSwim14ProcessEffectsEP4CPed")) {
    uint32_t *pe = (uint32_t *)so_find_addr(
        &game_mod, "_ZN15CTaskSimpleSwim14ProcessEffectsEP4CPed");
    pe[0x5a8 / 4] = 0xB9423808u; // ldr w8,[x0,#248] -> ldr w8,[x0,#568]  (z)
    pe[0x5ac / 4] = 0xF9411809u; // ldr x9,[x0,#240] -> ldr x9,[x0,#560]  (x,y)
  }

  // Always draw the wanted-level stars (config-gated, default off).
  if (config.show_wanted_stars &&
      so_try_find_addr_rx(&game_mod, "_ZN17CWidgetPlayerInfo10DrawWantedEv")) {
    uint32_t *dw = (uint32_t *)so_find_addr(
        &game_mod, "_ZN17CWidgetPlayerInfo10DrawWantedEv");
    dw[0x58 / 4] = 0x14000008u; // b 0x351628 (skip the "should we draw?" check)
  }

  // Return the fog.
  if (so_try_find_addr_rx(&game_mod, "_Z20emu_DistanceFogSetupfffff")) {
    g_emu_fogdistances = (float *)so_find_addr_rx(&game_mod, "emu_fogdistances");
    g_emu_fogcolor = (float *)so_find_addr_rx(&game_mod, "emu_fogcolor");
    g_emu_fogdirty = (uint8_t *)so_find_addr_rx(&game_mod, "emu_fogdirty");
    hook_arm64(so_find_addr(&game_mod, "_Z20emu_DistanceFogSetupfffff"),
               (uintptr_t)emu_DistanceFogSetup_fogwall);
  }

  // Remove the specular "shine" WarDrum added to peds/characters.
  if (config.disable_ped_spec &&
      so_try_find_addr_rx(&game_mod, "_Z16BuildPixelSourcej")) {
    uint32_t *bps = (uint32_t *)so_find_addr(&game_mod, "_Z16BuildPixelSourcej");
    bps[0x244 / 4] = 0x2a1603e8u; // mov w8, w22  (was: add w8, w22, #0x180)
  }

  // No adjustable
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
