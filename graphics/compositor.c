/*
 * kernel/graphics/compositor.c
 *
 * Transitional legacy window compatibility state.
 *
 * Ordinary frame composition, desktop rendering, pointer policy and chrome
 * belong to supervised userland. This file still contains compatibility
 * window storage and legacy syscall/object adapters that have not yet been
 * cut over to the resource/object plane. It must not regain a desktop
 * renderer or duplicate userland policy.
 *
 * Active code in this file follows the kernel hardening rules: bounded loops,
 * explicit input validation, checked allocation/copy results, no recursion or
 * goto, and short lock-held critical sections. Historical renderer-specific
 * compliance claims are intentionally not retained here after their code was
 * removed.
 */

#include <kernel/resource.h>
#include <drivers/gpu/gpu.h>
#include <drivers/timer.h> /* mono_ns: motion-event rate limiting */
#include <drivers/virtio_input.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/compositor_style.h>
#include <kernel/cpu.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/term.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <os1_compositor.h>
#include <surface.h>
#include <object.h> /* struct window_info, WININFO_* — windows as objects (§6.7) */
#include <stdint.h>

#define MAX_WINDOWS 40

/*
 * GFX-COMP-RESERVE-02: MAX_WINDOWS is one flat pool shared by every caller.
 * A stress test that opens ordinary user windows up to window_count ==
 * MAX_WINDOWS starves compositor_create_window() for EVERYONE, including a
 * supervised system service (nxui/dock, nxbar, the launcher) that init just
 * respawned after a crash — its create_window() call fails exactly like an
 * unprivileged app's would, and the service exits again (see nxui.c/nxbar.c
 * main(): `if (g_win < 0) return 1;`).  init's backoff queue (USR-INIT-04)
 * keeps retrying it, but it can never succeed until user windows close back
 * below the cap — a stress test can hold the desktop chrome down indefinitely.
 *
 * Fix: reserve the last RESERVED_SYSTEM_SLOTS pool entries for PRIVILEGED
 * callers only (PLVL_ROOT/PLVL_MACHINE — proc_is_privileged(), same check
 * process.c/object.c already gate other root-only operations on).  Ordinary
 * PLVL_USER/PLVL_GUEST windows are capped at MAX_WINDOWS -
 * RESERVED_SYSTEM_SLOTS; a privileged caller may use the full pool.  Sized
 * for the known supervised /sys/bin singletons (dock, nxbar, launcher) plus
 * one spare so a destroy+recreate resize (nxui's dock_reinit / nxbar's
 * set_window_height) never has to land in the exact instant a slot is free
 * to avoid double-booking itself.
 */
#define RESERVED_SYSTEM_SLOTS 8
#define MAX_USER_WINDOWS (MAX_WINDOWS - RESERVED_SYSTEM_SLOTS)

/*
 * NASA/JPL "Power of 10" rule 5 (assertion density): make the relationship
 * between the two pool-sizing constants a checkable invariant instead of an
 * implicit assumption.  A future edit that raises RESERVED_SYSTEM_SLOTS above
 * MAX_WINDOWS (or to equal it) would silently give MAX_USER_WINDOWS a
 * negative or zero value — window_count >= cap would then be true from boot,
 * permanently locking out every ordinary user window with no diagnostic.
 * Catching this at COMPILE time (not boot time) means it can never ship.
 */
_Static_assert(RESERVED_SYSTEM_SLOTS > 0 && RESERVED_SYSTEM_SLOTS < MAX_WINDOWS,
               "RESERVED_SYSTEM_SLOTS must leave a non-empty user window pool");

/* Desktop */
/* ========================================================================= */
/* Desktop background                                                        */
/* ========================================================================= */

#define COLOR_BG_TOP 0xFFF2F2F7
#define COLOR_BG_BOTTOM 0xFFECEFF1

/* ========================================================================= */
/* Window                                                                    */
/* ========================================================================= */

#define COLOR_WIN_BG 0xFFFCFCFD

/* ========================================================================= */
/* Title bar                                                                 */
/* ========================================================================= */

#define COLOR_TITLE_ACTIVE 0xFFEFEFF4
#define COLOR_TITLE_INACTIVE 0xFFE5E5EA

#define COLOR_TITLE_TEXT_ACTIVE 0xFF000000
#define COLOR_TITLE_TEXT_INACTIVE 0xFF8E8E93

/* macOS close button */
#define COLOR_CLOSE_BTN 0xFFFF5F57
/* macOS-style minimize/background button (yellow), drawn left of the close
 * button.  NOTE(GFX-WIN-THEME): a conventional fixed colour for now; threading
 * it through compositor_theme_t is a non-blocking follow-up. */
#define COLOR_MIN_BTN 0xFFFEBC2E

/* ========================================================================= */
/* Text                                                                       */
/* ========================================================================= */

/* Compatibility text windows share the ACTIVE palette with the native
 * window theme, rather than assuming a dark glyph on a light surface. */
#define COLOR_FG (compositor_theme_active()->title_text_active)
#define COLOR_FG_SECONDARY 0xFF757575
#define COLOR_FG_DISABLED 0xFFBDBDBD

/* ========================================================================= */
/* Caret / selection                                                          */
/* ========================================================================= */

#define COLOR_CARET 0x40007AFF
#define COLOR_SELECTION 0x40007AFF
#define COLOR_SELECTION_ACTIVE 0xFF007AFF

/* ========================================================================= */
/* Borders                                                                    */
/* ========================================================================= */

#define COLOR_BORDER 0xFFD1D1D6
#define COLOR_BORDER_LIGHT 0xFFE5E5EA
#define COLOR_BORDER_DARK 0xFFC7C7CC

/* ========================================================================= */
/* Buttons                                                                    */
/* ========================================================================= */

#define COLOR_BUTTON_BG 0xFFFFFFFF
#define COLOR_BUTTON_HOVER 0xFFF5F5F5
#define COLOR_BUTTON_PRESSED 0xFFE0E0E0

#define COLOR_BUTTON_TEXT 0xFF000000
#define COLOR_BUTTON_DISABLED 0xFFFAFAFA

/* ========================================================================= */
/* Input fields                                                               */
/* ========================================================================= */

#define COLOR_INPUT_BG 0xFFFFFFFF
#define COLOR_INPUT_BORDER 0xFFD1D1D6
#define COLOR_INPUT_BORDER_ACTIVE 0xFF007AFF

/* ========================================================================= */
/* Menus */
/* ========================================================================= */

#define COLOR_MENU_BG 0xFFFFFFFF
#define COLOR_MENU_HOVER 0xFFF5F5F5
#define COLOR_MENU_SELECTED 0xFFE3F2FD

/* ========================================================================= */
/* Scrollbars */
/* ========================================================================= */

#define COLOR_SCROLL_TRACK 0xFFF2F2F7
#define COLOR_SCROLL_THUMB 0xFFC7C7CC
#define COLOR_SCROLL_THUMB_HOVER 0xFF8E8E93

/* ========================================================================= */
/* Tooltip */
/* ========================================================================= */

#define COLOR_TOOLTIP_BG 0xFF212121
#define COLOR_TOOLTIP_TEXT 0xFFFFFFFF

/* ========================================================================= */
/* Shadows */
/* ========================================================================= */

#define COLOR_SHADOW 0x20000000
#define COLOR_SHADOW_STRONG 0x40000000

/* ========================================================================= */
/* Status colors */
/* ========================================================================= */

#define COLOR_SUCCESS 0xFF34C759
#define COLOR_WARNING 0xFFFF9500
#define COLOR_ERROR 0xFFFF3B30
#define COLOR_INFO 0xFF007AFF

/* ========================================================================= */
/* Terminal colors */
/* ========================================================================= */

#define COLOR_TERM_BLACK 0xFF1C1C1E
#define COLOR_TERM_RED 0xFFFF3B30
#define COLOR_TERM_GREEN 0xFF34C759
#define COLOR_TERM_YELLOW 0xFFFFCC00
#define COLOR_TERM_BLUE 0xFF007AFF
#define COLOR_TERM_MAGENTA 0xFFAF52DE
#define COLOR_TERM_CYAN 0xFF5AC8FA
#define COLOR_TERM_WHITE 0xFFF2F2F7

#define COLOR_TERM_BRIGHT_BLACK 0xFF8E8E93
#define COLOR_TERM_BRIGHT_RED 0xFFFF6961
#define COLOR_TERM_BRIGHT_GREEN 0xFF30D158
#define COLOR_TERM_BRIGHT_YELLOW 0xFFFFD60A
#define COLOR_TERM_BRIGHT_BLUE 0xFF409CFF
#define COLOR_TERM_BRIGHT_MAGENTA 0xFFBF5AF2
#define COLOR_TERM_BRIGHT_CYAN 0xFF64D2FF
#define COLOR_TERM_BRIGHT_WHITE 0xFFFFFFFF

