/* main.c
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * Drives libGame.so's exported GameNative_impl* entry points through a fake JNI
 * environment, replacing the Android GLSurfaceView + GameActivity. The engine
 * renders on its own RenderThread; implOnDrawFrame runs game logic on the main
 * thread and only queues render commands, so the main thread needs no GL context.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <GLES2/gl2.h>

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

so_module donor_mod; // libc++_shared.so, the C++ runtime donor (see config.h)
so_module game_mod;  // libGame.so

// memory-split sizes (MB), filled in by __libnx_initheap, printed from main().
size_t g_mem_total_mb = 0, g_mem_newlib_mb = 0, g_mem_so_mb = 0;

// All GPU bos come from this nv transfer-memory region (carved from the newlib
// heap); the libnx default (~256MB) OOMs during SA's world-load textures.
u32 __nx_nv_transfermem_size = 0x60000000; // 1.5 GB GPU memory pool

volatile int g_hide_saves = 0;         // libc_shim.c: New Game hides save slots
int g_hide_saves_frames = 0;           // auto-clear countdown

// '+' opens/closes the pause menu (no gamepad-mapped pause otherwise); consumed
// by the CPad::GetEscapeJustDown hook in hooks/game.c.
volatile int g_escape_pressed = 0;

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

  // The newlib heap backs both the game's malloc and mesa's GPU bos, so give it
  // the bulk of RAM; the .so region only holds the mapped libraries.
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t so_region = (size_t)MEMORY_SO_MB * 1024 * 1024;
  if (so_region > size / 4)            // never grab more than 25% from newlib
    so_region = size / 4;
  fake_heap_size  = size - so_region;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000); // align to page size
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_mem_total_mb  = size >> 20;
  g_mem_newlib_mb = fake_heap_size >> 20;
  g_mem_so_mb     = so_region >> 20;
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (stat(CXX_DONOR_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nExtract it from the arm64-v8a APK.", CXX_DONOR_SO_NAME);
  // warn (don't abort) if none of the usual top-level data dirs are present
  if (stat("data", &st) < 0 && stat("texdb", &st) < 0 &&
      stat("models", &st) < 0 && stat("audio", &st) < 0)
    debugPrintf("WARNING: no data/ texdb/ models/ audio/ dir found -- "
                "did you extract the game data to the gtasa folder?\n");
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
// input: fed through the GameNative gamepad/touch entry points.
//
// implOnGamepadButtonDown/Up take the engine's ButtonID (CHIDJoystick numbering),
// NOT a raw 0-15 keycode. Correct IDs (from the mobile engine / gtasa_vita config.h,
// verified against CHIDJoystickXbox360Standard's default map):
//   CROSS=0 CIRCLE=1 SQUARE=2 TRIANGLE=3 START=4 SELECT=5 L1=6 R1=7
//   DPAD_UP=8 DPAD_DOWN=9 DPAD_LEFT=10 DPAD_RIGHT=11 L3=12 R3=13; L2=68 R2=69.
// L2/R2 (ZL/ZR) are ANALOG triggers reported via implOnGamepadAxesChanged (lt/rt),
// so they are not in this digital table. Plus is reserved for the pause menu.
// (The old table used a different numbering, which is why the d-pad/crouch were
// mismapped.)
// ---------------------------------------------------------------------------

typedef struct {
  u64 hid;
  int button;
} PadMap;

static const PadMap pad_map[] = {
  { HidNpadButton_B,       0 },  // CROSS  (bottom face / confirm)
  { HidNpadButton_A,       1 },  // CIRCLE (right face)
  { HidNpadButton_Y,       2 },  // SQUARE (left face)
  { HidNpadButton_X,       3 },  // TRIANGLE (top face)
  { HidNpadButton_Minus,   5 },  // SELECT
  { HidNpadButton_L,       6 },  // L1
  { HidNpadButton_R,       7 },  // R1
  { HidNpadButton_Up,      8 },  // DPAD_UP
  { HidNpadButton_Down,    9 },  // DPAD_DOWN
  { HidNpadButton_Left,   10 },  // DPAD_LEFT
  { HidNpadButton_Right,  11 },  // DPAD_RIGHT
  { HidNpadButton_StickL, 12 },  // L3
  { HidNpadButton_StickR, 13 },  // R3
};

// GameNative entry points of libGame.so (Java_com_rockstargames_oswrapper_*).

// setup / lifecycle
static void (* implOnActivityCreated)(void *env, void *thiz, void *activity);
static void (* implOnActivityDestroyed)(void *env, void *thiz);
static void (* implOnInitialSetup)(void *env, void *thiz, void *activity, void *apk, void *names, void *paths);
static void (* implOnSurfaceCreated)(void *env, void *thiz);
static void (* implOnSurfaceChanged)(void *env, void *thiz, void *surface, int w, int h);
static void (* implOnSurfaceDestroyed)(void *env, void *thiz);
static void (* implOnDrawFrame)(void *env, void *thiz, float dt);
static void (* implOnResume)(void *env, void *thiz);
static void (* implOnPause)(void *env, void *thiz);
static int  (* implIsInitialized)(void *env, void *thiz);

// input
static void (* implOnTouchStart)(void *env, void *thiz, int id, float x, float y);
static void (* implOnTouchMove)(void *env, void *thiz, int id, float x, float y);
static void (* implOnTouchEnd)(void *env, void *thiz, int id, float x, float y);
static void (* implOnGamepadConnected)(void *env, void *thiz, int pad);
static void (* implOnGamepadButtonDown)(void *env, void *thiz, int pad, int keycode);
static void (* implOnGamepadButtonUp)(void *env, void *thiz, int pad, int keycode);
static void (* implOnGamepadAxesChanged)(void *env, void *thiz, int pad,
                                         float lx, float ly, float rx, float ry, float lt, float rt);
static void (* implOnBackButtonPressed)(void *env, void *thiz);

// async completion callbacks the engine blocks on during boot: jni_fake queues
// them and the main loop drives these entry points (see dispatch_jni_callbacks).
static void (* implOnPlaylistOpenComplete)(void *env, void *thiz, int success, int count);
static void (* implOnRockstarInitialComplete)(void *env, void *thiz);
static void (* implOnRockstarGateComplete)(void *env, void *thiz, int gate, int success);
static void (* implOnRockstarSignInComplete)(void *env, void *thiz);
static void (* implOnRockstarSignOutComplete)(void *env, void *thiz);
static void (* implOnRockstarStateChanged)(void *env, void *thiz, int state);

#define GN "Java_com_rockstargames_oswrapper_GameNative_"

static void resolve_entry_points(void) {
  #define ENT(var, sym) var = (void *)so_find_addr_rx(&game_mod, GN sym)
  #define ENTOPT(var, sym) var = (void *)so_try_find_addr_rx(&game_mod, GN sym)
  ENT(implOnActivityCreated, "implOnActivityCreated");
  ENT(implOnActivityDestroyed, "implOnActivityDestroyed");
  ENT(implOnInitialSetup, "implOnInitialSetup");
  ENT(implOnSurfaceCreated, "implOnSurfaceCreated");
  ENT(implOnSurfaceChanged, "implOnSurfaceChanged");
  ENT(implOnSurfaceDestroyed, "implOnSurfaceDestroyed");
  ENT(implOnDrawFrame, "implOnDrawFrame");
  ENT(implOnResume, "implOnResume");
  ENT(implOnPause, "implOnPause");
  ENT(implIsInitialized, "implIsInitialized");
  ENT(implOnTouchStart, "implOnTouchStart");
  ENT(implOnTouchMove, "implOnTouchMove");
  ENT(implOnTouchEnd, "implOnTouchEnd");
  ENT(implOnGamepadConnected, "implOnGamepadConnected");
  ENT(implOnGamepadButtonDown, "implOnGamepadButtonDown");
  ENT(implOnGamepadButtonUp, "implOnGamepadButtonUp");
  ENT(implOnGamepadAxesChanged, "implOnGamepadAxesChanged");
  ENT(implOnBackButtonPressed, "implOnBackButtonPressed");
  ENTOPT(implOnPlaylistOpenComplete, "implOnPlaylistOpenComplete");
  ENTOPT(implOnRockstarInitialComplete, "implOnRockstarInitialComplete");
  ENTOPT(implOnRockstarGateComplete, "implOnRockstarGateComplete");
  ENTOPT(implOnRockstarSignInComplete, "implOnRockstarSignInComplete");
  ENTOPT(implOnRockstarSignOutComplete, "implOnRockstarSignOutComplete");
  ENTOPT(implOnRockstarStateChanged, "implOnRockstarStateChanged");
  #undef ENT
  #undef ENTOPT
  (void)implIsInitialized;
  (void)implOnBackButtonPressed; // native back-button is a no-op; '+' = pause
}

// ---------------------------------------------------------------------------
// touch
// ---------------------------------------------------------------------------

#define MAX_TOUCHES 8

typedef struct {
  int active;
  u32 finger_id;
  float x, y; // panel pixels
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
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;

  // scale panel (1280x720) coordinates to the render resolution
  const float sx = (float)screen_width / 1280.0f;
  const float sy = (float)screen_height / 720.0f;

  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count && i < MAX_TOUCHES; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * sx;
    const float y = (float)t->y * sy;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0)
        continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
      debugPrintf("TOUCH: start slot=%d x=%.0f y=%.0f\n", slot, x, y);
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
      debugPrintf("TOUCH: end slot=%d x=%.0f y=%.0f\n", slot, touch_prev[slot].x, touch_prev[slot].y);
      implOnTouchEnd(fake_env, NULL, slot, touch_prev[slot].x, touch_prev[slot].y);
      touch_prev[slot].active = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// gamepad
// ---------------------------------------------------------------------------

// fire the native completion callbacks queued by the fake JNI for the engine's
// async platform requests; driven once per main-loop iteration.
static void dispatch_jni_callbacks(void) {
  JniCallback cb;
  int n = 0;
  while (n++ < 16 && jni_pop_callback(&cb)) {
    switch (cb.type) {
      case JNI_CB_PLAYLIST_OPEN_COMPLETE:
        debugPrintf("cb: implOnPlaylistOpenComplete(%d, %d)\n", cb.arg0, cb.arg1);
        if (implOnPlaylistOpenComplete)
          implOnPlaylistOpenComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_INITIAL_COMPLETE:
        debugPrintf("cb: implOnRockstarInitialComplete\n");
        if (implOnRockstarInitialComplete)
          implOnRockstarInitialComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_GATE_COMPLETE:
        debugPrintf("cb: implOnRockstarGateComplete(%d, %d)\n", cb.arg0, cb.arg1);
        if (implOnRockstarGateComplete)
          implOnRockstarGateComplete(fake_env, NULL, cb.arg0, cb.arg1);
        break;
      case JNI_CB_ROCKSTAR_SIGNIN_COMPLETE:
        debugPrintf("cb: implOnRockstarSignInComplete\n");
        if (implOnRockstarSignInComplete)
          implOnRockstarSignInComplete(fake_env, NULL);
        break;
      case JNI_CB_ROCKSTAR_SIGNOUT_COMPLETE:
        debugPrintf("cb: implOnRockstarSignOutComplete\n");
        if (implOnRockstarSignOutComplete)
          implOnRockstarSignOutComplete(fake_env, NULL);
        break;
      default:
        break;
    }
  }
}

static PadState pad;
static u64 pad_prev = 0;

// Cheat entry: open the Switch on-screen keyboard and feed the typed code to the
// game's CCheat via cheats_enqueue (hooks/game.c). Blocks the main loop while the
// keyboard is up (the render thread keeps presenting the last frame).
static void open_cheat_keyboard(void) {
  SwkbdConfig kbd;
  if (R_FAILED(swkbdCreate(&kbd, 0)))
    return;
  swkbdConfigMakePresetDefault(&kbd);
  swkbdConfigSetHeaderText(&kbd, "Enter cheat code");
  swkbdConfigSetStringLenMax(&kbd, 63);
  char out[64] = { 0 };
  Result rc = swkbdShow(&kbd, out, sizeof(out));
  swkbdClose(&kbd);
  if (R_SUCCEEDED(rc) && out[0])
    cheats_enqueue(out);
}

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  for (unsigned int i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
    if (changed & pad_map[i].hid) {
      if (down & pad_map[i].hid) {
        implOnGamepadButtonDown(fake_env, NULL, 0, pad_map[i].button);
        movie_skip(); // the game ignores input while waiting for a movie
      } else {
        implOnGamepadButtonUp(fake_env, NULL, 0, pad_map[i].button);
      }
    }
  }

  // '+' edge -> pause/escape (consumed by the GetEscapeJustDown hook)
  if ((changed & HidNpadButton_Plus) && (down & HidNpadButton_Plus))
    g_escape_pressed = 1;

  // L3+R3 edge -> on-screen keyboard for cheat entry
  const u64 cheat_combo = HidNpadButton_StickL | HidNpadButton_StickR;
  if ((changed & cheat_combo) && (down & cheat_combo) == cheat_combo)
    open_cheat_keyboard();

  pad_prev = down;

  const float scale = 1.f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  // Android stick Y points down, libnx up, so negate Y. Triggers also reported
  // as analog 0..1 alongside the L2/R2 keycodes above.
  const float lx = (float)ls.x * scale;
  const float ly = (float)ls.y * -scale;
  const float rx = (float)rs.x * scale;
  const float ry = (float)rs.y * -scale;
  const float lt = (down & HidNpadButton_ZL) ? 1.0f : 0.0f;
  const float rt = (down & HidNpadButton_ZR) ? 1.0f : 0.0f;

  static float prev[6];
  if (lx != prev[0] || ly != prev[1] || rx != prev[2] ||
      ry != prev[3] || lt != prev[4] || rt != prev[5]) {
    prev[0] = lx; prev[1] = ly; prev[2] = rx; prev[3] = ry; prev[4] = lt; prev[5] = rt;
    implOnGamepadAxesChanged(fake_env, NULL, 0, lx, ly, rx, ry, lt, rt);
  }
}

int main(void) {
  // full clock for boot; dropped once the menu has rendered
  cpu_boost(1);

  // keep all GL on the render thread: a mesa worker thread issuing GL races
  // nouveau's small-buffer allocator and corrupts its pool
  setenv("MESA_GLTHREAD", "false", 1);
  setenv("GALLIUM_THREAD", "0", 1);

  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();

  // Social Club / cloud netcode uses BSD sockets; offline play still works if
  // this fails, so log but don't abort.
  Result sock_rc = socketInitializeDefault();
  if (R_FAILED(sock_rc))
    debugPrintf("socketInitializeDefault failed: %08x (networking disabled)\n", sock_rc);

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("mem: total=%zu MB | newlib(game+mesa+GPU)=%zu MB | .so region=%zu MB\n",
              g_mem_total_mb, g_mem_newlib_mb, g_mem_so_mb);
  debugPrintf("nv GPU transfermem pool = %u MB\n",
              (unsigned)(__nx_nv_transfermem_size >> 20));

  // load the C++ runtime donor first: the game's std::/__cxa_ imports resolve
  // into the donor's libc++
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
  movie_wad_init();   // resolve cFile syms now (symtab is freed by so_flush_caches)

  resolve_entry_points();
  int (* JNI_OnLoad)(void *vm, void *reserved) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");

  so_finalize(&donor_mod);
  so_finalize(&game_mod);
  so_flush_caches(&donor_mod);
  so_flush_caches(&game_mod);

  // donor's initializers (libc++ statics) must run before the game's
  so_execute_init_array(&donor_mod);
  so_execute_init_array(&game_mod);

  so_free_temp(&donor_mod);
  so_free_temp(&game_mod);

  jni_init();

  // JNI_OnLoad internally runs NVThreadInit(vm) and caches the env
  debugPrintf("calling JNI_OnLoad\n");
  JNI_OnLoad(fake_vm, NULL);

  // ---- setup (replaces the Android GameActivity / GLSurfaceView bring-up) ----
  void *gn_class = jni_make_object("com/rockstargames/oswrapper/GameNative");
  void *activity = jni_make_object("GameActivity");
  void *asset_mgr = jni_make_object("AssetManager");
  void *surface   = jni_make_object("Surface");
  // no APK asset packs: the engine reads data off disk via AAsset emulation
  void *names = jni_make_string_array(0, NULL);
  void *paths = jni_make_string_array(0, NULL);

  debugPrintf("setup: implOnActivityCreated / implOnInitialSetup\n");
  implOnActivityCreated(fake_env, gn_class, activity);
  implOnInitialSetup(fake_env, gn_class, activity, asset_mgr, names, paths);

  debugPrintf("surface: implOnSurfaceCreated / implOnSurfaceChanged\n");
  implOnSurfaceCreated(fake_env, gn_class);
  implOnSurfaceChanged(fake_env, gn_class, surface, screen_width, screen_height);
  implOnResume(fake_env, gn_class);

  // the Rockstar/playlist completion callbacks are driven from the main loop
  // via dispatch_jni_callbacks(), not here
  implOnGamepadConnected(fake_env, gn_class, 0);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  const u64 tick_freq = armGetSystemTickFreq();
  u64 last_tick = armGetSystemTick();
  int boot_frames = 0;

  while (appletMainLoop() && !jni_quit_requested) {
    // fire native completion callbacks the engine is blocked on before the tick
    dispatch_jni_callbacks();

    update_gamepad();
    update_touch();

    const u64 now = armGetSystemTick();
    float dt = (float)(now - last_tick) / (float)tick_freq;
    last_tick = now;
    // clamp dt so a stray fast iteration never feeds the engine a near-zero
    // timestep (which stalls its timers); the loop is paced to ~60fps below
    if (dt < 1.0f / 120.0f || dt > 0.5f)
      dt = 1.0f / 60.0f;

    implOnDrawFrame(fake_env, NULL, dt);

    // keep movie playback going when the game stops rendering during it
    movie_main_loop_tick();
    jni_video_tick();

    if (boot_frames < 10) {
      if (++boot_frames == 10)
        cpu_boost(0);
    }

    // pace the loop to ~60fps: the engine renders on its own thread, so nothing
    // here blocks on vsync and the loop would otherwise free-run
    const u64 frame_ticks = tick_freq / 60;
    const u64 used = armGetSystemTick() - now;
    if (used < frame_ticks)
      svcSleepThread((frame_ticks - used) * 1000000000ULL / tick_freq);
  }

  implOnPause(fake_env, NULL);
  implOnSurfaceDestroyed(fake_env, NULL);
  implOnActivityDestroyed(fake_env, NULL);

  movie_stop();
  deinit_openal();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);

  return 0;
}
