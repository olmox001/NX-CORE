#ifndef _DRIVERS_GPU_H
#define _DRIVERS_GPU_H

#include <stddef.h>
#include <stdint.h>

struct gpu_device;

#define GPU_DISPLAY_MODE_MAX 32U
#define GPU_DISPLAY_MODE_CURRENT (1U << 0)
#define GPU_DISPLAY_MODE_PREFERRED (1U << 1)
#define GPU_DISPLAY_MODE_INTERLACED (1U << 2)

struct gpu_display_mode {
  uint32_t width;
  uint32_t height;
  uint32_t refresh_millihz;
  uint32_t flags;
};

struct gpu_ops {
  int (*init)(struct gpu_device *dev);
  /* Prepare width x height as the next presentation target. Providers may
   * keep the current scanout visible until a frame for the prepared geometry
   * is ready; present() then commits the replacement atomically. 0 means the
   * target is active or prepared. (GFX-DYN-01 #49) */
  int (*set_mode)(struct gpu_device *dev, int width, int height);
  void *(*get_framebuffer)(struct gpu_device *dev, size_t *size);
  int (*flush)(struct gpu_device *dev, int x, int y, int w, int h);
  /* Atomically copy a region of a caller-owned ARGB8888 source (src_w x src_h,
   * stride == src_w) into the scanout backing AND transfer+flush it to the
   * host, all under the driver's own lock (S-STAB / RC2).  The compositor uses
   * this instead of get_framebuffer()+memcpy()+flush(): doing the copy under
   * the same lock that set_mode() takes to swap+free the backing closes the
   * use-after-free where a concurrent resolution/zoom change frees the scanout
   * mid-copy. A frame matching a prepared target may atomically commit that
   * target after upload. A stale frame returns -EAGAIN so the caller refreshes
   * display state and retries; other negative returns are provider failures.
   * 0 means the frame was presented and any pending commit completed. */
  int (*present)(struct gpu_device *dev, const uint32_t *src, int src_w,
                 int src_h, int x, int y, int w, int h);
  void (*destroy)(struct gpu_device *dev);
  /* Snapshot the provider's active scanout size. This is the current
   * hardware presentation target, not the compositor's logical desktop and
   * not a preferred/advertised host mode. 0 on success. */
  int (*get_display_info)(struct gpu_device *dev, int *width, int *height);
  /* Poll for a host-initiated display-change event.  Returns 1 and fills
   * new_w/new_h when the host resized the output, 0 if nothing changed,
   * <0 on error/unsupported transport. */
  int (*poll_events)(struct gpu_device *dev, int *new_w, int *new_h);
  /* Provider-attested hardware mode catalog. The generation changes when the
   * connector catalog changes so userland cannot apply a stale index. */
  int (*get_mode)(struct gpu_device *dev, uint32_t index,
                  struct gpu_display_mode *mode, uint32_t *count,
                  uint64_t *generation);
  int (*set_mode_index)(struct gpu_device *dev, uint32_t index,
                        uint64_t generation);
};

struct gpu_device {
  char name[32];
  int width;
  int height;
  int bpp;
  /* The scanout buffer is reached ONLY through ops->get_framebuffer (S-ALIGN
   * F7) — the old raw framebuffer_virt field was a second, field-level path
   * to the same memory and is retired. */
  size_t framebuffer_size;
  struct gpu_ops *ops;
  void *priv;              /* Driver private data */
  struct gpu_device *next; /* Linked list for multi-gpu */
};

/* Core GPU Management */
int gpu_register(struct gpu_device *dev);
void gpu_unregister(struct gpu_device *dev);
struct gpu_device *gpu_get_primary(void);

/* Contract wrappers over the primary GPU (GFX-DYN-01).  The kernel core
 * (compositor) calls these instead of reaching into a driver: no driver
 * internals, no magic numbers in the core. */
int gpu_set_mode(int width, int height);
int gpu_get_display_info(int *width, int *height);
int gpu_poll_events(int *new_w, int *new_h);
int gpu_get_mode(uint32_t index, struct gpu_display_mode *mode,
                 uint32_t *count, uint64_t *generation);
int gpu_set_mode_index(uint32_t index, uint64_t generation);

/* Surface-speaking present (graphics-port): the provider contract talks the
 * same semantic unit the whole stack does — a validated gfx_surface plus a
 * damage rect — instead of raw pointer+geometry ints.  The S-STAB
 * gfx_surface_verify() gate runs HERE, at the core/driver seam, so a
 * geometry/allocation desync panics loudly at the source instead of becoming
 * a silent OOB copy in a driver.  Present semantics are unchanged underneath
 * (atomic copy+flush under the driver lock, <0 = skip this frame). */
struct gfx_rect;
struct gl_surface;
int gpu_present_surface(const struct gl_surface *src,
                        const struct gfx_rect *damage);

#endif