struct window {
  int id;
  int x, y;
  int width, height; /* LOGICAL surface size (the app's pixel buffer). */
  /* ON-SCREEN size the compositor draws the surface at (GFX-DYN-01 surface
   * model).  When draw_w/draw_h differ from width/height the compositor scales
   * the logical surface to the draw rect (the app may keep drawing at its old
   * size while it reallocates → fluid resize).  0 means "same as logical". */
  int draw_w, draw_h;
  int presentation_managed; /* Explicit WM extent survives delayed app adoption. */
  int requested_draw_w, requested_draw_h; /* Restored after workspace expands. */
  uint32_t layout_anchor, layout_layer, layout_order;
  uint32_t layout_margin, layout_reserved;
  int layout_registered; /* Registration is tied to the window/PID lifetime. */
  int z_order;
  int visible;
  int minimized; /* If true, sent to background by the user/WM (dock-
                    restorable).  A minimized window has visible==0 but
                    stays in the table so the dock can restore it; this
                    distinguishes it from a window an app hid itself. */
  int pid;
  int protected;     /* If true, cannot be closed */
  int top_most;      /* If true, always on top and no decorations */
  int passive;       /* If true, click-through: never focused, never hit-tested
                        for input (system popups e.g. notifications). */
  uint32_t *buffer;  /* Window's pixel buffer */
  uint32_t bg_color; /* Default background color */
  char title[64];
  int radius;
  int has_rounded_corners;

  /* Terminal State — VT/ANSI emulator extracted to term.c (#123). */
  struct terminal term;

  /* Compositor flags */
  int has_alpha; /* Se 1, contiene trasparenze e non occlude i layer inferiori
                  */
};

static int window_pixel_count(int width, int height, size_t *count_out) {
  if (!count_out || width <= 0 || height <= 0)
    return -1;
  size_t width_sz = (size_t)width;
  size_t height_sz = (size_t)height;
  if (width_sz > (size_t)-1 / height_sz)
    return -1;
  *count_out = width_sz * height_sz;
  return 0;
}

/* Global State */
static struct window windows[MAX_WINDOWS];
static int window_count = 0;
static int next_window_id = 100;
static volatile int compositor_dirty = 1;
static DEFINE_SPINLOCK(compositor_lock);

/* Damage rect: tracks the bounding box of pixels that need GPU upload */
static int damage_x1 = 0, damage_y1 = 0;
static int damage_x2 = 0, damage_y2 = 0;

/* Helper to expand damage region */
static void expand_damage(int x, int y, int w, int h) {
  if (x < damage_x1)
    damage_x1 = x;
  if (y < damage_y1)
    damage_y1 = y;
  if (x + w > damage_x2)
    damage_x2 = x + w;
  if (y + h > damage_y2)
    damage_y2 = y + h;
  compositor_dirty = 1;
  nx_surface_changed();
}

static void expand_window_content_damage(struct window *win, int x, int y,
                                         int w, int h) {
  if (!win || w <= 0 || h <= 0)
    return;

  int dw = win->draw_w > 0 ? win->draw_w : win->width;
  int dh = win->draw_h > 0 ? win->draw_h : win->height;
  if (win->width <= 0 || win->height <= 0 || dw <= 0 || dh <= 0)
    return;

  int x1 = x;
  int y1 = y;
  int x2 = x + w;
  int y2 = y + h;

  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x2 > win->width)
    x2 = win->width;
  if (y2 > win->height)
    y2 = win->height;
  if (x2 <= x1 || y2 <= y1)
    return;

  int sx1 = win->x + (int)((int64_t)x1 * dw / win->width);
  int sy1 = win->y + (int)((int64_t)y1 * dh / win->height);
  int sx2 = win->x + (int)(((int64_t)x2 * dw + win->width - 1) / win->width);
  int sy2 = win->y + (int)(((int64_t)y2 * dh + win->height - 1) / win->height);
  expand_damage(sx1, sy1, sx2 - sx1, sy2 - sy1);
}

static int window_region_has_alpha(struct window *win, int x, int y, int w,
                                   int h) {
  if (!win || !win->buffer || w <= 0 || h <= 0)
    return 0;

  int x1 = x < 0 ? 0 : x;
  int y1 = y < 0 ? 0 : y;
  int x2 = x + w;
  int y2 = y + h;
  if (x2 > win->width)
    x2 = win->width;
  if (y2 > win->height)
    y2 = win->height;
  if (x2 <= x1 || y2 <= y1)
    return 0;

  if (win->width <= 0)
    return 0;
  size_t width_sz = (size_t)win->width;
  for (int py = y1; py < y2; py++) {
    uint32_t *row = win->buffer + (size_t)py * width_sz;
    for (int px = x1; px < x2; px++) {
      if ((row[px] >> 24) != 0xFF)
        return 1;
    }
  }
  return 0;
}

/* Title-bar height still comes from the legacy active style. The remaining
 * kernel compositor compatibility path no longer owns any chrome renderer. */

/* Display bounds support kernel window placement policy. Framebuffer pixels
 * and render surfaces belong to the ring-3 compositor. Geometry must come from
 * the registered display provider; the kernel never invents a desktop mode. */

static int bb_width = 0;
static int bb_height = 0;

/* Physical output geometry and logical desktop scale are independent.
 * native_w/native_h track the active or prepared scanout mode. bb_width/
 * bb_height remain the logical desktop used by window placement and input.
 * Ring-3 nxcompositor performs the final logical->physical scale. */
static int native_w = 0, native_h = 0;
static int desktop_zoom = OS1_DISPLAY_ZOOM_DEFAULT_PERCENT;

/* Single ownership boundary for the logical base policy. This record is
 * accessed only in critical sections protected by compositor_lock. It is
 * not a second compositor or a per-process desktop policy. */
struct workspace_policy_state {
  uint32_t mode;
  int base_w;
  int base_h;
};

static struct workspace_policy_state *workspace_policy(void) {
  static struct workspace_policy_state state = {
      OS1_DISPLAY_WORKSPACE_AUTO, 0, 0};
  return &state;
}

/*
 * Initialize Compositor
 *
 * FIX(COMPOSITOR-INIT-01): every field written here (windows[], window_count,
 * next_window_id, native_w/h, desktop_zoom, compositor_backbuffer, bb_pages,
 * bb_width, bb_height, damage_x1/y1/x2/y2) is also read and written under
 * compositor_lock everywhere else in this file (compositor_full_damage(),
 * the render/tick path, resize, etc.) — but this function used to write all
 * of it WITHOUT holding that lock.  Once local IRQs are enabled and any
 * other CPU/IRQ path can reach a compositor_lock-protected reader before
 * this function returns, that reader can observe a partially-initialised
 * state (e.g. bb_width/bb_height updated but compositor_backbuffer not yet,
 * or the reverse), which is exactly the kind of half-initialised global
 * state that produces a black/corrupt screen instead of a crash.
 *
 * FIX(COMPOSITOR-INIT-02, revised): a prior version of this fix took
 * compositor_lock (IRQs saved) around the ENTIRE function, including the
 * pmm_alloc_pages_dma() call, on the strength of a comment asserting that
 * this specific allocator path "never sleeps, never takes another lock,
 * never re-enters the scheduler". That claim is not something this file
 * can verify or enforce — pmm.c's allocator takes its own per-zone
 * spinlock internally (alloc_pages_impl), and holding a second spinlock
 * with IRQs off for the duration of an unbounded, size-dependent
 * allocation (plus, on the failure path, a pr_err() that itself takes
 * printk's locks) is exactly the shape of critical section this codebase
 * otherwise avoids everywhere else. The allocation is done here BEFORE
 * compositor_lock is taken at all: compositor_lock now protects only the
 * bookkeeping-field PUBLISH step, which is the actual thing
 * COMPOSITOR-INIT-01 needed atomic, and never overlaps with pmm's own
 * lock or with I/O. */
void compositor_init(void) {
  int w = 0, h = 0;
  if (gpu_get_display_info(&w, &h) != 0 || w <= 0 || h <= 0) {
    pr_err("%s", "Compositor: no valid display geometry; window manager unavailable\n");
    return;
  }

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;
  native_w = w;
  native_h = h;
  desktop_zoom = OS1_DISPLAY_ZOOM_DEFAULT_PERCENT;
  struct workspace_policy_state *policy = workspace_policy();
  policy->mode = OS1_DISPLAY_WORKSPACE_AUTO;
  policy->base_w = 0;
  policy->base_h = 0;
  bb_width = w;
  bb_height = h;
  spin_unlock_irqrestore(&compositor_lock, flags);

  pr_info("Compositor: Window manager initialized (%dx%d)\n", w, h);
}

/* Mark the whole desktop dirty (full repaint next tick).  Used by the
 * Style/Theme switch (compositor_style.c) so a new look shows immediately. */
void compositor_full_damage(void) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
  nx_surface_changed();
}

