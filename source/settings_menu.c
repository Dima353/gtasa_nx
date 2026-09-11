/* SDL launcher and pre-boot options. */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "config.h"
#include "font_atlas.h"
#include "settings_menu.h"

#define BASE_W 1920
#define BASE_H 1080
#define COVER_W 450
#define COVER_H 675
#define PULSE_PERIOD 2.0f
#define PULSE_MAX_SCALE 1.08f
#define STICK_THRESHOLD 16000
#define STICK_DEADZONE 8000
#define NUM_OPTION_ROWS 13
#define OPT_VISIBLE_ROWS 9

typedef enum {
  LAUNCHER_MAIN,
  LAUNCHER_OPTIONS,
} LauncherView;

static SDL_Texture *font_texture;

static const char *const option_labels[NUM_OPTION_ROWS] = {
  "AUTO BOOT",
  "PS2 CORONA SUN",
  "PS2 COLOR FILTER",
  "NO PED SPECULAR",
  "SHOW WANTED STARS",
  "KEEP OFFSCREEN NPCS",
  "NO EXTRA AIR RESIST",
  "SPRINT ANY SURFACE",
  "TRILINEAR FILTER",
  "MOBILE WIDGETS",
  "FUZZY SEEK",
  "FPS COUNTER",
  "30 FPS CAP",
};

static int *option_value(int row) {
  switch (row) {
  case 1: return &config.ps2_corona_rotation;
  case 2: return &config.ps2_color_filter;
  case 3: return &config.disable_ped_spec;
  case 4: return &config.show_wanted_stars;
  case 5: return &config.no_offscreen_despawn;
  case 6: return &config.remove_air_resistance;
  case 7: return &config.sprint_any_surface;
  case 8: return &config.trilinear_filter;
  case 9: return &config.mobile_widgets;
  case 10: return &config.fuzzy_seek;
  case 11: return &config.show_fps;
  case 12: return &config.fps_cap_30;
  default: return NULL;
  }
}

static int valid_auto_boot_delay(int delay) {
  return delay == 1 || delay == 3 || delay == 5 || delay == 10;
}

static void step_auto_boot_delay(int direction) {
  static const int delays[] = { 1, 3, 5, 10 };
  int current = 1;
  for (unsigned i = 0; i < sizeof(delays) / sizeof(*delays); i++) {
    if (config.auto_boot_delay == delays[i]) {
      current = (int)i;
      break;
    }
  }
  current = (current + direction + (int)(sizeof(delays) / sizeof(*delays))) %
            (int)(sizeof(delays) / sizeof(*delays));
  config.auto_boot_delay = delays[current];
  write_config(CONFIG_NAME);
}

static float ui_scale(int w, int h) {
  float sx = (float)w / (float)BASE_W;
  float sy = (float)h / (float)BASE_H;
  return sx < sy ? sx : sy;
}

static int text_width(const char *text, float scale) {
  return (int)((float)(strlen(text) * FONT_CELL_W) * scale + 0.5f);
}

static SDL_Texture *create_font_texture(SDL_Renderer *ren) {
  const size_t count = FONT_ATLAS_W * FONT_ATLAS_H;
  uint32_t *pixels = malloc(count * sizeof(*pixels));
  if (!pixels)
    return NULL;

  for (size_t i = 0; i < count; i++)
    pixels[i] = ((uint32_t)font_atlas[i] << 24) | 0x00ffffffu;

  SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
      pixels, FONT_ATLAS_W, FONT_ATLAS_H, 32, FONT_ATLAS_W * 4,
      0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u);
  SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(ren, surface) : NULL;
  if (surface)
    SDL_FreeSurface(surface);
  free(pixels);

  if (texture)
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

