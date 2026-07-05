/* settings_menu.c -- pre-boot mod settings menu (libnx framebuffer, GTA:SA styled).
 *
 * On every launch this briefly shows a splash ("Hold ZR for Mod Settings"). Holding
 * ZR during that ~2s window opens a menu that toggles the wrapper's fix/feature
 * config flags and saves them; otherwise it exits and the game boots normally. It
 * runs in main() right after read_config() and BEFORE patch_game(), so every toggle
 * -- including the boot-time hooks/byte-patches -- takes effect on this same launch
 * (patch_game reads config afterwards); no separate restart is needed.
 *
 * WHY a splash and not a silent held-at-launch check: a silent poll at the very start
 * of main() did NOT detect a held ZR -- HID isn't reliably readable that early / the
 * launcher hand-off eats the hold. Once we bring up a framebuffer and present a few
 * frames the pad reads correctly, so we show the splash first and poll during it.
 *
 * Rendered by drawing RGBA pixels straight into the libnx framebuffer (like the
 * console did, but graphical): a charcoal background, an amber title/footer bar, and
 * toggle rows drawn with the shipped Consolas atlas (font_atlas.h). This runs before
 * the game's EGL is set up, so it can't touch the game's mesa/nouveau renderer -- the
 * framebuffer is created and closed here, exactly like consoleInit/consoleExit were.
 *
 * To expose a new fix: add an `int` to Config (config.h/.c) and one row to the
 * `items` table in settings_menu_maybe_show().
 */

#include <switch.h>
#include <string.h>

#include "config.h"
#include "font_atlas.h"
#include "settings_menu.h"

typedef struct {
  const char *label;
  int *value;
} MenuItem;

#define FB_W 1280
#define FB_H 720
#define TITLE_H 64
#define FOOTER_H 44
#define ROW_STEP 48
#define ROW_H 44
#define GLYPH_ADV FONT_CELL_W // monospace advance (Consolas cell is 20 wide)
#define GLYPH_CROP 2          // skip the last 2 atlas rows: no glyph draws there, and
                              // it strips a stray artifact row under 'l' (font bleed)
#define TEXT_DY ((ROW_H - FONT_CELL_H) / 2) // vertical centring of a text cell in a row

// ~3s window (frames * vsync). If ZR is already held it opens on the first frame.
#define PROMPT_FRAMES 180

// RGBA8888 is little-endian byte order R,G,B,A -> u32 = A<<24|B<<16|G<<8|R. RGB()
// takes an 0xRRGGBB literal and packs it opaque.
#define RGB(h)                                                                 \
  ((u32)0xFF000000u | ((u32)((h) & 0xFF) << 16) |                              \
   ((u32)(((h) >> 8) & 0xFF) << 8) | ((u32)(((h) >> 16) & 0xFF)))

#define C_BG RGB(0x14120e)      // charcoal background
#define C_BAR RGB(0x1c1a14)     // title/footer bar
#define C_AMBER RGB(0xe0a030)   // GTA:SA accent
#define C_TEXT RGB(0xefe7d6)    // selected/bright label
#define C_TEXTDIM RGB(0xcfc7b6) // unselected label
#define C_MUTED RGB(0x8a8270)   // hints, "on" toggle fill when unselected
#define C_ROWSEL RGB(0x2a2618)  // selected row background
#define C_OFFBORD RGB(0x3a3527) // "off" pill border
#define C_OFFTEXT RGB(0x6a6455) // "off" pill text
#define C_DARK RGB(0x14120e)    // text on an amber/muted pill
#define C_FOOTLINE RGB(0x2a2618)

static Framebuffer g_fb;
static u32 *g_buf;  // current frame's pixels (from framebufferBegin)
static u32 g_pitch; // pixels per row (byte stride / 4)

static void fill(int x, int y, int w, int h, u32 c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > FB_W) w = FB_W - x;
  if (y + h > FB_H) h = FB_H - y;
  for (int j = 0; j < h; j++) {
    u32 *row = g_buf + (y + j) * g_pitch + x;
    for (int i = 0; i < w; i++) row[i] = c;
  }
}