/* Report the current desktop (compositor backbuffer) size.  Backs
 * SYS_DISPLAY_INFO. */
void compositor_get_size(int *w, int *h) {
  /* Scanout and workspace may change concurrently on different CPUs.
   * Expose one atomic logical extent, never a torn w/h pair. */
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  if (w)
    *w = bb_width;
  if (h)
    *h = bb_height;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/* Edge reservations are declared by generic, capability-owned surface
 * plugins. No executable name, topmost heuristic or dock height enters the
 * kernel. A popup can extend beyond the strip it reserves. */
static int compositor_reserved_top_locked(int exclude_id) {
  int top = 0;
  for (int i = 0; i < MAX_WINDOWS; ++i) {
    const struct window *w = &windows[i];
    if (w->id == 0 || w->id == exclude_id || !w->visible ||
        !w->layout_registered ||
        w->layout_anchor != OS1_SURFACE_LAYOUT_ANCHOR_TOP) continue;
    int depth = (int)w->layout_margin + (int)w->layout_reserved;
    if (depth > top) top = depth;
  }
  return top;
}

static int compositor_reserved_bottom_locked(int exclude_id) {
  int bottom = 0;
  for (int i = 0; i < MAX_WINDOWS; ++i) {
    const struct window *w = &windows[i];
    if (w->id == 0 || w->id == exclude_id || !w->visible ||
        !w->layout_registered ||
        w->layout_anchor != OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM) continue;
    int depth = (int)w->layout_margin + (int)w->layout_reserved;
    if (depth > bottom) bottom = depth;
  }
  return bottom;
}

static void compositor_workarea_locked(int *top, int *bottom) {
  *top = compositor_reserved_top_locked(0);
  *bottom = compositor_reserved_bottom_locked(0);
  if (*top > bb_height - 1) *top = bb_height - 1;
  if (*bottom > bb_height - *top - 1)
    *bottom = bb_height - *top - 1;
}

static int compositor_virtual_size(int physical_w, int physical_h,
                                   int percent, int *virtual_w,
                                   int *virtual_h) {
  if (virtual_w == NULL || virtual_h == NULL || physical_w <= 0 ||
      physical_h <= 0 || percent < OS1_DISPLAY_ZOOM_MIN_PERCENT ||
      percent > OS1_DISPLAY_ZOOM_MAX_PERCENT)
    return -EINVAL;
  int64_t width =
      (int64_t)physical_w * OS1_DISPLAY_ZOOM_DEFAULT_PERCENT / percent;
  int64_t height =
      (int64_t)physical_h * OS1_DISPLAY_ZOOM_DEFAULT_PERCENT / percent;
  if (width < 1 || height < 1 || width > 8192 || height > 8192)
    return -ERANGE;
  *virtual_w = (int)width;
  *virtual_h = (int)height;
  return 0;
}

/* First pin the registered edge surfaces to the current logical desktop.
 * Their source framebuffer is left intact: a client can resize/redraw it
 * asynchronously without interrupting the current visible surface. */
static void compositor_anchor_plugins_locked(void) {
  for (int i = 0; i < MAX_WINDOWS; ++i) {
    struct window *win = &windows[i];
    if (win->id == 0 || !win->layout_registered) continue;
    int desired_w = win->requested_draw_w > 0 ? win->requested_draw_w : win->width;
    int desired_h = win->requested_draw_h > 0 ? win->requested_draw_h : win->height;
    win->draw_w = desired_w < bb_width ? desired_w : bb_width;
    int room = bb_height - (int)win->layout_margin;
    if (room < 1) room = 1;
    win->draw_h = desired_h < room ? desired_h : room;
    win->x = (bb_width - win->draw_w) / 2;
    if (win->layout_anchor == OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM)
      win->y = bb_height - (int)win->layout_margin - win->draw_h;
    else
      win->y = (int)win->layout_margin;
    if (win->y < 0) win->y = 0;
  }
}

/* Normal windows may be larger than their work area. In that case shrink
 * only the PRESENTATION extent; retain requested/source sizes for recovery.
 * The userland compositor scales the source and keeps titlebars on-screen. */
static void compositor_fit_application_locked(struct window *win,
                                               int top, int bottom) {
  int title = win->top_most ? 0 : compositor_titlebar_height();
  int room_w = bb_width;
  int room_h = bb_height - top - bottom - title;
  if (room_w < 1) room_w = 1;
  if (room_h < 1) room_h = 1;
  int desired_w = win->requested_draw_w > 0 ? win->requested_draw_w : win->width;
  int desired_h = win->requested_draw_h > 0 ? win->requested_draw_h : win->height;
  win->draw_w = desired_w < room_w ? desired_w : room_w;
  win->draw_h = desired_h < room_h ? desired_h : room_h;
  int xmax = bb_width - win->draw_w;
  int ymin = top + title;
  int ymax = bb_height - bottom - win->draw_h;
  if (ymax < ymin) ymax = ymin;
  if (win->x > xmax) win->x = xmax;
  if (win->x < 0) win->x = 0;
  if (win->y > ymax) win->y = ymax;
  if (win->y < ymin) win->y = ymin;
}

static void compositor_apply_bounds_locked(int w, int h) {
  bb_width = w;
  bb_height = h;
  compositor_anchor_plugins_locked();
  int top = 0, bottom = 0;
  compositor_workarea_locked(&top, &bottom);
  for (int i = 0; i < MAX_WINDOWS; ++i) {
    if (windows[i].id == 0 || windows[i].layout_registered) continue;
    compositor_fit_application_locked(&windows[i], top, bottom);
  }
  expand_damage(0, 0, bb_width, bb_height);
}

static int compositor_layout_valid(const struct os1_surface_layout_v1 *req) {
  if (req == NULL || req->abi_version != OS1_SURFACE_ABI_VERSION ||
      req->struct_size != sizeof(*req) || req->window_id <= 0 ||
      req->layer != OS1_SURFACE_LAYOUT_LAYER_OVERLAY ||
      req->anchor < OS1_SURFACE_LAYOUT_ANCHOR_TOP ||
      req->anchor > OS1_SURFACE_LAYOUT_ANCHOR_BOTTOM ||
      req->order > (uint32_t)MAX_WINDOWS || req->margin > 128U ||
      req->reserved == 0U || req->reserved > 2048U ||
      req->width == 0U || req->width > 4096U ||
      req->height == 0U || req->height > 4096U)
    return -EINVAL;
  return 0;
}

int compositor_layout_apply(const struct os1_surface_layout_v1 *req,
                            int caller_pid) {
  int valid = compositor_layout_valid(req);
  if (valid != 0) return valid;
  struct window_info owner;
  int found = compositor_window_info(req->window_id, &owner);
  if (found != 0) return found;
  if (caller_pid != owner.pid) return -EPERM;
  /* Reuse the existing checked surface reallocation path (outside the
   * short metadata lock) instead of duplicating a pixel allocator. */
  int resized = compositor_resize_window(req->window_id,
                                          (int)req->width, (int)req->height);
  if (resized != 0) return -ENOMEM;
  uint64_t flags;
  int status = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; ++i) {
    struct window *win = &windows[i];
    if (win->id != req->window_id || win->pid != caller_pid) continue;
    win->layout_registered = 1;
    win->layout_anchor = req->anchor;
    win->layout_layer = req->layer;
    win->layout_order = req->order;
    win->layout_margin = req->margin;
    win->layout_reserved = req->reserved;
    win->top_most = 1;
    compositor_apply_bounds_locked(bb_width, bb_height);
    status = 0;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return status;
}

int compositor_layout_query(struct os1_surface_layout_v1 *req) {
  if (req == NULL || req->abi_version != OS1_SURFACE_ABI_VERSION ||
      req->struct_size != sizeof(*req) || req->window_id < 0) return -EINVAL;
  int target = req->window_id, found = target == 0 ? 1 : 0;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  int top = 0, bottom = 0;
  compositor_workarea_locked(&top, &bottom);
  *req = (struct os1_surface_layout_v1){
      .abi_version = OS1_SURFACE_ABI_VERSION, .struct_size = sizeof(*req),
      .window_id = target, .work_x = 0, .work_y = top,
      .work_width = (uint32_t)bb_width,
      .work_height = (uint32_t)(bb_height - top - bottom)};
  for (int i = 0; i < MAX_WINDOWS && !found; ++i) {
    const struct window *win = &windows[i];
    if (win->id != target) continue;
    req->anchor = win->layout_anchor;
    req->layer = win->layout_layer;
    req->order = win->layout_order;
    req->margin = win->layout_margin;
    req->reserved = win->layout_reserved;
    req->width = (uint32_t)win->draw_w;
    req->height = (uint32_t)win->draw_h;
    req->x = win->x;
    req->y = win->y;
    found = 1;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return found ? 0 : -ESRCH;
}

/* Read-only snapshot for ROOT and USER inspection through object resources.
 * The exposed base is always the current physical extent in Auto mode. */
int compositor_workspace_query(struct os1_display_workspace_v1 *out) {
  if (out == NULL) return -EINVAL;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  const struct workspace_policy_state *policy = workspace_policy();
  *out = (struct os1_display_workspace_v1){
      .abi_version = OS1_SURFACE_ABI_VERSION, .struct_size = sizeof(*out),
      .mode = policy->mode, .zoom_percent = (uint32_t)desktop_zoom,
      .base_width = (uint32_t)(policy->mode == OS1_DISPLAY_WORKSPACE_AUTO
                                    ? native_w : policy->base_w),
      .base_height = (uint32_t)(policy->mode == OS1_DISPLAY_WORKSPACE_AUTO
                                     ? native_h : policy->base_h),
      .physical_width = (uint32_t)native_w,
      .physical_height = (uint32_t)native_h,
      .logical_width = (uint32_t)bb_width,
      .logical_height = (uint32_t)bb_height};
  spin_unlock_irqrestore(&compositor_lock, flags);
  return 0;
}

/* Apply an explicit ROOT workspace choice. No hardware mode changes occur
 * here, no per-executable privileges, and no duplicate userland renderer. */
int compositor_workspace_set(const struct os1_display_workspace_v1 *request) {
  if (request == NULL || request->abi_version != OS1_SURFACE_ABI_VERSION ||
      request->struct_size != sizeof(*request) ||
      request->mode > OS1_DISPLAY_WORKSPACE_MANUAL ||
      request->zoom_percent < OS1_DISPLAY_ZOOM_MIN_PERCENT ||
      request->zoom_percent > OS1_DISPLAY_ZOOM_MAX_PERCENT) return -EINVAL;
  if (request->mode == OS1_DISPLAY_WORKSPACE_AUTO &&
      (request->base_width != 0U || request->base_height != 0U))
    return -EINVAL;
  if (request->mode == OS1_DISPLAY_WORKSPACE_MANUAL &&
      (request->base_width < 64U || request->base_height < 64U ||
       request->base_width > 8192U || request->base_height > 8192U))
    return -ERANGE;
  uint64_t flags;
  int w = 0, h = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  int base_w = request->mode == OS1_DISPLAY_WORKSPACE_AUTO
                   ? native_w : (int)request->base_width;
  int base_h = request->mode == OS1_DISPLAY_WORKSPACE_AUTO
                   ? native_h : (int)request->base_height;
  int valid = compositor_virtual_size(base_w, base_h,
                                       (int)request->zoom_percent, &w, &h);
  if (valid == 0 && (size_t)w * (size_t)h > (16U * 1024U * 1024U))
    valid = -ERANGE;
  if (valid == 0) {
    struct workspace_policy_state *policy = workspace_policy();
    policy->mode = request->mode;
    policy->base_w = request->mode == OS1_DISPLAY_WORKSPACE_MANUAL ? base_w : 0;
    policy->base_h = request->mode == OS1_DISPLAY_WORKSPACE_MANUAL ? base_h : 0;
    desktop_zoom = (int)request->zoom_percent;
    if (w != bb_width || h != bb_height)
      compositor_apply_bounds_locked(w, h);
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (valid == 0) nx_surface_changed();
  return valid;
}

/* A physical mode change updates only the output reference. The logical
 * desktop keeps the selected UI scale and remains independent of scanout. */
void compositor_resize(int w, int h) {
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
    return;
  int logical_w = 0, logical_h = 0, changed = 0, zoom = 0;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  /* Surface reads poll the provider every frame. Only a transition is a
   * resize; publishing and logging unchanged dimensions saturates NXPaX/UART
   * and needlessly wakes the userland compositor. */
  if (native_w == w && native_h == h) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }
  native_w = w;
  native_h = h;
  zoom = desktop_zoom;
  const struct workspace_policy_state *policy = workspace_policy();
  int base_w = policy->mode == OS1_DISPLAY_WORKSPACE_AUTO ? w : policy->base_w;
  int base_h = policy->mode == OS1_DISPLAY_WORKSPACE_AUTO ? h : policy->base_h;
  if (compositor_virtual_size(base_w, base_h, zoom, &logical_w, &logical_h) == 0) {
    changed = logical_w != bb_width || logical_h != bb_height;
    if (changed)
      compositor_apply_bounds_locked(logical_w, logical_h);
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (changed)
    nx_surface_changed();
  pr_info("Compositor: scanout %dx%d, logical %dx%d, scale %d%%\n", w, h,
          logical_w, logical_h, zoom);
}

/* UI scale changes the logical desktop only. Ring-3 nxcompositor performs the
 * final logical-to-physical scale; the GPU scanout mode is never changed. */
int compositor_set_zoom(int percent) {
  if (percent < OS1_DISPLAY_ZOOM_MIN_PERCENT ||
      percent > OS1_DISPLAY_ZOOM_MAX_PERCENT)
    return -EINVAL;
  int logical_w = 0, logical_h = 0, changed = 0;
  int physical_w = 0, physical_h = 0;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  physical_w = native_w;
  physical_h = native_h;
  const struct workspace_policy_state *policy = workspace_policy();
  int base_w = policy->mode == OS1_DISPLAY_WORKSPACE_AUTO
                   ? physical_w : policy->base_w;
  int base_h = policy->mode == OS1_DISPLAY_WORKSPACE_AUTO
                   ? physical_h : policy->base_h;
  if (compositor_virtual_size(base_w, base_h, percent, &logical_w,
                              &logical_h) != 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -ENODEV;
  }
  desktop_zoom = percent;
  changed = logical_w != bb_width || logical_h != bb_height;
  if (changed)
    compositor_apply_bounds_locked(logical_w, logical_h);
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (changed)
    nx_surface_changed();
  pr_info("Compositor: UI scale %d%% -> logical %dx%d (scanout %dx%d)\n",
          percent, logical_w, logical_h, physical_w, physical_h);
  return 0;
}

/* Forward Declarations */
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid);

/*
 * Create Window
 */
/*
 * Interrupt Locking Helpers
 * Prevent nested interrupts by saving/restoring PSTATE.DAIF
 */
/* Interrupt Locking Helpers from cpu.h */

int compositor_create_window(int x, int y, int w, int h, const char *title,
                             struct process *owner) {
  if (!owner || !title ||
      __atomic_load_n(&owner->dying, __ATOMIC_ACQUIRE))
    return -EPERM;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return -EINVAL;

  int pid = (int)owner->pid;
  int privileged = proc_is_privileged(owner);
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w <= 0 || char_h <= 0)
    return -EINVAL;

  struct window fresh;
  memset(&fresh, 0, sizeof(fresh));
  size_t pixels = (size_t)w * (size_t)h;
  fresh.buffer = (uint32_t *)kmalloc(pixels * sizeof(uint32_t));
  if (!fresh.buffer)
    return -ENOMEM;

  uint32_t bg = compositor_theme_active()->win_bg;
  fresh.width = w;
  fresh.height = h;
  fresh.draw_w = w;
  fresh.draw_h = h;
  fresh.requested_draw_w = w;
  fresh.requested_draw_h = h;
  fresh.visible = 1;
  fresh.pid = pid;
  fresh.bg_color = bg;
  fresh.has_alpha = ((bg >> 24) != 0xFF);
  fresh.protected = (pid == 2);
  for (size_t i = 0; i < pixels; i++)
    fresh.buffer[i] = bg;

  int len = 0;
  while (title[len] && len < (int)sizeof(fresh.title) - 1) {
    fresh.title[len] = title[len];
    len++;
  }
  fresh.title[len] = '\0';

  if (term_init(&fresh.term, w / char_w, h / char_h, COLOR_FG, bg) != 0) {
    kfree(fresh.buffer);
    return -ENOMEM;
  }

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  if (__atomic_load_n(&owner->dying, __ATOMIC_ACQUIRE)) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    term_free(&fresh.term);
    kfree(fresh.buffer);
    return -EPERM;
  }

  int cap = privileged ? MAX_WINDOWS : MAX_USER_WINDOWS;
  int slot = -1;
  if (window_count < cap) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id == 0) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    term_free(&fresh.term);
    kfree(fresh.buffer);
    return -ENOSPC;
  }

  if (bb_width <= 0 || bb_height <= 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    term_free(&fresh.term);
    kfree(fresh.buffer);
    return -ENODEV;
  }
  int screen_w = bb_width;
  int screen_h = bb_height;
  if (x + w > screen_w)
    x = screen_w - w;
  if (x < 0)
    x = 0;
  int top = compositor_reserved_top_locked(0);
  int bottom = compositor_reserved_bottom_locked(0);
  if (y + h > screen_h - bottom)
    y = screen_h - bottom - h;
  if (y < top)
    y = top;

  int id = next_window_id++;
  fresh.id = id;
  fresh.x = x;
  fresh.y = y;
  fresh.z_order = window_count;
  windows[slot] = fresh;
  window_count++;
  compositor_apply_bounds_locked(bb_width, bb_height);
  expand_damage(x, y - compositor_titlebar_height(), w,
                h + compositor_titlebar_height());
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  pr_debug("Compositor: Created window '%s' (%dx%d) at (%d,%d)\n", title, w,
           h, x, y);
  return id;
}

