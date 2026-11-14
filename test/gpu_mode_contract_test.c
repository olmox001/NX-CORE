#include <drivers/gpu/gpu.h>
#include <graphics/gl.h>
#include <stdarg.h>

int console_loglevel = -1;

void hal_irq_save(uint64_t *flags) { *flags = 0U; }
void hal_irq_restore(uint64_t flags) { (void)flags; }
void hal_spin_lock(volatile uint32_t *lock) { (void)lock; }
void hal_spin_unlock(volatile uint32_t *lock) { (void)lock; }

int printk(const char *fmt, ...) {
  (void)fmt;
  return 0;
}

void gfx_surface_verify(const struct gl_surface *surface, const char *where) {
  (void)surface;
  (void)where;
}

static int fake_get_display_info(struct gpu_device *dev, int *width,
                                 int *height) {
  if (dev == 0 || dev->width <= 0 || dev->height <= 0) return -1;
  if (width != 0) *width = dev->width;
  if (height != 0) *height = dev->height;
  return 0;
}

static int test_missing_provider_fails(void) {
  int width = 17;
  int height = 19;
  if (gpu_get_display_info(&width, &height) != -1) return 1;
  return width == 17 && height == 19 ? 0 : 2;
}

static int test_active_mode_tracks_provider(void) {
  struct gpu_ops ops = {.get_display_info = fake_get_display_info};
  struct gpu_device dev = {.width = 1280, .height = 800, .ops = &ops};
  int width = 0;
  int height = 0;
  if (gpu_register(&dev) != 0) return 1;
  if (gpu_get_display_info(&width, &height) != 0 ||
      width != 1280 || height != 800) {
    gpu_unregister(&dev);
    return 2;
  }
  dev.width = 1024;
  dev.height = 768;
  if (gpu_get_display_info(&width, &height) != 0 ||
      width != 1024 || height != 768) {
    gpu_unregister(&dev);
    return 3;
  }
  gpu_unregister(&dev);
  return gpu_get_display_info(&width, &height) == -1 ? 0 : 4;
}

int main(void) {
  if (test_missing_provider_fails() != 0) return 1;
  if (test_active_mode_tracks_provider() != 0) return 2;
  return 0;
}