static void rect_border(int x, int y, int w, int h, u32 c) {
  fill(x, y, w, 1, c);
  fill(x, y + h - 1, w, 1, c);
  fill(x, y, 1, h, c);
  fill(x + w - 1, y, 1, h, c);
}

// alpha-blend src over the pixel at (x,y) with coverage a (0..255).
static void blend(int x, int y, u32 src, unsigned a) {
  if ((unsigned)x >= FB_W || (unsigned)y >= FB_H) return;
  u32 *p = &g_buf[y * g_pitch + x];
  u32 d = *p;
  unsigned ia = 255 - a;
  unsigned r = ((src & 0xFF) * a + (d & 0xFF) * ia) / 255;
  unsigned g = (((src >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia) / 255;
  unsigned b = (((src >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia) / 255;
  *p = 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void glyph(int x, int y, char ch, u32 color) {
  int idx = (unsigned char)ch - FONT_FIRST;
  if (idx < 0 || idx >= FONT_COUNT) return;
  int ax = (idx % FONT_COLS) * FONT_CELL_W;
  int ay = (idx / FONT_COLS) * FONT_CELL_H;
  for (int gy = 0; gy < FONT_CELL_H - GLYPH_CROP; gy++) {
    const unsigned char *arow = &font_atlas[(ay + gy) * FONT_ATLAS_W + ax];
    for (int gx = 0; gx < FONT_CELL_W; gx++)
      if (arow[gx]) blend(x + gx, y + gy, color, arow[gx]);
  }
}

static void text(int x, int y, const char *s, u32 color) {
  for (; *s; s++, x += GLYPH_ADV) glyph(x, y, *s, color);
}

static int text_w(const char *s) { return (int)strlen(s) * GLYPH_ADV; }

// right-aligned toggle pill: filled amber (bright when the row is selected, muted
// otherwise) for ON, hollow bordered for OFF.
static void pill(int right_edge, int row_y, int on, int selected) {
  const char *lbl = on ? "ON" : "OFF";
  int tw = text_w(lbl);
  int pw = tw + 22, ph = 32;
  int px = right_edge - pw;
  int py = row_y + (ROW_H - ph) / 2;
  int tx = px + (pw - tw) / 2;
  if (on) {
    fill(px, py, pw, ph, selected ? C_AMBER : C_MUTED);
    text(tx, row_y + TEXT_DY, lbl, C_DARK);
  } else {
    rect_border(px, py, pw, ph, C_OFFBORD);
    text(tx, row_y + TEXT_DY, lbl, C_OFFTEXT);
  }
}

// one footer hint: white button glyph(s) + muted action, advancing *x.
static void foot(int *x, int y, const char *key, const char *act) {
  text(*x, y, key, C_TEXT);
  *x += text_w(key) + GLYPH_ADV / 2;
  text(*x, y, act, C_MUTED);
  *x += text_w(act) + GLYPH_ADV * 2;
}

static void begin_frame(void) {
  u32 stride;
  g_buf = (u32 *)framebufferBegin(&g_fb, &stride);
  g_pitch = stride / 4;
}

static void draw_splash(void) {
  fill(0, 0, FB_W, FB_H, C_BG);
  const char *t = "GTA : San Andreas NX";
  text((FB_W - text_w(t)) / 2, 296, t, C_AMBER);
  fill((FB_W - 360) / 2, 344, 360, 2, C_ROWSEL);
  const char *s = "Hold  ZR  for Mod Settings";
  text((FB_W - text_w(s)) / 2, 366, s, C_MUTED);
}

static void draw_menu(const MenuItem *items, int n, int cursor) {
  fill(0, 0, FB_W, FB_H, C_BG);

  // title bar
  fill(0, 0, FB_W, TITLE_H, C_BAR);
  fill(0, TITLE_H - 2, FB_W, 2, C_AMBER);
  text(28, 14, "GTA : San Andreas", C_AMBER);
  text(28 + text_w("GTA : San Andreas") + GLYPH_ADV, 14, "NX", C_TEXT);
  const char *sub = "MOD SETTINGS";
  text(FB_W - 28 - text_w(sub), 18, sub, C_MUTED);

  // rows (vertically centred between the bars)
  int start_y = TITLE_H + ((FB_H - TITLE_H - FOOTER_H) - n * ROW_STEP) / 2;
  for (int i = 0; i < n; i++) {
    int y = start_y + i * ROW_STEP;
    if (i == cursor) {
      fill(24, y, 1232, ROW_H, C_ROWSEL);
      fill(24, y, 4, ROW_H, C_AMBER);
    }
    text(52, y + TEXT_DY, items[i].label, i == cursor ? C_TEXT : C_TEXTDIM);
    pill(1240, y, *items[i].value, i == cursor);
  }

  // footer hint bar: button glyphs in white, their actions muted
  fill(0, FB_H - FOOTER_H, FB_W, FOOTER_H, C_BAR);
  fill(0, FB_H - FOOTER_H, FB_W, 1, C_FOOTLINE);
  int fx = 28, fy = FB_H - FOOTER_H + (FOOTER_H - FONT_CELL_H) / 2;
  foot(&fx, fy, "A", "Toggle");
  foot(&fx, fy, "Up/Dn", "Move");
  foot(&fx, fy, "+", "Save & Launch");
  foot(&fx, fy, "B", "Cancel & Launch");
}

void settings_menu_maybe_show(void) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  framebufferCreate(&g_fb, nwindowGetDefault(), FB_W, FB_H,
                    PIXEL_FORMAT_RGBA_8888, 2);
  framebufferMakeLinear(&g_fb);

  // Splash phase: present the hint and poll ZR for a short window. The pad reads
  // reliably once we're presenting frames (not before), so the splash doubles as
  // the settle time.
  int open = 0;
  for (int f = 0; f < PROMPT_FRAMES && appletMainLoop(); f++) {
    begin_frame();
    draw_splash();
    framebufferEnd(&g_fb); // presents + vsync
    padUpdate(&pad);
    if (padGetButtons(&pad) & HidNpadButton_ZR) {
      open = 1;
      break;
    }
  }

  if (!open) {
    framebufferClose(&g_fb); // no ZR -> boot the game
    return;
  }

  const MenuItem items[] = {
      {"PS2 Corona Sun", &config.ps2_corona_rotation},
      {"PS2 Color Filter", &config.ps2_color_filter},
      {"Disable ped specular shine", &config.disable_ped_spec},
      {"Show Wanted Stars", &config.show_wanted_stars},
      {"No off-screen despawn", &config.no_offscreen_despawn},
      {"Remove \"ExtraAirResistance\"", &config.remove_air_resistance},
      {"Sprint on any surface", &config.sprint_any_surface},
      {"Trilinear filtering", &config.trilinear_filter},
      {"FPS counter (may stall)", &config.show_fps},
  };
  const int n = (int)(sizeof(items) / sizeof(items[0]));
  int cursor = 0, save = 0, stick_ready = 1;

  while (appletMainLoop()) {
    begin_frame();
    draw_menu(items, n, cursor);
    framebufferEnd(&g_fb);

    padUpdate(&pad);
    const u64 k = padGetButtonsDown(&pad);

    // left stick as an alternative to the D-pad (edge-triggered so a held stick
    // steps once, then must return near centre before it steps again)
    HidAnalogStickState ls = padGetStickPos(&pad, 0);
    int down = (k & HidNpadButton_Down) != 0;
    int up = (k & HidNpadButton_Up) != 0;
    if (stick_ready && ls.y < -16000) { down = 1; stick_ready = 0; }
    else if (stick_ready && ls.y > 16000) { up = 1; stick_ready = 0; }
    else if (ls.y > -8000 && ls.y < 8000) { stick_ready = 1; }

    if (down) cursor = (cursor + 1) % n;
    if (up) cursor = (cursor + n - 1) % n;
    if (k & HidNpadButton_A) *items[cursor].value = !*items[cursor].value;
    if (k & HidNpadButton_Plus) { save = 1; break; }
    if (k & HidNpadButton_B) break;
  }

  framebufferClose(&g_fb);

  if (save)
    write_config(CONFIG_NAME);
}