/*
 * __focus_topmost_locked - re-point keyboard focus after a window goes away.
 *
 * The highest visible, focusable window receives input. An empty desktop
 * has no keyboard destination.
 *
 * Caller MUST hold compositor_lock; destroyed slots are already zeroed and
 * thus excluded by the id != 0 check.
 */
/* Erase the caret on every window not owned by keep_pid (caller holds
 * compositor_lock).  Keeps the caret on the input window only. */
static void __clear_other_carets_locked(int keep_pid);

static void __focus_topmost_locked(void) {
  int max_z = -1;
  int pid = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible && !windows[i].passive &&
        windows[i].z_order > max_z) {
      max_z = windows[i].z_order;
      pid = windows[i].pid;
    }
  }
  sched_set_focus_pid(pid); /* push the focus hint down (#67); never write the
                               scheduler's global directly */
  __clear_other_carets_locked(pid);
  pr_debug("Compositor: Focus reset to PID %d\n", pid);
}

/*
 * compositor_window_owner - return the owning PID of a window, or -1 if the
 * window id does not exist.  Used by the syscall layer for the ABI-04
 * ownership check on SYS_DESTROY_WINDOW (kernel-internal callers like the
 * close button or process teardown bypass the check by design).
 */
int compositor_window_owner(int window_id) {
  uint64_t flags;
  int owner = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      owner = windows[i].pid;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return owner;
}

