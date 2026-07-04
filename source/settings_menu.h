/* settings_menu.h -- pre-boot mod settings menu (libnx console). */

#ifndef __SETTINGS_MENU_H__
#define __SETTINGS_MENU_H__

// Shown at launch ONLY when ZR is held, before patch_game() so toggles apply the
// same boot. Renders with the libnx console (no GL) -> no interference with the
// game's mesa/nouveau renderer. If ZR is not held it returns immediately without
// touching the console. Saves to CONFIG_NAME on confirm.
void settings_menu_maybe_show(void);

#endif
