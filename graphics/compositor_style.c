/*
 * kernel/graphics/compositor_style.c
 *
 * Transitional compatibility state for legacy style/theme syscalls.
 *
 * Ordinary appearance policy belongs to userland. The remaining kernel window
 * compatibility path still consumes the active theme and title-bar height, so
 * those values remain here until that path is cut over to resource/object
 * mechanisms.
 */
#include <kernel/compositor_style.h>
/* Bootstrap/legacy kernel compositor must build without the userland SDK.
 * This immutable snapshot mirrors the SDK Material design tokens; the host
 * parity regression checks all copied values. The modern TTY palette itself
 * remains exclusively in userland (nxttyd + libos1). */

/* Built-in styles, indexed by STYLE_* ids. */
static const compositor_style_t styles[STYLE_COUNT] = {

    /* STYLE_NEXS — */
    [STYLE_NEXS] = {.titlebar = 1,
                    .window_borders = 0,
                    .rounded_corners = 1,
                    .shadows = 1,
                    .animations = 0,
                    .titlebar_height = 24,
                    .border_radius = 10,
                    .button_shape = 2,
                    .button_side = 1,
                    .shadow_size = 6,
                    .shadow_type = 1},
    /* STYLE_CLASSIC — X11/Motif: square, no shadow . */
    [STYLE_CLASSIC] = {.titlebar = 1,
                       .window_borders = 1,
                       .rounded_corners = 1,
                       .shadows = 1,
                       .animations = 0,
                       .titlebar_height = 20,
                       .border_radius = 10,
                       .button_shape = 0,
                       .button_side = 1,
                       .shadow_size = 4,
                       .shadow_type = 1},
    /* STYLE_MATERIAL — Android/Material 3: rounded, soft shadow, animations. */
    [STYLE_MATERIAL] = {.titlebar = 1,
                        .window_borders = 1,
                        .rounded_corners = 1,
                        .shadows = 1,
                        .animations = 1,
                        .titlebar_height = 24,
                        .border_radius = 12,
                        .button_shape = 2,
                        .button_side = 1,
                        .shadow_size = 6,
                        .shadow_type = 2},
    /* STYLE_GLASS — macOS/Win11: rounded, thin border, blur (future). */
    [STYLE_GLASS] = {.titlebar = 1,
                     .window_borders = 0,
                     .rounded_corners = 1,
                     .shadows = 1,
                     .animations = 1,
                     .titlebar_height = 22,
                     .border_radius = 10,
                     .button_shape = 0,
                     .button_side = 0,
                     .shadow_size = 6,
                     .shadow_type = 2},
    /* STYLE_MINIMAL — */
    [STYLE_MINIMAL] = {.titlebar = 1,
                       .window_borders = 0,
                       .rounded_corners = 1,
                       .shadows = 0,
                       .animations = 0,
                       .titlebar_height = 20,
                       .border_radius = 10,
                       .button_shape = 2,
                       .button_side = 1,
                       .shadow_size = 0,
                       .shadow_type = 0},
    /* STYLE_RETRO — CDE/Win95: bold square borders. */
    [STYLE_RETRO] = {.titlebar = 1,
                     .window_borders = 1,
                     .rounded_corners = 0,
                     .shadows = 1,
                     .animations = 0,
                     .titlebar_height = 20,
                     .border_radius = 10,
                     .button_shape = 1,
                     .button_side = 1,
                     .shadow_size = 4,
                     .shadow_type = 0},

};

/* Built-in themes, indexed by THEME_* ids. */
static const compositor_theme_t themes[THEME_COUNT] = {
    /* THEME_LIGHT — the existing macOS-style palette (default; preserves the
     * current appearance exactly). */
    [THEME_LIGHT] = {.win_bg = 0xFFF5F3EFU,
                     .title_active = 0xFFEBE9E4,
                     .title_inactive = 0xFFE3E0DA,
                     .title_text_active = 0xFF303238U,
                     .title_text_inactive = 0xFF8E8E93,
                     .close_btn = 0xFFFF5F57,
                     .border = 0xFFD1D1D6,
                     .accent = 0xFF007AFF},
    /* THEME_DARK — dark palette. */
    [THEME_DARK] = {.win_bg = 0xFF25272CU,
                    .title_active = 0xFF303238,
                    .title_inactive = 0xFF282A30,
                    .title_text_active = 0xFFE8E6E0U,
                    .title_text_inactive = 0xFF8E8E93,
                    .close_btn = 0xFFFF5F57,
                    .border = 0xFF3A3A3C,
                    .accent = 0xFF0A84FF},
};

static int active_style = STYLE_MINIMAL;
static int active_theme = THEME_DARK;
static int active_background = BG_BLUE;

const compositor_theme_t *compositor_theme_active(void) {
  return &themes[active_theme];
}

int compositor_titlebar_height(void) {
  const compositor_style_t *s = &styles[active_style];
  return s->titlebar ? s->titlebar_height : 0;
}

int compositor_set_style(int style_id) {
  if (style_id < 0 || style_id >= STYLE_COUNT)
    return -1;
  active_style = style_id;
  compositor_full_damage(); /* re-render with the new chrome */
  return 0;
}

int compositor_set_theme(int theme_id) {
  if (theme_id < 0 || theme_id >= THEME_COUNT)
    return -1;
  active_theme = theme_id;
  compositor_full_damage();
  return 0;
}

int compositor_set_background(int bg_id) {
  if (bg_id < 0 || bg_id >= BG_COUNT)
    return -1;
  active_background = bg_id;
  compositor_full_damage();
  return 0;
}
