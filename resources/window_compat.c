/* Transitional v1 surface storage. Desktop policy is not part of object core.
 * The existing snapshot and move ABI remains available during broker migration. */
#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/graphics.h>
#include <kernel/hal_uaccess.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <surface.h>
#include <os1_compositor.h>

static long window_read(void *state, void *user, size_t size) {
  (void)state;
  if (size != sizeof(struct os1_window_snapshot_v1)) return -EINVAL;
  if (!proc_is_privileged(current_process)) {
    struct os1_window_snapshot_v1 request;
    if (hal_copy_from_user(&request, user, sizeof(request)) != 0)
      return -EFAULT;
    /* The public enumeration already exposes geometry/title. An unprivileged
     * reader is allowed to query exactly that metadata, never raw pixels. */
    if (request.pixels != NULL || request.pixel_capacity != 0U)
      return -EPERM;
  }
  return compositor_window_snapshot(user);
}

static long window_write(void *state, const void *user, size_t size) {
  struct os1_surface_move_v1 request;
  (void)state;
  struct process *caller = current_process;
  if (caller == NULL || !proc_has_cap(caller, CAP_WINDOW_MANAGE))
    return -EPERM;
  if (size != sizeof(request)) return -EINVAL;
  if (hal_copy_from_user(&request, user, size)) return -EFAULT;
  if (request.abi_version != OS1_SURFACE_ABI_VERSION ||
      request.struct_size != sizeof(request) || request.id <= 0) return -EINVAL;
  compositor_move_window(request.id, request.x, request.y);
  return 0;
}

/* Plugins mutate only their own surfaces, even if the caller is privileged.
 * Resource rights/capabilities and PID ownership are separate checks. */
static long layout_read(void *state, void *user, size_t size) {
  (void)state;
  if (size != sizeof(struct os1_surface_layout_v1)) return -EINVAL;
  struct os1_surface_layout_v1 layout;
  if (hal_copy_from_user(&layout, user, sizeof(layout)) != 0) return -EFAULT;
  if (layout.abi_version != OS1_SURFACE_ABI_VERSION ||
      layout.struct_size != sizeof(layout)) return -EINVAL;
  int status = compositor_layout_query(&layout);
  if (status != 0) return status;
  return hal_copy_to_user(user, &layout, sizeof(layout)) == 0
             ? (long)sizeof(layout) : -EFAULT;
}

static long layout_write(void *state, const void *user, size_t size) {
  (void)state;
  if (size != sizeof(struct os1_surface_layout_v1)) return -EINVAL;
  struct process *caller = current_process;
  /* This right is deliberately narrower than CAP_WINDOW_MANAGE: a ROOT
   * plugin can register ONLY its own window. It cannot move, restack or
   * reserve space on behalf of another process. The compositor verifies
   * PID ownership again under its metadata lock. */
  if (caller == NULL || !proc_is_privileged(caller))
    return -EPERM;
  struct os1_surface_layout_v1 layout;
  if (hal_copy_from_user(&layout, user, sizeof(layout)) != 0) return -EFAULT;
  int result = compositor_layout_apply(&layout, (int)caller->pid);
  return result == 0 ? (long)sizeof(layout) : result;
}

static const struct nx_resource_ops layout_ops = {
    .read = layout_read, .write = layout_write};

static const struct nx_resource_ops window_ops = {
    .read = window_read, .write = window_write};

int nx_window_resource_acquire(const char *path, uint32_t rights,
                               struct kobject *object) {
  if (strcmp(path, OS1_RESOURCE_SURFACE_LAYOUT) == 0) {
    if (rights != OS1_RIGHT_READ && rights != OS1_RIGHT_WRITE)
      return -EPERM;
    object->resource_ops = &layout_ops;
    return 0;
  }
  if (strcmp(path, OS1_RESOURCE_WINDOW_STORAGE)) return -ENOENT;
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                          OS1_RIGHT_TRANSFER | OS1_RIGHT_DUPLICATE;
  if (rights & ~allowed) return -EPERM;
  object->resource_ops = &window_ops;
  return 0;
}