/* ========================================================================= */
/* Window state control + enumeration (ASTRA §6.7: windows as objects).      */
/* These back the OBJ_TYPE_WINDOW capability (kernel/core/object.c) and       */
/* SYS_WINDOW_ENUM, used by the dock /sys/bin/nxui and the titlebar           */
/* background button below.  The compositor stays the mechanism; policy       */
/* (dock layout, what to restore) lives in userland (nxui).                   */
/* ========================================================================= */

/* __raise_to_front_locked - give a window the highest z-order.  Caller holds
 * compositor_lock.  Mirrors the click-to-front logic in
 * compositor_handle_click. */
static void __raise_to_front_locked(struct window *w) {
  int top_z = 0;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (windows[i].id != 0 && windows[i].z_order > top_z)
      top_z = windows[i].z_order;
  w->z_order = top_z + 1;
}

/* compositor_minimize_window - send a window to the background: hide it but
 * keep its slot/buffer so the dock can restore it.  If it held focus, focus
 * falls to the next top-most visible window.  Returns 0, or -ESRCH for an
 * unknown id. */
int compositor_minimize_window(int window_id) {
  uint64_t flags;
  int ret = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].visible = 0;
      windows[i].minimized = 1;
      if (sched_get_focus_pid() == windows[i].pid)
        __focus_topmost_locked(); /* re-point focus off the hidden window */
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      ret = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return ret;
}

/* Visibility, stacking and the focus hint belong to the same window
 * transition. A passive surface may be restored without taking input. */
static int activate_window(int id, int restoring) {
  if (id <= 0) return -ESRCH;
  uint64_t flags;
  int result = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS && result == -ESRCH; ++i) {
    struct window *win = &windows[i];
    if (win->id != id) continue;
    if (win->passive && !restoring) {
      result = -EPERM;
    } else {
      win->visible = 1;
      win->minimized = 0;
      __raise_to_front_locked(win);
      if (!win->passive) {
        sched_set_focus_pid(win->pid);
        __clear_other_carets_locked(win->pid);
      }
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      result = 0;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return result;
}

int compositor_restore_window(int window_id) {
  return activate_window(window_id, 1);
}

int compositor_focus_window(int window_id) {
  return activate_window(window_id, 0);
}

/* __fill_window_info_locked - serialise one window slot into the shared ABI
 * record (include/api/object.h).  Caller holds compositor_lock. */
static void __fill_window_info_locked(const struct window *w,
                                      struct window_info *wi) {
  wi->id = w->id;
  wi->pid = w->pid;
  wi->x = w->x;
  wi->y = w->y;
  wi->w = w->draw_w > 0 ? w->draw_w : w->width;
  wi->h = w->draw_h > 0 ? w->draw_h : w->height;
  unsigned int f = 0;
  if (w->visible)
    f |= WININFO_VISIBLE;
  if (w->minimized)
    f |= WININFO_MINIMIZED;
  if (w->top_most)
    f |= WININFO_TOPMOST;
  if (w->passive)
    f |= WININFO_PASSIVE;
  if (w->protected)
    f |= WININFO_PROTECTED;
  if (w->pid == sched_get_focus_pid())
    f |= WININFO_FOCUSED;
  wi->flags = f;
  int k = 0;
  while (k < 63 && w->title[k]) {
    wi->title[k] = w->title[k];
    k++;
  }
  wi->title[k] = '\0';
}

/* compositor_window_info - fill one window's info record by id.  Returns 0, or
 * -ESRCH for an unknown id.  Backs OS1_object_read() on an OBJ_TYPE_WINDOW. */
int compositor_window_info(int window_id, struct window_info *out) {
  uint64_t flags;
  int ret = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      __fill_window_info_locked(&windows[i], out);
      ret = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return ret;
}

/* sys_window_enum - SYS_WINDOW_ENUM backend: snapshot every live window into
 * the caller's struct window_info[] and return the count.  Read-only and
 * ungated (enumeration is information, not authority — acting on a window still
 * needs an OBJ_TYPE_WINDOW capability).  Mirrors sys_getprocs: snapshot under
 * the lock, copy to user after unlocking. */
long sys_window_enum(struct window_info *ubuf, size_t max) {
  if (!ubuf || max == 0)
    return 0;
  if (max > MAX_WINDOWS)
    max = MAX_WINDOWS;
  struct window_info *kbuf =
      (struct window_info *)kmalloc(sizeof(struct window_info) * max);
  if (!kbuf)
    return -ENOMEM;

  uint64_t flags;
  size_t n = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
    if (windows[i].id != 0)
      __fill_window_info_locked(&windows[i], &kbuf[n++]);
  }
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (n > 0 &&
      vmm_copy_to_user(ubuf, kbuf, sizeof(struct window_info) * n) != 0) {
    kfree(kbuf);
    return -EFAULT;
  }
  kfree(kbuf);
  return (long)n;
}

/* Copy one legacy window surface to the userland compositor.  This is the
 * compatibility bridge while v1 window objects are converted to mapped
 * capability surfaces: pixel composition and all scene policy belong to
 * compositord; the kernel only pins the window under its lock long enough to
 * make a bounded immutable copy. */