static void draw_text(SDL_Renderer *ren, int x, int y, float scale,
                      const char *text, unsigned char r, unsigned char g,
                      unsigned char b, unsigned char a) {
  if (!font_texture)
    return;

  int cw = (int)(FONT_CELL_W * scale + 0.5f);
  int ch = (int)(FONT_CELL_H * scale + 0.5f);
  if (cw < 1) cw = 1;
  if (ch < 1) ch = 1;
  SDL_SetTextureColorMod(font_texture, r, g, b);
  SDL_SetTextureAlphaMod(font_texture, a);

  for (const char *p = text; *p; p++, x += cw) {
    int index = (unsigned char)*p - FONT_FIRST;
    if (index < 0 || index >= FONT_COUNT)
      continue;
    SDL_Rect src = {
      (index % FONT_COLS) * FONT_CELL_W,
      (index / FONT_COLS) * FONT_CELL_H,
      FONT_CELL_W,
      FONT_CELL_H,
    };
    SDL_Rect dst = { x, y, cw, ch };
    SDL_RenderCopy(ren, font_texture, &src, &dst);
  }
}

static void draw_centered_text(SDL_Renderer *ren, int cx, int y, float scale,
                               const char *text, unsigned char r,
                               unsigned char g, unsigned char b,
                               unsigned char a) {
  draw_text(ren, cx - text_width(text, scale) / 2, y, scale, text, r, g, b, a);
}

static void draw_outline(SDL_Renderer *ren, const SDL_Rect *rect, int thick,
                         unsigned char r, unsigned char g, unsigned char b,
                         unsigned char a) {
  SDL_SetRenderDrawColor(ren, r, g, b, a);
  for (int i = 0; i < thick; i++) {
    SDL_Rect line = {
      rect->x - i,
      rect->y - i,
      rect->w + i * 2,
      rect->h + i * 2,
    };
    SDL_RenderDrawRect(ren, &line);
  }
}

static int rect_contains(const SDL_Rect *rect, int x, int y) {
  return x >= rect->x && x < rect->x + rect->w &&
         y >= rect->y && y < rect->y + rect->h;
}

static SDL_Rect cover_rect(int w, int h) {
  float scale = ui_scale(w, h);
  int cw = (int)(COVER_W * scale + 0.5f);
  int ch = (int)(COVER_H * scale + 0.5f);
  SDL_Rect rect = { (w - cw) / 2, (h - ch) / 2, cw, ch };
  return rect;
}

static SDL_Rect options_button_rect(int w, int h) {
  float scale = ui_scale(w, h);
  int bw = (int)(230.0f * scale + 0.5f);
  int bh = (int)(56.0f * scale + 0.5f);
  int margin = (int)(38.0f * scale + 0.5f);
  if (bw < 156) bw = 156;
  if (bh < 38) bh = 38;
  if (margin < 24) margin = 24;
  SDL_Rect rect = { w - bw - margin, margin, bw, bh };
  return rect;
}

static SDL_Rect options_panel_rect(int w, int h) {
  float scale = ui_scale(w, h);
  int pw = (int)(920.0f * scale + 0.5f);
  int ph = (int)(900.0f * scale + 0.5f);
  if (pw > w - 80) pw = w - 80;
  if (ph > h - 80) ph = h - 80;
  if (pw < 560) pw = 560;
  if (ph < 360) ph = 360;
  SDL_Rect rect = { (w - pw) / 2, (h - ph) / 2, pw, ph };
  return rect;
}

static SDL_Rect option_row_rect(const SDL_Rect *panel, int row, int w, int h) {
  float scale = ui_scale(w, h);
  int row_h = (int)(74.0f * scale + 0.5f);
  int gap_x = (int)(40.0f * scale + 0.5f);
  int top = panel->y + (int)(128.0f * scale + 0.5f);
  if (row_h < 48) row_h = 48;
  if (gap_x < 24) gap_x = 24;
  SDL_Rect rect = {
    panel->x + gap_x,
    top + row * row_h,
    panel->w - gap_x * 2,
    row_h - 8,
  };
  return rect;
}

static int option_scroll_for(int selected) {
  int max_scroll = NUM_OPTION_ROWS - OPT_VISIBLE_ROWS;
  int scroll = selected >= OPT_VISIBLE_ROWS ?
               selected - OPT_VISIBLE_ROWS + 1 : 0;
  if (scroll > max_scroll) scroll = max_scroll;
  if (scroll < 0) scroll = 0;
  return scroll;
}

static void toggle_option(int row) {
  int *value = option_value(row);
  if (!value)
    return;
  *value = !*value;
  write_config(CONFIG_NAME);
}

