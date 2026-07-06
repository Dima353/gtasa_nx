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
* `xbox_layout` — `1` uses the original Xbox/positional face buttons; default
  `0` matches the Switch's A/B/X/Y labels

### How to build

You're going to need devkitA64 and the following packages/libraries:
* `switch-mesa`
* `switch-libdrm_nouveau`
* `switch-sdl2`
* `switch-mpg123`
* `switch-openal-soft`
* `devkitpro-pkgbuild-helpers`

Then run `make` (with `DEVKITPRO` set in the environment).

### Credits

* TheOfficialFloW for the method and the original PS Vita work;
* fgsfds for max_nx, which the shared Switch platform layer is based on;

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
