<div align=center>

<img src="extras/banner.png" alt="Banner" width="35%">

</div>
<h1 align=center>GTA: San Andreas - Nintendo Switch port</h1>

This is a wrapper/port of the Android version of Grand Theft Auto: San Andreas
(**v2.11.311**, arm64-v8a). It loads the original game binary, patches it and
runs it. It's basically a minimalist Android environment in which we natively
run the original 64-bit Android binary as-is.

### How to install

You're going to need:
* the **arm64-v8a** `.apk` (and `.obb`) for version **2.11.311**.

To install:
1. Create a folder called `gtasa` in the `switch` folder on your SD card.
2. From the **arm64-v8a** APK, extract these two native libraries to
   `/switch/gtasa/`:
   * `lib/arm64-v8a/libGame.so`
   * `lib/arm64-v8a/libc++_shared.so`
3. Extract the **game data** so the files sit loose under `/switch/gtasa/`,
   preserving their directory structure:
   * everything under the APK's `assets/` folder, **and**
   * the contents of the OBB (`main.*.obb` — it is just a ZIP; the `data/`,
     `models/`, `texdb/`, `audio/`, `text/`, `anim/`, `es2/`, … trees inside it
     go directly in `/switch/gtasa/`).
4. Copy `gtasa_nx.nro` into `/switch/gtasa/`.

Your SD card should end up with at least `/switch/gtasa/gtasa_nx.nro`,
`/switch/gtasa/libGame.so`, `/switch/gtasa/libc++_shared.so`, and the extracted
game data folders (`data/`, `models/`, `texdb/`, `audio/`, …) all inside
`/switch/gtasa/`.

### Notes

This will not work in applet/album mode (it needs the full memory + syscall set).
Launch it through a **game override** (hold R on an installed title) or a
forwarder.

Save games and settings are stored in `/switch/gtasa/`.

The port has a config file at `/switch/gtasa/gtasa_nx.cfg`, created on first run:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720 in
  handheld and 1920x1080 docked
* `trilinear_filter` — `1` forces trilinear texture filtering
* `show_fps` — `1` draws a small FPS counter in the top-left corner
* `fps_cap_30` — `1` enables the wrapper's 30 FPS cap
* `auto_boot_delay` — launcher countdown in seconds (`1`, `3`, `5`, or `10`)
* `ps2_corona_rotation` — `1` PS2 Corona Sun
* `ps2_color_filter` — `1` PS2 Color filter
* `sprint_any_surface` — `0` Sprinting on any surface is allowed
* `remove_air_resistance` — `0` Remove "ExtraAirResistance" flag
* `show_wanted_stars` — `0` Always drawable wanted stars
* `disable_ped_spec` — `1` Removed specular lighting on pedestrians
* `no_offscreen_despawn` — `0` Cars and peds don't despawn when you look away
* `mobile_widgets` — `0` Hidden Mobile Widgets

### Tips and Tricks

- You can input PC cheats by pressing **R3** + **L3** to open the on-screen keyboard. See [CHEATS.md](CHEATS.md) for available and unavailable cheats (you can input cheat codes in lowercase as well as uppercase).
- Due to expired licensing, some songs were cut from the game. See [MUSIC.md](MUSIC.md) for a list of removed tracks and a guide on how to restore them.
- Console-style HUD (optional). Drop a custom `Adjustable.cfg` into `switch/gtasa/` for the console HUD (e.g. radar in the bottom-left corner). Since **v2.11.311** no longer includes `data/360Default1280x720.cfg`, take it from the older **v2.10** build and rename it to `Adjustable.cfg`. It's a leftover from the Xbox 360 version.
- In order to reduce occasional stutters in-game, delete both `switch/gtasa/scache_small_low.txt` and `switch/gtasa/scache_small.txt`, then create a copy of the `switch/gtasa/scache.txt` file to have two version of it. (for example `scache(1).txt` so in the end you end up with both `scache.txt` and `scache(1).txt` inside the `switch/gtasa/` folder), then rename `scache.txt` to `scache_small.txt` and `scache(1).txt` to `scache_small_low.txt` . This will however make the loading screen longer since it needs to compile more shaders ahead.
  - Mesa stores its persistent single-file shader cache under `switch/gtasa/shadercache/mesa_shader_cache_sf`. Delete that directory only when diagnosing a corrupt cache.

### Mod Settings Menu

The SDL launcher opens on every boot with the game cover centered. Select the
cover to launch, or open **Options** to change the port's fixes and features.
Changes are saved immediately to `gtasa_nx.cfg`.

### How to build

**1. Install the Switch portlibs:**

```sh
dkp-pacman -S switch-sdl2 switch-sdl2_image switch-mpg123 switch-ffmpeg switch-openal-soft switch-libexpat switch-libzstd switch-zlib
```

**2. Build the `.nro`:**

```sh
make
```

### Credits

* TheOfficialFloW for the method and the original PS Vita work;
* fgsfds for max_nx, which the shared Switch platform layer is based on;
* Gameplay and engine improvements ported from the [GTA:SA PS Vita port](https://github.com/TheOfficialFloW/gtasa_vita).
* Extra patches and hooks adapted from [JPatch](https://github.com/AndroidModLoader/JPatch).

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no direct affiliation with Take-Two Interactive Software, Inc.
or Rockstar Games, Inc. "Grand Theft Auto" and "Grand Theft Auto: San Andreas"
are trademarks of their respective owners. All Rights Reserved.

No assets or program code from the original game or its Android port are included
in this project. We do not condone piracy in any way, shape or form and encourage
users to legally own the original game.

Unless specified otherwise, the source code provided in this repository is
licensed under the MIT License. Please see the accompanying LICENSE file.
