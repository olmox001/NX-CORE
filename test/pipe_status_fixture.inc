/* Real NX-CORE object_pipe_status() is textually included from the AST
 * extraction at build time, never rewritten in a local test imitation.
 * This fixture mocks only HAL memory, spinlocks and kobject pinning. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <object.h>
#define PIPE_BUF_SIZE OS1_TEST_PIPE_BUF_SIZE
struct kpipe {
  unsigned int count;
  int readers, writers;
};
struct kobject {
  int type;
  struct kpipe *pipe;
};
static struct kpipe current_pipe = {5U, 2, 1};
static struct kobject current = {OBJ_TYPE_PIPE, &current_pipe};
static int object_lock, pin_count, unref_count, lock_count;
static int hal_copy_from_user(void *to, const void *from, size_t length) {
  if (to == NULL || from == NULL) return -1;
  memcpy(to, from, length); return 0;
}
static int hal_copy_to_user(void *to, const void *from, size_t length) {
  if (to == NULL || from == NULL) return -1;
  memcpy(to, from, length); return 0;
}
static void spin_lock_irqsave(int *lock, uint64_t *flags) {
  if (lock != &object_lock) return;
  ++lock_count; *flags = 0;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
  (void)lock; (void)flags;
}
static struct kobject *pin_handle(int handle, uint32_t rights, long *error) {
  if (handle != 7 || rights != 0U) { *error = -EBADF; return NULL; }
  ++pin_count; return &current;
}
static void obj_unref(struct kobject *object) {
  if (object == &current) ++unref_count;
}
#include "actual_pipe_status.inc"

static int test_snapshot_and_updates(void) {
  struct os1_pipe_status_v1 status = {
      .abi_version = OS1_PIPE_STATUS_VERSION,
      .struct_size = sizeof(status)
  };
  if (object_pipe_status(7, (long)(uintptr_t)&status) != 0) return 1;
  if (status.available != 5U || status.capacity != PIPE_BUF_SIZE ||
      status.readers != 2U || status.writers != 1U) return 2;
  current_pipe.count = 0U;
  current_pipe.writers = 0;
  if (object_pipe_status(7, (long)(uintptr_t)&status) != 0 ||
      status.available != 0U || status.writers != 0U) return 3;
  if (pin_count != 2 || unref_count != 2 || lock_count != 2) return 4;
  return 0;
}

static int test_rejected_requests_and_types(void) {
  struct os1_pipe_status_v1 status = {
      .abi_version = OS1_PIPE_STATUS_VERSION,
      .struct_size = sizeof(status)
  };
  int prior = pin_count;
  status.abi_version++;
  if (object_pipe_status(7, (long)(uintptr_t)&status) != -EINVAL)
    return 1;
  status.abi_version = OS1_PIPE_STATUS_VERSION;
  status.struct_size = 0U;
  if (object_pipe_status(7, (long)(uintptr_t)&status) != -EINVAL)
    return 2;
  status.struct_size = sizeof(status);
  if (object_pipe_status(7, 0L) != -EFAULT) return 3;
  if (object_pipe_status(-1, (long)(uintptr_t)&status) != -EBADF)
    return 4;
  current.type = OBJ_TYPE_FILE;
  if (object_pipe_status(7, (long)(uintptr_t)&status) != -EINVAL)
    return 5;
  current.type = OBJ_TYPE_PIPE;
  if (pin_count != prior + 1 || pin_count != unref_count)
    return 6;
  return 0;
}

int main(void) {
  int first = test_snapshot_and_updates();
  int second = test_rejected_requests_and_types();
  return first != 0 ? first : second == 0 ? 0 : 100 + second;
}