long compositor_window_snapshot(struct os1_window_snapshot_v1 *user_snapshot) {
  struct os1_window_snapshot_v1 request;
  if (!user_snapshot ||
      vmm_copy_from_user(&request, user_snapshot, sizeof(request)) != 0)
    return -EFAULT;
  if (request.abi_version != OS1_COMPOSITOR_ABI_V1 ||
      request.struct_size < sizeof(request) || request.id <= 0)
    return -EINVAL;

  struct os1_window_snapshot_v1 result = {0};
  uint32_t *source = NULL;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    const struct window *win = &windows[i];
    if (win->id != request.id)
      continue;
    result.abi_version = OS1_COMPOSITOR_ABI_V1;
    result.struct_size = sizeof(result);
    result.id = win->id;
    result.pid = win->pid;
    result.x = win->x;
    result.y = win->y;
    result.width = win->width;
    result.height = win->height;
    result.draw_width = win->draw_w > 0 ? win->draw_w : win->width;
    result.draw_height = win->draw_h > 0 ? win->draw_h : win->height;
    result.z_order = win->layout_registered &&
                             win->layout_layer == OS1_SURFACE_LAYOUT_LAYER_OVERLAY
                         ? 100000 + (int)win->layout_order : win->z_order;
    result.background_argb = win->bg_color;
    result.pixel_count = (size_t)win->width * (size_t)win->height;
    result.pixels = request.pixels;
    result.pixel_capacity = request.pixel_capacity;
    result.flags = 0;
    if (win->visible)
      result.flags |= WININFO_VISIBLE;
    if (win->minimized)
      result.flags |= WININFO_MINIMIZED;
    if (win->top_most)
      result.flags |= WININFO_TOPMOST;
    if (win->passive)
      result.flags |= WININFO_PASSIVE;
    if (win->protected)
      result.flags |= WININFO_PROTECTED;
    if (win->pid == sched_get_focus_pid())
      result.flags |= WININFO_FOCUSED;
    memcpy(result.title, win->title, sizeof(result.title));
    result.title[sizeof(result.title) - 1] = '\0';
    if (win->width <= 0 || win->height <= 0 || win->width > 4096 ||
        win->height > 4096 || result.pixel_count > 16u * 1024u * 1024u) {
      spin_unlock_irqrestore(&compositor_lock, flags);
      return -EINVAL;
    }
    source = win->buffer;
    break;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (result.id == 0)
    return -ESRCH;

  if (!request.pixels) {
    return vmm_copy_to_user(user_snapshot, &result, sizeof(result)) == 0
               ? 0
               : -EFAULT;
  }
  if (request.pixel_capacity < result.pixel_count)
    return -ENOSPC;
  if (!source)
    return -EIO;

  size_t bytes = result.pixel_count * sizeof(uint32_t);
  uint32_t *copy = (uint32_t *)kmalloc(bytes);
  if (!copy)
    return -ENOMEM;

  /* Revalidate the buffer identity after allocation; destroy/resize may have
   * happened while the lock was dropped.  A stale pointer is never read. */
  int copied = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    const struct window *win = &windows[i];
    if (win->id == request.id && win->buffer == source &&
        win->width == result.width && win->height == result.height) {
      memcpy(copy, source, bytes);
      copied = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (!copied) {
    kfree(copy);
    return -EAGAIN;
  }

  int rc = vmm_copy_to_user(request.pixels, copy, bytes);
  kfree(copy);
  if (rc != 0)
    return -EFAULT;
  return vmm_copy_to_user(user_snapshot, &result, sizeof(result)) == 0
             ? 0
             : -EFAULT;
}

/*
 * compositor_window_grid - report a window's terminal grid (cols x rows).
 *
 * The terminal cell size derives from the active font (proportional fonts
 * give a cell == max glyph advance), so a windowed TTY app cannot assume a
 * fixed 80x25.  Returns 0 and fills cols/rows on success, -1 if the window
 * id does not exist.  Backs SYS_WINDOW_GRID.
 */
int compositor_window_grid(int window_id, int *cols, int *rows) {
  uint64_t flags;
  int found = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      if (cols)
        *cols = windows[i].term.cols;
      if (rows)
        *rows = windows[i].term.rows;
      found = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return found;
}

/*
 * Detach window-owned allocations under compositor_lock, then release them
 * after unlocking. Allocator calls must not be nested under compositor_lock:
 * resize/create and process teardown otherwise invert allocator/compositor
 * lock order under SMP load.
 */
struct retired_window_resources {
  uint32_t *buffer;
  uint8_t *text_grid;
  uint32_t *attr_grid;
};

static void __free_retired_window_resources(
    struct retired_window_resources *resources) {
  if (resources->buffer)
    kfree(resources->buffer);
  if (resources->text_grid)
    kfree(resources->text_grid);
  if (resources->attr_grid)
    kfree(resources->attr_grid);
  resources->buffer = NULL;
  resources->text_grid = NULL;
  resources->attr_grid = NULL;
}

/* Destroy one window. The slot is unpublished before its storage is freed. */
void compositor_destroy_window(int window_id) {
  struct retired_window_resources retired = {0};
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != window_id)
      continue;

    int refocus = (windows[i].pid == sched_get_focus_pid());
    int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                  ddw, ddh + compositor_titlebar_height());
    compositor_dirty = 1;

    retired.buffer = windows[i].buffer;
    retired.text_grid = windows[i].term.text_grid;
    retired.attr_grid = windows[i].term.attr_grid;
    memset(&windows[i], 0, sizeof(struct window));
    if (window_count > 0)
      window_count--;
    else
      panic("Compositor: window_count underflow destroying id=%d", window_id);

    if (refocus)
      __focus_topmost_locked();
    compositor_apply_bounds_locked(bb_width, bb_height);
    break;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);

  __free_retired_window_resources(&retired);
}

/* Destroy all windows owned by a PID. A bounded local resource list makes the
 * table update atomic while keeping kfree outside compositor_lock. */
void compositor_destroy_windows_by_pid(int pid) {
  struct retired_window_resources retired[MAX_WINDOWS] = {{0}};
  int retired_count = 0;
  int refocus = 0;
  uint64_t flags;

  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0 || windows[i].pid != pid)
      continue;

    if (windows[i].pid == sched_get_focus_pid())
      refocus = 1;
    int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                  ddw, ddh + compositor_titlebar_height());
    compositor_dirty = 1;

    retired[retired_count].buffer = windows[i].buffer;
    retired[retired_count].text_grid = windows[i].term.text_grid;
    retired[retired_count].attr_grid = windows[i].term.attr_grid;
    retired_count++;
    memset(&windows[i], 0, sizeof(struct window));
    if (window_count > 0)
      window_count--;
    else
      panic("Compositor: window_count underflow destroying PID=%d", pid);
  }

  if (refocus)
    __focus_topmost_locked();
  if (retired_count > 0) compositor_apply_bounds_locked(bb_width, bb_height);
  spin_unlock_irqrestore(&compositor_lock, flags);

  for (int i = 0; i < retired_count; i++)
    __free_retired_window_resources(&retired[i]);
}

/*
 * compositor_get_buffer - return the raw pixel buffer pointer for a window.
 *
 * FIX(GFX-COMP-LOCK-02): the lookup itself now runs under compositor_lock,
 * consistent with every other window-table reader in this file (before this
 * fix it was the only lookup that read windows[] unlocked, racing a
 * concurrent compositor_destroy_window()/compositor_resize_window() that
 * kfree()s or reallocates the same buffer on another CPU).
 *
 * IMPORTANT — pointer lifetime: taking the lock here only makes the LOOKUP
 * safe.  It cannot make the RETURNED POINTER safe to dereference after this
 * function returns and the lock is released: nothing stops a concurrent
 * compositor_destroy_window() or compositor_resize_window() from freeing or
 * replacing that exact buffer the instant after this call unlocks.  This
 * function is kept for kernel-internal callers that own the window's
 * lifetime by construction (e.g. the same context that owns the window and
 * knows no destroy/resize can race it — direct kernel console mirroring).
 * A caller that CANNOT make that guarantee (a syscall handler acting on a
 * user-suppliable window_id) must use compositor_draw_rect() or
 * compositor_blit() instead: both perform the write to the buffer while
 * still holding compositor_lock, so there is no window for a concurrent
 * free to land in.
 */
uint32_t *compositor_get_buffer(int window_id) {
  uint64_t flags;
  uint32_t *buffer = NULL;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      buffer = windows[i].buffer;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return buffer;
}

/*
 * Find window by PID
 */
int compositor_get_window_by_pid(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].pid == pid) {
      int id = windows[i].id;
      spin_unlock_irqrestore(&compositor_lock, flags);
      return id;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return -1;
}

/* compositor_get_focus_pid() removed (DIR-02 / SCHED-01 #83): it was dead after
 * the scheduler→compositor inversion.  Focus is now read from the scheduler's
 * published sched_get_focus_pid() hint, never queried back from the compositor. */

/*
 * compositor_move_window - reposition a window programmatically (not via the
 * interactive drag path).
 *
 * FIX(GFX-COMP-LOCK-01): this function used to mutate windows[i].x/y with NO
 * lock at all — the only window-geometry mutator in the file that didn't take
 * compositor_lock.  Two concrete failures followed:
 *   1. Data race: compositor_render_internal() reads win->x/win->y under the
 *      lock on another CPU; a torn (non-atomic on some ABIs) or interleaved
 *      write here could hand the renderer a half-updated position — the same
 *      class of bug S1b fixed for compositor_update_mouse().
 *   2. Silent damage loss: every OTHER geometry mutator (drag, resize,
 *      minimize, restore, focus, destroy) calls expand_damage() so the moved
 *      footprint is actually recomposited.  This one didn't, so a
 *      programmatic move (e.g. a window manager repositioning a window
 *      on-screen-change) left stale pixels at the old AND new location until
 *      an unrelated event happened to damage that area.
 *
 * Both the old and new footprints are damaged (mirrors compositor_
 * destroy_window's vacated-footprint handling), and the on-screen draw size
 * is used for the footprint, consistent with every other damage call site in
 * this file.
 */
