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

Low Performance : CPU-bound on draw calls, the game issues a lot of GL calls per frame and Switch mesa driver is slow per-call and the GPU sits idle most of the time 
Best lever today is CPU overclock a real fix would need a thinner/threaded GL driver

This will not work in applet/album mode (it needs the full memory + syscall set).
Launch it through a **game override** (hold R on an installed title) or a
forwarder.

Save games and settings are stored in `/switch/gtasa/`.

The port has a config file at `/switch/gtasa/config.txt`, created on first run:
* `screen_width` / `screen_height` — render resolution; `-1` picks 1280x720 in
  handheld and 1920x1080 docked
* `trilinear_filter` — `1` forces trilinear texture filtering
* `show_fps` — `1` draws a small FPS counter in the top-left corner
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

### Mod Settings Menu

The port includes a built-in configurator for toggling the mod's fixes and features.

**To open it:** at launch, a splash screen appears for ~3 seconds
("Hold ZR for Mod Settings"). **Hold ZR** during this window to enter the menu.
If you don't hold ZR, the game boots normally.

### How to build

You need **devkitA64** with `DEVKITPRO` set in the environment. The full flow is
three steps; the CI workflow (`.github/workflows/build.yml`) does exactly the same.

**1. Install the Switch portlibs:**

```sh
dkp-pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-mpg123 switch-openal-soft devkitpro-pkgbuild-helpers
```

**2. Build the patched Mesa** (required — the stock `switch-mesa` ships with the
on-disk shader cache disabled on Horizon, so shaders are recompiled every launch,
causing stutter). This rebuilds `switch-mesa` from the devkitPro Mesa fork with
`patches/mesa-switch-shadercache.patch` and installs it over the stock portlib:

```sh
bash scripts/build-mesa.sh
```

The script installs its own Mesa build dependencies — `meson`, `ninja`, `bison`,
`flex`, `python3-mako` (via `apt`) and `dkp-meson-scripts`, `dkp-toolchain-vars`,
`switch-pkg-config` (via `dkp-pacman`). Re-run it only when the patch or the
pinned Mesa commit changes.

**3. Build the `.nro`:**

```sh
make
```

### Credits

* TheOfficialFloW for the method and the original PS Vita work;
* fgsfds for max_nx, which the shared Switch platform layer is based on;
* Gameplay and engine improvements ported from the [GTA:SA PS Vita port](https://github.com/TheOfficialFloW/gtasa_vita).
* Extra patches and hooks adapted from [JPatch](https://github.com/AndroidModLoader/JPatch).
* See the [CHANGELOG.md](CHANGELOG.md) for everything that changed.

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
