/* settings_menu.c -- pre-boot mod settings menu (libnx console).
 *
 * On every launch this briefly shows a console prompt ("Hold ZR for Mod
 * Settings"). Holding ZR during that ~2s window opens a menu that toggles the
 * wrapper's fix/feature config flags and saves them; otherwise it exits and the
 * game boots normally. It runs in main() right after read_config() and BEFORE
 * patch_game(), so every toggle -- including the boot-time hooks/byte-patches --
 * takes effect on this same launch (patch_game reads config afterwards); no
 * separate restart is needed.
 *
 * WHY the prompt splash and not a silent held-at-launch check: a silent poll at
 * the very start of main() (before consoleInit) did NOT detect a held ZR -- HID
 * isn't reliably readable that early / the launcher hand-off eats the hold. Once
 * the console is up the pad reads correctly (verified: ZR == HidNpadButton_ZR ==
 * 0x200), so we bring the console up first and poll during a visible prompt.
 *
 * Rendered with the libnx console (framebuffer text) + pad API, like error.c.
 * This deliberately avoids the in-game GL overlay (fps_render), which stalls the
 * game on this driver.
 *
 * To expose a new fix: add an `int` to Config (config.h/.c) and one row to the
 * `items` table below.
 */

#include <switch.h>
#include <stdio.h>

#include "config.h"
#include "settings_menu.h"

typedef struct {
  const char *label;
  int *value;
} MenuItem;

// ~2s window (frames * sleep). If ZR is already held it opens on the first frame.
#define PROMPT_FRAMES 120
#define FRAME_NS 16000000LL // 16 ms

void settings_menu_maybe_show(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  // Prompt phase: draw a static hint (no countdown) once, then poll ZR for a
  // short window. The console must be up for the pad to read reliably this early,
  // so the hint doubles as the reason it's on screen.
  consoleClear();
  printf("\n\n");
  printf("      GTA:SA NX\n\n");
  printf("      Hold  ZR  for Mod Settings...\n");
  consoleUpdate(NULL);

  int open = 0;
  for (int f = 0; f < PROMPT_FRAMES && appletMainLoop(); f++) {
    padUpdate(&pad);
    if (padGetButtons(&pad) & HidNpadButton_ZR) {
      open = 1;
      break;
    }
    svcSleepThread(FRAME_NS);
  }

  if (!open) {
    consoleExit(NULL); // no ZR -> boot the game
    return;
  }

  MenuItem items[] = {
      {"PS2 corona rotation", &config.ps2_corona_rotation},
      {"PS2 color filter", &config.ps2_color_filter},
      {"Sprint on any surface", &config.sprint_any_surface},
      {"Remove extra air resistance", &config.remove_air_resistance},
      {"Always show wanted stars", &config.show_wanted_stars},
      {"Disable ped specular shine", &config.disable_ped_spec},
      {"Trilinear filtering", &config.trilinear_filter},
      {"FPS counter (may stall)", &config.show_fps},
  };
  const int n = (int)(sizeof(items) / sizeof(items[0]));
  int cursor = 0;
  int save = 0;
  int redraw = 1;

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 k = padGetButtonsDown(&pad);

    if (k & HidNpadButton_Down) {
      cursor = (cursor + 1) % n;
      redraw = 1;
    }
    if (k & HidNpadButton_Up) {
      cursor = (cursor + n - 1) % n;
      redraw = 1;
    }
    if (k & HidNpadButton_A) {
      *items[cursor].value = !*items[cursor].value;
      redraw = 1;
    }
    if (k & HidNpadButton_Plus) {
      save = 1;
      break;
    }
    if (k & HidNpadButton_B)
      break;

    if (redraw) {
      consoleClear();
      printf("======================================\n");
      printf("      GTA:SA NX  -  Mod Settings\n");
      printf("======================================\n\n");
      for (int i = 0; i < n; i++)
        printf("   %c  %-28s [%c]\n", i == cursor ? '>' : ' ', items[i].label,
               *items[i].value ? 'x' : ' ');
      printf("\n--------------------------------------\n");
      printf(" D-Pad Up/Down : move\n");
      printf(" A             : toggle\n");
      printf(" +             : save & start game\n");
      printf(" B             : start without saving\n");
      redraw = 0;
    }
    consoleUpdate(NULL);
  }

  consoleExit(NULL);

  if (save)
    write_config(CONFIG_NAME);
}
