/*
 * kernel/graphics/graphics.c
 *
 * GPU presentation and emergency framebuffer graphics.
 *
 * Normal desktop composition belongs to userland. This file keeps the bounded
 * full-frame presentation bridge plus the direct framebuffer path required by
 * panic/fault reporting. Legacy generic screen-drawing wrappers that had no
 * production consumers are intentionally not part of this interface.
 */
#include <drivers/gpu/gpu.h>
#include <graphics/gl.h>
#include <kernel/fault.h>
#include <kernel/gfx_surface.h>
#include <kernel/graphics.h>
#include <kernel/printk.h>

/*
 * graphics_init - verify the primary GPU is discoverable through the HAL.
 *
 * Pure discovery log: the scanout is always reached per-call through
 * graphics_screen_surface(), never cached at init (a cached copy went stale
 * on every runtime resolution change — the retired g_ctx bug).
 */
void graphics_init(void) {
  struct gpu_device *dev = gpu_get_primary();
  if (dev)
    pr_info("Graphics: Initialized via HAL (%dx%d)\n", dev->width, dev->height);
  else
    pr_err("%s", "Graphics: No GPU device found!\n");
}

/*
 * graphics_screen_surface - fill 'out' with a gl_surface over the scanout.
 *
 * THE single accessor to the screen buffer (S-ALIGN F7): resolves the primary
 * GPU per call (picks up runtime mode changes) and reads the buffer through
 * the gpu_ops.get_framebuffer contract — the same method the compositor's
 * flush path uses.  The caller owns 'out' (no static: SMP- and fault-safe,
 * closes GFX-GFX-01).  vgpu_get_framebuffer is lock-free and NULL-safe, so
 * this path is safe from panic context (panic_screen below).
 *
 * Returns 0 on success, -1 if no GPU/framebuffer is present.
 */
int graphics_screen_surface(struct gl_surface *out) {
  struct gpu_device *dev = gpu_get_primary();
  if (!out || !dev || !dev->ops || !dev->ops->get_framebuffer)
    return -1;
  uint32_t *fb = (uint32_t *)dev->ops->get_framebuffer(dev, NULL);
  if (!fb)
    return -1;
  out->width = dev->width;
  out->height = dev->height;
  out->stride = dev->width;
  out->buffer = fb;
  /* S-STAB: the scanout's true size in pixels.  If a concurrent mode change
   * (vgpu_set_mode) left width/height and framebuffer_size momentarily torn —
   * the compositor↔GPU seam is unlocked here (S-ALIGN F7) — this makes the
   * inconsistency a loud, localized panic instead of an OOB blit into the
   * freed/short scanout backing. */
  out->capacity = dev->framebuffer_size / sizeof(uint32_t);
  gfx_surface_verify(out, "graphics_screen_surface");
  return 0;
}

/* Controlled full-frame hand-off used by the ring-3 compositor.  This kernel
 * path validates the surface and delegates only the bounded presentation to
 * the GPU provider; it performs no rasterization or window composition. */
int graphics_present_argb8888(const uint32_t *pixels, int width, int height,
                              int stride, size_t pixel_capacity) {
  if (!pixels || width <= 0 || height <= 0 || stride != width)
    return -EINVAL;
  /* Do not compare against the active scanout here. A mode transition may
   * have a prepared target with different geometry while the old scanout is
   * intentionally still visible. The GPU provider owns active/pending target
   * selection under its device lock. */
  size_t required = (size_t)width * (size_t)height;
  if (required / (size_t)width != (size_t)height ||
      pixel_capacity < required)
    return -EINVAL;
  struct gl_surface frame = {
      .width = width,
      .height = height,
      .stride = stride,
      .buffer = (uint32_t *)pixels,
      .capacity = pixel_capacity,
  };
  struct gfx_rect full = {.x = 0, .y = 0, .width = width, .height = height};
  return gpu_present_surface(&frame, &full);
}

/*
 * graphics_draw_rect - fill an axis-aligned rectangle on the screen.
 *
 * Params: x, y — top-left corner; w, h — dimensions; color — ARGB8888.
 * Clips to screen surface bounds via gl_draw_rect_fill.
 * Side effects: writes pixels to the HAL framebuffer.
 * Locking: none.
 */
void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        uint32_t color) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) == 0) {
    gfx_rect_t rect = {(int)x, (int)y, (int)w, (int)h};
    gfx_surface_fill(&screen, &rect, color);
  }
}

/*
 * panic_screen - paint the captured fault transcript on a RED framebuffer.
 *
 * DIR-05 / #139: a kernel fault halts everything and is otherwise visible only
 * on the UART.  This makes it readable on the display too, for machines with no
 * serial.  Fault-safe by construction: it does NOT touch the compositor (which
 * may be wedged or mid-render), takes no locks of its own, writes the primary
 * GPU framebuffer directly, and presents through the device flush op (which
 * skips its own lock in fault context).  Best-effort: returns quietly if no GPU
 * / framebuffer is present.  Called from panic() after the other CPUs quiesce.
 */
void panic_screen(const char *text) {
  struct gl_surface screen;
  if (graphics_screen_surface(&screen) != 0)
    return;
  struct gl_surface *s = &screen;
  if (!s->buffer || s->width <= 0 || s->height <= 0)
    return;

  const uint32_t RED = 0xFFB91C1C, WHITE = 0xFFFCFCFD;
  uint32_t npix = (uint32_t)s->width * (uint32_t)s->height;
  for (uint32_t i = 0; i < npix; i++)
    s->buffer[i] = RED;

  gl_draw_string(s, 12, 8, "*** KERNEL PANIC - system halted ***", WHITE);

  /* The fault transcript, line by line, clipped to the screen.  The top lines
   * (arch register dump + fault class) are the most diagnostic, so render from
   * the top. */
  int y = 32;
  char line[200];
  for (const char *p = text ? text : ""; *p && y < (int)s->height - 16;) {
    int k = 0;
    while (*p && *p != '\n' && k < (int)sizeof(line) - 1)
      line[k++] = *p++;
    line[k] = '\0';
    if (*p == '\n')
      p++;
    if (k > 0)
      gl_draw_string(s, 12, y, line, WHITE);
    y += 16;
  }

  struct gpu_device *dev = gpu_get_primary();
  if (dev && dev->ops && dev->ops->flush)
    dev->ops->flush(dev, 0, 0, dev->width, dev->height);
}