void compositor_move_window(int window_id, int x, int y) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      /* Registered plugins follow their edge declaration, not arbitrary
       * legacy WM coordinates. This also prevents a delayed old drag from
       * leaving an overlay suspended after a logical mode transition. */
      if (windows[i].layout_registered) continue;
      int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      int title_h = compositor_titlebar_height();

      /* Damage the OLD footprint (about to be vacated). */
      expand_damage(windows[i].x, windows[i].y - title_h, dw, dh + title_h);

      windows[i].x = x;
      windows[i].y = y;
      /* Dragged application windows follow the exact same resolved workarea
       * as created/resized windows. A mouse move must not bypass the top or
       * bottom edge reservations, even when dragging rapidly across modes. */
      int top = 0, bottom = 0;
      compositor_workarea_locked(&top, &bottom);
      compositor_fit_application_locked(&windows[i], top, bottom);
      int final_w = windows[i].draw_w > 0 ?
                    windows[i].draw_w : windows[i].width;
      int final_h = windows[i].draw_h > 0 ?
                    windows[i].draw_h : windows[i].height;

      /* Damage the NEW, bounded footprint. */
      expand_damage(windows[i].x, windows[i].y - title_h,
                    final_w, final_h + title_h);
      compositor_dirty = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * compositor_resize_window - resize a window's LOGICAL surface (GFX-DYN-01).
 *
 * Reallocates the window pixel buffer to w x h, reflows the embedded terminal
 * to the new cell grid. Explicit window-manager presentation bounds remain
 * authoritative while the application adopts logical pixels.  Allocation happens here, so this must be called from process
 * context (it backs SYS_WINDOW_RESIZE), never from an IRQ.  Returns 0 on
 * success, -1 on failure (the old surface is kept on failure).
 */

/* WINDOW object presentation geometry. No pixel allocation or rendering.
 * Holding the compositor lock serializes snapshot source and draw bounds. */
int compositor_set_window_draw_size(int id, int w, int h) {
  if (w < 1 || h < 1 || w > 4096 || h > 4096) return -EINVAL;
  uint64_t flags;
  int owner = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS && owner == -ESRCH; ++i) {
    struct window *win = &windows[i];
    if (win->id != id) continue;
    if (win->layout_registered) {
      owner = -EPERM; /* Edge plugins are resized only via their own layout. */
      continue;
    }
    owner = win->pid;
    win->presentation_managed = 1;
    win->requested_draw_w = w;
    win->requested_draw_h = h;
    int old_w = win->draw_w > 0 ? win->draw_w : win->width;
    int old_h = win->draw_h > 0 ? win->draw_h : win->height;
    if (old_w != w || old_h != h) {
      int title = compositor_titlebar_height();
      expand_damage(win->x, win->y - title, old_w, old_h + title);
      win->draw_w = w;
      win->draw_h = h;
      compositor_apply_bounds_locked(bb_width, bb_height);
      expand_damage(win->x, win->y - title, w, h + title);
      compositor_dirty = 1;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return owner < 0 ? owner : 0;
}

int compositor_resize_window(int window_id, int w, int h) {
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  struct window *win = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      win = &windows[i];
      break;
    }
  }
  if (!win) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  /* No-op if already at this logical size: keeps the grip-resize release path
   * (which echoes an INPUT_TYPE_RESIZE) from looping when the app re-applies
   * the same size. */
  if (win->width == w && win->height == h) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return 0;
  }

  /* Allocate the new surface before touching the live one. */
  size_t new_pixels = 0U;
  if (window_pixel_count(w, h, &new_pixels) != 0 ||
      new_pixels > (size_t)-1 / sizeof(uint32_t)) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  uint32_t *nbuf =
      (uint32_t *)kmalloc(new_pixels * sizeof(uint32_t));
  if (!nbuf) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  for (size_t p = 0U; p < new_pixels; ++p)
    nbuf[p] = win->bg_color;

  /* Damage the OLD on-screen footprint so the vacated area is repainted. */
  int old_dw = win->draw_w > 0 ? win->draw_w : win->width;
  int old_dh = win->draw_h > 0 ? win->draw_h : win->height;
  expand_damage(win->x, win->y - compositor_titlebar_height(), old_dw,
                old_dh + compositor_titlebar_height());

  uint32_t *obuf = win->buffer;
  int follows_logical = !win->presentation_managed;
  win->buffer = nbuf;
  win->width = w;
  win->height = h;
  if (follows_logical) {
    win->requested_draw_w = w;
    win->requested_draw_h = h;
    win->draw_w = w;
    win->draw_h = h;
  }
  compositor_apply_bounds_locked(bb_width, bb_height);
  win->has_alpha = ((win->bg_color >> 24) != 0xFF);

  /* Reflow the terminal to the new cell grid. */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w > 0 && char_h > 0 &&
      term_resize(&win->term, w / char_w, h / char_h) != 0)
    pr_err("compositor: terminal reflow to %dx%d cells failed for pid %d — the "
           "window keeps the OLD cell grid and will render clipped\n",
           w / char_w, h / char_h, win->pid);

  /* Damage the NEW footprint and repaint. */
  expand_damage(win->x, win->y - compositor_titlebar_height(), w,
                h + compositor_titlebar_height());
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (obuf)
    kfree(obuf);
  /* The owning application already requested this logical size. Echoing a
   * resize here feeds stale dimensions back into the same mailbox alongside
   * newer WM requests, causing repeated reallocations and resize oscillation.
   * Only the userland WM publishes presentation-change requests to owners. */
  return 0;
}

/*
 * The VT/ANSI terminal emulator (blend_pixel, SGR, CSI, caret, scroll) was
 * extracted to kernel/graphics/term.c (GFX-DYN-01, #123).  Pixel blending now
 * uses gl_blend_pixel() from <graphics/gl.h>.
 */

/* Erase carets on every window not owned by keep_pid; marks damage so they
 * repaint.  Caller holds compositor_lock.  Used on focus changes so the caret
 * follows the input window instead of lingering on the one that lost focus. */
static void __clear_other_carets_locked(int keep_pid) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    struct window *win = &windows[i];
    if (win->id == 0 || !win->term.caret_shown || win->pid == keep_pid)
      continue;
    size_t pixel_count = 0U;
    if (window_pixel_count(win->width, win->height, &pixel_count) != 0)
      continue;
    struct gl_surface s = {.width = win->width,
                           .height = win->height,
                           .stride = win->width,
                           .buffer = win->buffer,
                           .capacity = pixel_count};
    gfx_surface_verify(&s, "__clear_other_carets_locked/win");
    term_clear_caret(&win->term, &s);
    win->term.focused = 0;
    expand_damage(win->x, win->y - compositor_titlebar_height(), win->width,
                  win->height + compositor_titlebar_height());
    compositor_dirty = 1;
  }
}

/*
 * Write text to a window's terminal emulator.  Thin compositor seam over the
 * extracted VT engine (term.c, #123): look up the window, hand the terminal the
 * window's content surface, mark damage.  ANSI parsing, caret, scroll and SGR
 * all live in term_write().
 */
void compositor_window_write(int win_id, const char *buf, size_t count) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  struct window *win = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == win_id) {
      win = &windows[i];
      break;
    }
  }
  if (win == NULL || win->buffer == NULL) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }

  size_t pixel_count = 0U;
  if (window_pixel_count(win->width, win->height, &pixel_count) != 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }
  struct gl_surface win_surf = {.width = win->width,
                                .height = win->height,
                                .stride = win->width,
                                .buffer = win->buffer,
                                .capacity = pixel_count};
  gfx_surface_verify(&win_surf, "compositor_window_write/win");

  /* The caret is drawn only on the window that currently owns keyboard input.
   */
  win->term.focused = (win->pid == sched_get_focus_pid());
  term_write(&win->term, &win_surf, buf, count);

  /* Mark compositor as needing redraw (window area including title bar) */
  expand_damage(win->x, win->y - compositor_titlebar_height(), win->width,
                win->height + compositor_titlebar_height());
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/* Kernel pointer hit-testing, dragging and input dispatch were retired;
 * nxmoused is now the sole pointer policy implementation. */

/*
 * Composite All Windows to Screen
 */
/* Legacy desktop rasterization is retired; nxcompositor owns frame rendering. */

/*
 * Draw to Window
 */
/*
 * Draw to Window (Internal - No Locking)
 */
