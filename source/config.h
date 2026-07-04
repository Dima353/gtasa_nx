/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB reserved for the .so load region: it only holds the RW staging copies of
// the mapped libraries, and the rest of RAM goes to the newlib heap (game malloc
// + mesa GPU bos). Sizing this larger starves the GPU and OOMs SA's texture
// streaming, so keep it small.
#define MEMORY_SO_MB 64

// libc++_shared.so is the C++ runtime donor: the game's std::/__cxa_ imports
// resolve into it module-to-module. OpenAL still binds to native openal-soft
// because the import table beats module exports (so_resolve_symbol).
#define SO_NAME "libGame.so"
#define CXX_DONOR_SO_NAME "libc++_shared.so"
// the game ships its own binary "config.txt" at the data root, so the wrapper's
// config uses a distinct name to avoid parsing that blob as text
#define CONFIG_NAME "gtasa_nx.cfg"
#define LOG_NAME "debug.log"
// backing store for the engine's get/setAppLocalValue key/value pairs
#define APPSTATE_NAME "appstate.txt"

// Define to write debug.log and nxlink stdout. Off for release (debugPrintf
// becomes a no-op).
//#define DEBUG_LOG 1

// actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int show_fps;
  // PS2-style rendering (SkyGFX-derived) toggles. Each 1 = on, 0 = off.
  int ps2_corona_rotation; // #1: coronas spin like PS2/PC
  int ps2_color_filter;    // #3: PS2 color-grade filter in CPostEffects::MobileRender
  // Gameplay-feel fixes (Vita Category A). 1 = fix applied, 0 = stock. Default 0.
  int sprint_any_surface;    // SurfaceInfos_c::CantSprintOn -> ret0 (sprint on sand/grass)
  int remove_air_resistance; // CCullZones::DoExtraAirResistanceForPlayer -> ret0
  int show_wanted_stars;     // CWidgetPlayerInfo::DrawWanted: always draw the stars
  int disable_ped_spec;      // BuildPixelSource: drop BONE3|BONE4 from the spec gate
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