static void adjust_option(int row, int direction) {
  if (row == 0)
    step_auto_boot_delay(direction);
  else
    toggle_option(row);
}

static void draw_options_button(SDL_Renderer *ren, int w, int h, int active) {
  float scale = ui_scale(w, h);
  SDL_Rect rect = options_button_rect(w, h);
  SDL_SetRenderDrawColor(ren, 20, 20, 28, 220);
  SDL_RenderFillRect(ren, &rect);
  draw_outline(ren, &rect, active ? 4 : 2,
               active ? 255 : 120, active ? 170 : 120,
               active ? 0 : 125, 235);
  draw_centered_text(ren, rect.x + rect.w / 2,
                     rect.y + (rect.h - (int)(FONT_CELL_H * scale)) / 2,
                     scale, "OPTIONS", 235, 235, 220, 255);
}

static void draw_toggle(SDL_Renderer *ren, const SDL_Rect *box, int enabled) {
  SDL_SetRenderDrawColor(ren, 14, 14, 18, 240);
  SDL_RenderFillRect(ren, box);
  draw_outline(ren, box, 2, 210, 210, 205, 230);
  if (enabled) {
    SDL_Rect fill = {
      box->x + box->w / 4,
      box->y + box->h / 4,
      box->w / 2,
      box->h / 2,
    };
    SDL_SetRenderDrawColor(ren, 255, 170, 0, 255);
    SDL_RenderFillRect(ren, &fill);
  }
}

static void draw_options(SDL_Renderer *ren, int w, int h, int selected) {
  float scale = ui_scale(w, h);
  float small_scale = scale * 0.82f;
  int box = (int)(30.0f * scale + 0.5f);
  int scroll = option_scroll_for(selected);
  SDL_Rect full = { 0, 0, w, h };
  SDL_Rect panel = options_panel_rect(w, h);
  char range[16];
  if (box < 22) box = 22;

  SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
  SDL_RenderFillRect(ren, &full);
  SDL_SetRenderDrawColor(ren, 14, 14, 20, 245);
  SDL_RenderFillRect(ren, &panel);
  draw_outline(ren, &panel, 3, 255, 170, 0, 235);
  draw_centered_text(ren, panel.x + panel.w / 2,
                     panel.y + (int)(38.0f * scale + 0.5f),
                     scale, "OPTIONS", 255, 205, 80, 255);

  snprintf(range, sizeof(range), "%d-%d/%d", scroll + 1,
           scroll + OPT_VISIBLE_ROWS < NUM_OPTION_ROWS ?
           scroll + OPT_VISIBLE_ROWS : NUM_OPTION_ROWS,
           NUM_OPTION_ROWS);
  draw_text(ren, panel.x + panel.w - text_width(range, small_scale) -
             (int)(28.0f * scale + 0.5f),
            panel.y + (int)(46.0f * scale + 0.5f), small_scale,
            range, 135, 135, 145, 255);

  for (int vr = 0; vr < OPT_VISIBLE_ROWS; vr++) {
    int i = scroll + vr;
    if (i >= NUM_OPTION_ROWS)
      break;
    SDL_Rect row = option_row_rect(&panel, vr, w, h);
    int active = i == selected;
    int *value = option_value(i);
    int enabled = value ? *value : 0;

    if (active) {
      SDL_SetRenderDrawColor(ren, 52, 38, 22, 220);
      SDL_RenderFillRect(ren, &row);
      draw_outline(ren, &row, 2, 255, 170, 0, 235);
    }

    draw_text(ren, row.x + (int)(20.0f * scale + 0.5f),
              row.y + (row.h - (int)(FONT_CELL_H * small_scale)) / 2,
              small_scale, option_labels[i], 230, 230, 220, 255);

    if (i == 0) {
      char delay[16];
      snprintf(delay, sizeof(delay), "%d SEC", config.auto_boot_delay);
      int value_w = text_width(delay, small_scale) +
                    (int)(32.0f * scale + 0.5f);
      SDL_Rect value_box = {
        row.x + row.w - value_w - (int)(20.0f * scale + 0.5f),
        row.y + (row.h - box) / 2,
        value_w,
        box,
      };
      SDL_SetRenderDrawColor(ren, 20, 20, 28, 230);
      SDL_RenderFillRect(ren, &value_box);
      draw_outline(ren, &value_box, 2, 180, 180, 175, 220);
      draw_centered_text(ren, value_box.x + value_box.w / 2,
                         value_box.y +
                         (value_box.h - (int)(FONT_CELL_H * small_scale)) / 2,
                         small_scale, delay, 235, 235, 225, 255);
    } else {
      SDL_Rect cb = {
        row.x + row.w - box - (int)(82.0f * scale + 0.5f),
        row.y + (row.h - box) / 2,
        box,
        box,
      };
      draw_toggle(ren, &cb, enabled);
      draw_text(ren, cb.x + cb.w + (int)(14.0f * scale + 0.5f),
                row.y + (row.h - (int)(FONT_CELL_H * small_scale)) / 2,
                small_scale, enabled ? "ON" : "OFF",
                enabled ? 255 : 150, enabled ? 205 : 150,
                enabled ? 80 : 155, 255);
    }
  }

  draw_centered_text(ren, panel.x + panel.w / 2,
                     panel.y + panel.h - (int)(36.0f * scale + 0.5f),
                     small_scale, "A TOGGLE   B BACK",
                     150, 150, 160, 255);
}