/*
 * FIX(GFX-COMP-BOUND-01): w/h arrive here verbatim from a syscall argument
 * (SYS_DRAW_RECT) with no upper bound applied before this point.  The old
 * code only bounds-checked each individual PIXEL (px/py against the window's
 * width/height) but still let the dy/dx LOOPS themselves iterate the full,
 * caller-supplied w*h — including the huge majority of iterations that would
 * immediately fail the bounds check and do nothing.  Because this runs under
 * compositor_lock taken via spin_lock_irqsave (a single global spinlock, IRQs
 * off, on the CPU that owns it), a caller passing h close to INT_MAX turns a
 * few nanoseconds of legitimate work into an unbounded hang of the ENTIRE
 * SYSTEM: every other CPU spinning on compositor_lock, timer ticks queued
 * behind disabled IRQs, one process' bad syscall value freezing the machine.
 *
 * This is exactly the loop-bound hazard NASA/JPL "Power of 10" rule 2 (every
 * loop must have a statically demonstrable fixed upper bound) is written for.
 * The fix clips the requested rect against the window's own logical
 * dimensions BEFORE entering the pixel loops, so the loop trip count is
 * bounded by MAX_WINDOW_DIM^2 regardless of what the caller passes in — the
 * same bound compositor_create_window() already enforces on window creation
 * (w,h <= 4096), reused here for consistency.
 */
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      /* Process Isolation: Verify Ownership */
      if (windows[i].pid != caller_pid &&
          caller_pid != 1) { /* PID 1 is root/init */
        pr_warn(
            "Compositor: Process %d tried to draw to window %d (owned by %d)\n",
            caller_pid, window_id, windows[i].pid);
        return;
      }

      /* Clip the rect to the window's logical surface FIRST: this is what
       * bounds the loop trip count, not the per-pixel check below (which
       * stays as defence in depth against an off-by-one in this clip). */
      int cx = x, cy = y, cw = w, ch = h;
      if (cw < 0)
        cw = 0;
      if (ch < 0)
        ch = 0;
      if (cx < 0) {
        cw += cx; /* shrink width by the amount we clip off the left */
        cx = 0;
      }
      if (cy < 0) {
        ch += cy;
        cy = 0;
      }
      if (cx + cw > windows[i].width)
        cw = windows[i].width - cx;
      if (cy + ch > windows[i].height)
        ch = windows[i].height - cy;

      if (cw > 0 && ch > 0) {
        size_t width_sz = (size_t)windows[i].width;
        uint32_t *row_base =
            windows[i].buffer + (size_t)cy * width_sz;
        for (int dy = 0; dy < ch; dy++) {
          uint32_t *row =
              row_base + (size_t)dy * width_sz + (size_t)cx;
          for (int dx = 0; dx < cw; dx++)
            row[dx] = color;
        }
      }

      if ((color >> 24) != 0xFF) {
        windows[i].has_alpha = 1;
      } else if (x <= 0 && y <= 0 && w >= windows[i].width &&
                 h >= windows[i].height) {
        windows[i].has_alpha = 0;
      }

      /* Damage the CLIPPED rect: nothing outside it was actually touched. */
      expand_window_content_damage(&windows[i], cx, cy, cw, ch);
      return;
    }
  }
}

/*
 * Draw to Window (Public - Locks)
 */
void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                          uint32_t color, int caller_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  draw_rect_internal(window_id, x, y, w, h, color, caller_pid);
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Blit user buffer to window
 */
void compositor_blit(int window_id, int x, int y, int w, int h,
                     const uint32_t *user_buf, int caller_pid) {
  // pr_info("BLIT: win=%d pid=%d buf=%p %dx%d\n", window_id, caller_pid,
  // user_buf, w, h);

  /*
   * FIX(GFX-COMP-BOUND-01 / GFX-COMP-OVERFLOW-01): reject an out-of-range
   * source rect BEFORE taking the lock.  Two independent hazards:
   *   - Unbounded loop trip count (see the header comment further below),
   *     same class as draw_rect_internal.
   *   - `dy * w` (used below to index into the caller's user_buf) is `int`
   *     arithmetic: with dy bounded only by an unclamped h and w unclamped,
   *     this can overflow and wrap to a small/negative offset, handing
   *     vmm_copy_from_user() a source address far from the caller's actual
   *     buffer instead of the OOB access safely faulting.  Capping w/h to
   *     the same <=4096 bound compositor_create_window() enforces on any
   *     window keeps `dy * w` (<=4096*4096, well within int32) provably
   *     free of overflow.
   */
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      /* Process Isolation: Verify Ownership */
      if (windows[i].pid != caller_pid && caller_pid != 1) {
        spin_unlock_irqrestore(&compositor_lock, flags);
        return;
      }

      int copied_x1 = windows[i].width;
      int copied_y1 = windows[i].height;
      int copied_x2 = 0;
      int copied_y2 = 0;

      /*
       * FIX(GFX-COMP-BOUND-01): same class of bug as draw_rect_internal
       * (see its comment) — h came straight from a syscall argument.  The
       * per-row `continue` below only skips WORK for an out-of-range row, it
       * does not shrink the LOOP itself, so a caller passing a huge h still
       * forces this loop — held under compositor_lock, IRQs off — to run
       * that many iterations before returning.  Clip the vertical range to
       * the window's height up front so the trip count is bounded by the
       * window's own (already-validated, <=4096) dimensions.
       */
      int row_start = y, row_count = h;
      if (row_count < 0)
        row_count = 0;
      if (row_start < 0) {
        row_count += row_start;
        row_start = 0;
      }
      if (row_start + row_count > windows[i].height)
        row_count = windows[i].height - row_start;
      if (row_count < 0)
        row_count = 0;

      /* Row offset (relative to the CALLER's original y) at which the
       * clipped range begins: user_buf's layout follows the caller's
       * original (x, y, w, h) rect, not the destination-clipped window
       * coordinates, so the source row index must be computed from this
       * offset rather than reusing `dy` directly (dy now indexes the
       * post-clip range, which starts later than row 0 whenever the top of
       * the rect was clipped). */
      int src_row_offset = row_start - y;

      /* Destination X clip is identical for every row (x/w do not vary per
       * row), so compute it once outside the loop instead of recomputing
       * four branches per row for every row in the rect. */
      int dest_x = x;
      int src_x = 0;
      int copy_w = w;
      if (dest_x < 0) {
        src_x += -dest_x;
        copy_w -= -dest_x;
        dest_x = 0;
      }
      if (dest_x + copy_w > windows[i].width)
        copy_w = windows[i].width - dest_x;

      /* Copy Logic: Row by Row for speed */
      for (int dy = 0; dy < row_count; dy++) {
        int py = row_start + dy;

        if (copy_w <= 0)
          break; /* every row has the same (empty) X range: nothing to do */

        /* Use copy_from_user instead of raw memcpy for security.  The
         * caller rect and destination window are both validated <=4096, so
         * these non-negative indices are representable before conversion. */
        size_t dst_index =
            (size_t)py * (size_t)windows[i].width + (size_t)dest_x;
        size_t src_row = (size_t)(src_row_offset + dy);
        size_t src_index = src_row * (size_t)w + (size_t)src_x;
        size_t copy_bytes = (size_t)copy_w * sizeof(uint32_t);
        void *dst_ptr = &windows[i].buffer[dst_index];
        const void *src_ptr = &user_buf[src_index];

        if (vmm_copy_from_user(dst_ptr, src_ptr, copy_bytes) != 0) {
          /* Page fault or invalid access: abort blit */
          spin_unlock_irqrestore(&compositor_lock, flags);
          return;
        }
        if (dest_x < copied_x1)
          copied_x1 = dest_x;
        if (py < copied_y1)
          copied_y1 = py;
        if (dest_x + copy_w > copied_x2)
          copied_x2 = dest_x + copy_w;
        if (py + 1 > copied_y2)
          copied_y2 = py + 1;
      }

      if (copied_x2 > copied_x1 && copied_y2 > copied_y1) {
        int cw = copied_x2 - copied_x1;
        int ch = copied_y2 - copied_y1;
        int copied_has_alpha =
            window_region_has_alpha(&windows[i], copied_x1, copied_y1, cw, ch);
        if (copied_has_alpha) {
          windows[i].has_alpha = 1;
        } else if (copied_x1 == 0 && copied_y1 == 0 &&
                   copied_x2 >= windows[i].width &&
                   copied_y2 >= windows[i].height) {
          windows[i].has_alpha = 0;
        }
        expand_window_content_damage(&windows[i], copied_x1, copied_y1, cw, ch);
      }

      spin_unlock_irqrestore(&compositor_lock, flags);
      return;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

void compositor_set_window_flags(int window_id, int flags_val) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      if (!windows[i].layout_registered)
        windows[i].top_most = (flags_val & 1) ? 1 : 0;
      windows[i].passive = (flags_val & 8) ? 1 : 0; /* bit 3: click-through */
      if (flags_val & 4)
        windows[i].visible = 0; /* bit 2: hide window */
      else if (flags_val & 2)
        windows[i].visible = 1; /* bit 1: show window */
      if ((!windows[i].visible || windows[i].passive) &&
          windows[i].pid == sched_get_focus_pid())
        __focus_topmost_locked();
      /* Damage the window's footprint so the show/hide/restack is actually
       * composited + flushed on the next render.  Without this the state
       * changed but the damage box stayed empty, so the popup only refreshed
       * when some OTHER event (the mouse passing over it) damaged that region —
       * the "notification sticks / won't disappear until I move the mouse" bug.
       * Same footprint math as compositor_destroy_windows_by_pid. */
      {
        int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
        int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
        expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                      ddw, ddh + compositor_titlebar_height());
        compositor_dirty = 1;
      }
      /* A hidden plugin releases its reservation immediately; showing it
       * restores the reservation before any ordinary window can overlap. */
      if (windows[i].layout_registered)
        compositor_apply_bounds_locked(bb_width, bb_height);
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}
