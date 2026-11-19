/* /dev/null is a capability-backed null resource, not a fake inode.
 * Read returns EOF; writes discard all bytes; no state, allocator, global
 * mutable data, or HAL dependency. Providers own behavior, not the kernel
 * object core or the Unix compatibility personality. */
#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/string.h>
#include <object.h>
#include <stdint.h>

static long null_read(void *state, void *buffer, size_t length) {
  (void)state;
  (void)buffer;
  (void)length;
  return 0; /* EOF, not an error and not simulated input */
}

static long null_write(void *state, const void *buffer, size_t length) {
  (void)state;
  if (buffer == NULL && length != 0U) return -EFAULT;
  if (length > INT64_MAX) return -EINVAL;
  /* No read/copy of caller bytes: the NULL device discards output by
   * definition. A 64-bit count is returned without partial success. */
  return (long)length;
}

static const struct nx_resource_ops null_ops = {
    .read = null_read, .write = null_write};

int nx_null_resource_acquire(const char *path, uint32_t rights,
                             struct kobject *object) {
  if (strcmp(path, OS1_RESOURCE_NULL) != 0) return -ENOENT;
  const uint32_t supported = OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                             OS1_RIGHT_DUPLICATE;
  if (object == NULL || rights == 0U ||
      (rights & ~supported) != 0U ||
      (rights & (OS1_RIGHT_READ | OS1_RIGHT_WRITE)) == 0U)
    return -EINVAL;
  object->resource_ops = &null_ops;
  return 0;
}