static void draw_main(SDL_Renderer *ren, SDL_Texture *cover, int w, int h,
                      int selected, float pulse_time, bool auto_boot_active,
                      unsigned auto_boot_seconds) {
  float t = pulse_time / PULSE_PERIOD;
  float pulse = 1.0f + (PULSE_MAX_SCALE - 1.0f) * 0.5f *
                (1.0f - cosf(t * 6.28318530718f));
  float scale = ui_scale(w, h);
  float small_scale = scale * 0.78f;
  SDL_Rect base = cover_rect(w, h);
  SDL_Rect dst = base;

  SDL_SetRenderDrawColor(ren, 10, 10, 15, 255);
  SDL_RenderClear(ren);

  if (selected == 0) {
    dst.w = (int)(base.w * pulse);
    dst.h = (int)(base.h * pulse);
    dst.x = base.x + (base.w - dst.w) / 2;
    dst.y = base.y + (base.h - dst.h) / 2;
    draw_outline(ren, &dst, 4, 255, 170, 0, 235);
  }

  if (cover) {
    unsigned char mod = selected == 0 ? 255 : 165;
    SDL_SetTextureColorMod(cover, mod, mod, mod);
    SDL_RenderCopy(ren, cover, NULL, &dst);
  } else {
    SDL_SetRenderDrawColor(ren, 24, 24, 32, 255);
    SDL_RenderFillRect(ren, &dst);
    draw_centered_text(ren, dst.x + dst.w / 2,
                       dst.y + dst.h / 2 - (int)(24.0f * scale),
                       small_scale, "GRAND THEFT AUTO",
                       235, 235, 225, 255);
    draw_centered_text(ren, dst.x + dst.w / 2,
                       dst.y + dst.h / 2 + (int)(12.0f * scale),
                       small_scale, "SAN ANDREAS", 255, 170, 0, 255);
  }

  draw_options_button(ren, w, h, selected == 1);
  if (auto_boot_active) {
    char countdown[64];
    snprintf(countdown, sizeof(countdown),
             "AUTO BOOT IN %u   PRESS ANY BUTTON TO CANCEL",
             auto_boot_seconds);
    draw_centered_text(ren, w / 2,
                       h - (int)(42.0f * scale + 0.5f),
                       small_scale, countdown, 255, 185, 80, 255);
  } else {
    draw_centered_text(ren, w / 2,
                       h - (int)(42.0f * scale + 0.5f),
                       small_scale, "A LAUNCH   UP OPTIONS   + EXIT",
                       155, 155, 165, 255);
  }
}

int settings_menu_run(void) {
  int result = 1;
  int win_w = appletGetOperationMode() == AppletOperationMode_Console ? 1920 : 1280;
  int win_h = appletGetOperationMode() == AppletOperationMode_Console ? 1080 : 720;

  if (!valid_auto_boot_delay(config.auto_boot_delay))
    config.auto_boot_delay = 3;

  if (R_FAILED(romfsInit()))
    return 1;
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_JOYSTICK |
               SDL_INIT_GAMECONTROLLER) != 0)
    goto out_romfs;
  if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG))
    goto out_sdl;

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
  SDL_Window *win = SDL_CreateWindow("GTA San Andreas",
                                     SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED,
                                     win_w, win_h, SDL_WINDOW_FULLSCREEN);
  if (!win)
    goto out_img;
  SDL_Renderer *ren = SDL_CreateRenderer(
      win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren)
    goto out_win;
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  int screen_w = win_w;
  int screen_h = win_h;
  SDL_GetRendererOutputSize(ren, &screen_w, &screen_h);
  if (screen_w <= 0 || screen_h <= 0) {
    screen_w = win_w;
    screen_h = win_h;
  }

  font_texture = create_font_texture(ren);
  SDL_Texture *cover = NULL;
  SDL_Surface *surface = IMG_Load("romfs:/gtasa.png");
  if (surface) {
    cover = SDL_CreateTextureFromSurface(ren, surface);
    SDL_FreeSurface(surface);
  }

  SDL_GameController *ctrl = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      ctrl = SDL_GameControllerOpen(i);
      if (ctrl)
        break;
    }
  }

  LauncherView view = LAUNCHER_MAIN;
  int selected = 0;
  int option_selected = 0;
  float pulse_time = 0.0f;
  Uint32 prev_ms = SDL_GetTicks();
  bool running = true;
  bool stick_moved_x = false;
  bool stick_moved_y = false;
  bool auto_boot_active = true;
  Uint32 auto_boot_deadline = prev_ms + (Uint32)config.auto_boot_delay * 1000;

  while (running && appletMainLoop()) {
    Uint32 now_ms = SDL_GetTicks();
    pulse_time += (now_ms - prev_ms) * 0.001f;
    prev_ms = now_ms;
    if (pulse_time >= PULSE_PERIOD)
      pulse_time -= PULSE_PERIOD;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      switch (ev.type) {
      case SDL_QUIT:
        result = 0;
        running = false;
        break;
      case SDL_CONTROLLERBUTTONDOWN:
        auto_boot_active = false;
        switch (ev.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
          if (view == LAUNCHER_MAIN) selected = 1;
          else option_selected = (option_selected + NUM_OPTION_ROWS - 1) % NUM_OPTION_ROWS;
          pulse_time = 0.0f;
          break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
          if (view == LAUNCHER_MAIN) selected = 0;
          else option_selected = (option_selected + 1) % NUM_OPTION_ROWS;
          pulse_time = 0.0f;
          break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
          if (view == LAUNCHER_MAIN) selected = 0;
          else adjust_option(option_selected, -1);
          pulse_time = 0.0f;
          break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
          if (view == LAUNCHER_MAIN) selected = 1;
          else adjust_option(option_selected, 1);
          pulse_time = 0.0f;
          break;
        case SDL_CONTROLLER_BUTTON_B:
          if (view == LAUNCHER_MAIN && selected == 1) {
            view = LAUNCHER_OPTIONS;
            option_selected = 0;
          } else if (view == LAUNCHER_MAIN) {
            result = 1;
            running = false;
          } else {
            adjust_option(option_selected, 1);
          }
          break;
        case SDL_CONTROLLER_BUTTON_A:
          if (view == LAUNCHER_OPTIONS) {
            view = LAUNCHER_MAIN;
            selected = 0;
          } else {
            result = 0;
            running = false;
          }
          break;
        case SDL_CONTROLLER_BUTTON_START:
          if (view == LAUNCHER_OPTIONS) {
            view = LAUNCHER_MAIN;
            selected = 0;
          } else {
            result = 0;
            running = false;
          }
          break;
        }
        break;
      case SDL_CONTROLLERAXISMOTION:
        if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) {
          if (!stick_moved_x && ev.caxis.value < -STICK_THRESHOLD) {
            stick_moved_x = true;
            auto_boot_active = false;
            if (view == LAUNCHER_MAIN) selected = 0;
            else adjust_option(option_selected, -1);
            pulse_time = 0.0f;
          } else if (!stick_moved_x && ev.caxis.value > STICK_THRESHOLD) {
            stick_moved_x = true;
            auto_boot_active = false;
            if (view == LAUNCHER_MAIN) selected = 1;
            else adjust_option(option_selected, 1);
            pulse_time = 0.0f;
          } else if (ev.caxis.value > -STICK_DEADZONE &&
                     ev.caxis.value < STICK_DEADZONE) {
            stick_moved_x = false;
          }
        } else if (ev.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
          if (!stick_moved_y && ev.caxis.value < -STICK_THRESHOLD) {
            stick_moved_y = true;
            auto_boot_active = false;
            if (view == LAUNCHER_MAIN) selected = 1;
            else option_selected = (option_selected + NUM_OPTION_ROWS - 1) % NUM_OPTION_ROWS;
            pulse_time = 0.0f;
          } else if (!stick_moved_y && ev.caxis.value > STICK_THRESHOLD) {
            stick_moved_y = true;
            auto_boot_active = false;
            if (view == LAUNCHER_MAIN) selected = 0;
            else option_selected = (option_selected + 1) % NUM_OPTION_ROWS;
            pulse_time = 0.0f;
          } else if (ev.caxis.value > -STICK_DEADZONE &&
                     ev.caxis.value < STICK_DEADZONE) {
            stick_moved_y = false;
          }
        }
        break;
      case SDL_FINGERDOWN: {
        auto_boot_active = false;
        int tx = (int)(ev.tfinger.x * screen_w);
        int ty = (int)(ev.tfinger.y * screen_h);
        if (view == LAUNCHER_MAIN) {
          SDL_Rect options = options_button_rect(screen_w, screen_h);
          SDL_Rect cover_hit = cover_rect(screen_w, screen_h);
          if (rect_contains(&options, tx, ty)) {
            selected = 1;
            view = LAUNCHER_OPTIONS;
            option_selected = 0;
          } else if (rect_contains(&cover_hit, tx, ty)) {
            result = 1;
            running = false;
          }
        } else {
          SDL_Rect panel = options_panel_rect(screen_w, screen_h);
          int scroll = option_scroll_for(option_selected);
          int handled = 0;
          for (int vr = 0; vr < OPT_VISIBLE_ROWS; vr++) {
            int i = scroll + vr;
            if (i >= NUM_OPTION_ROWS)
              break;
            SDL_Rect row = option_row_rect(&panel, vr, screen_w, screen_h);
            if (rect_contains(&row, tx, ty)) {
              option_selected = i;
              adjust_option(i, 1);
              handled = 1;
              break;
            }
          }
          if (!handled && !rect_contains(&panel, tx, ty)) {
            view = LAUNCHER_MAIN;
            selected = 0;
          }
        }
        break;
      }
      case SDL_KEYDOWN:
        auto_boot_active = false;
        break;
      }
    }

    now_ms = SDL_GetTicks();
    if (auto_boot_active && (Sint32)(now_ms - auto_boot_deadline) >= 0) {
      result = 1;
      running = false;
      auto_boot_active = false;
    }
    unsigned auto_boot_seconds = auto_boot_active ?
        (unsigned)((auto_boot_deadline - now_ms + 999) / 1000) : 0;
    draw_main(ren, cover, screen_w, screen_h, selected, pulse_time,
              auto_boot_active, auto_boot_seconds);
    if (view == LAUNCHER_OPTIONS)
      draw_options(ren, screen_w, screen_h, option_selected);
    SDL_RenderPresent(ren);
  }

  if (!running)
    write_config(CONFIG_NAME);
  else
    result = 0;

  if (ctrl)
    SDL_GameControllerClose(ctrl);
  if (cover)
    SDL_DestroyTexture(cover);
  if (font_texture) {
    SDL_DestroyTexture(font_texture);
    font_texture = NULL;
  }
  SDL_DestroyRenderer(ren);
out_win:
  SDL_DestroyWindow(win);
out_img:
  IMG_Quit();
out_sdl:
  SDL_Quit();
out_romfs:
  romfsExit();
  return result;
}
