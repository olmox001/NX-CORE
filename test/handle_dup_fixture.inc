/* Host fixture compiles the REAL object_duplicate_min/object_duplicate_to
 * implementations extracted from NX-CORE by tools/test_handle_dup.py.
 * The mocks provide ONLY kobject table, spinlock and endpoint lifecycle. */
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <object.h>
#define NPROC_HANDLES OS1_TEST_NPROC_HANDLES
#define ENDPOINT_WAKE_NONE 0U
struct kobject {
  int type;
  int refcount;
};
struct handle_entry {
  struct kobject *obj;
  uint32_t rights;
};
struct process {
  struct handle_entry *handles;
  int pid;
};
static struct handle_entry table[NPROC_HANDLES];
static struct process process = {table, 12};
static struct process *current_process = &process;
static struct kobject first = {OBJ_TYPE_FILE, 1};
static struct kobject old_object = {OBJ_TYPE_PIPE, 1};
static int sched_lock, lock_error;
static int object_lock, added, dropped, wakes, freed;
static void spin_lock_irqsave(int *lock, uint64_t *flags) {
  if (*lock != 0) ++lock_error;
  *lock = 1; *flags = 0U;
}
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
  if (*lock != 1) ++lock_error;
  *lock = 0; (void)flags;
}
static void spin_lock(int *lock) {
  if (sched_lock != 1 || *lock != 0) ++lock_error;
  *lock = 1;
}
static void spin_unlock(int *lock) {
  if (sched_lock != 1 || *lock != 1) ++lock_error;
  *lock = 0;
}
static struct process *__process_find_by_pid(int pid) {
  if (!sched_lock) ++lock_error;
  return pid == process.pid ? &process : NULL;
}
static void endpoint_handle_add_locked(struct kobject *object,
                                       uint32_t rights) {
  if (object != NULL && rights != 0U) ++added;
}
static uint32_t endpoint_handle_drop_locked(struct kobject *object,
                                           uint32_t rights) {
  if (object != NULL && rights != 0U) {
    ++dropped;
    return 1U;
  }
  return ENDPOINT_WAKE_NONE;
}
static void endpoint_wake(struct kobject *object, uint32_t flags) {
  if (sched_lock || object_lock) ++lock_error;
  if (object != NULL && object->refcount > 0 && flags != 0U) ++wakes;
}
static void kobj_free(struct kobject *object) {
  if (object != NULL) ++freed;
}
#include "actual_handle_dup.inc"

static void reset(void) {
  memset(table, 0, sizeof(table));
  first.refcount = 1;
  old_object.refcount = 1;
  added = dropped = wakes = freed = 0;
  table[5].obj = &first;
  table[5].rights = OS1_RIGHT_READ | OS1_RIGHT_DUPLICATE;
  table[1].obj = &old_object;
  table[1].rights = OS1_RIGHT_WRITE;
}

static int test_minimum_and_attenuation(void) {
  reset();
  long result = object_duplicate_min(5, 8, OS1_RIGHT_READ);
  if (result != 8 || table[8].obj != &first ||
      table[8].rights != OS1_RIGHT_READ || first.refcount != 2) return 1;
  result = sys_handle_dup(5, OS1_RIGHT_ALL);
  if (result != 0 || table[0].obj != &first ||
      table[0].rights != table[5].rights || first.refcount != 3) return 2;
  if (object_duplicate_min(5, NPROC_HANDLES, OS1_RIGHT_ALL) !=
      -EINVAL) return 3;
  if (object_duplicate_min(9, 0, OS1_RIGHT_ALL) != -EBADF) return 4;
  table[5].rights = OS1_RIGHT_READ;
  if (object_duplicate_min(5, 0, OS1_RIGHT_ALL) != -EPERM) return 5;
  if (added != 2 || dropped != 0 || freed != 0) return 6;
  return 0;
}

static int test_exact_atomic_replacement_and_denials(void) {
  reset();
  long result = object_duplicate_to(5, 1);
  if (result != 1 || table[1].obj != &first ||
      table[1].rights != table[5].rights ||
      first.refcount != 2 || old_object.refcount != 0 ||
      dropped != 1 || added != 1 || wakes != 1 || freed != 1)
    return 1;
  if (object_duplicate_to(5, 5) != 5 || first.refcount != 2)
    return 2;
  if (object_duplicate_to(9, 1) != -EBADF ||
      object_duplicate_to(5, -1) != -EBADF ||
      object_duplicate_to(5, NPROC_HANDLES) != -EBADF) return 3;
  table[5].rights = OS1_RIGHT_READ;
  if (object_duplicate_to(5, 1) != -EPERM) return 4;
  if (object_duplicate_to(5, 5) != 5) return 5;
  return 0;
}

static int test_redirect_source_lifetime(void) {
  reset();
  struct handle_entry child_table[NPROC_HANDLES] = {0};
  struct process child = {child_table, 33};
  child_table[1] = table[1];
  if (process_redirect_child_fd_from(999, &child, 1, 5) != -ESRCH ||
      process_redirect_child_fd_from(process.pid, &child, 1, 4) != -EBADF ||
      child_table[1].obj != &old_object || wakes != 0) return 1;
  if (process_redirect_child_fd(&child, 1, 5) != 0 ||
      child_table[1].obj != &first || child_table[1].rights != table[5].rights ||
      first.refcount != 2 || freed != 1 || wakes != 1 || lock_error != 0) return 2;
  return 0;
}

int main(void) {
  int first_result = test_minimum_and_attenuation();
  int second_result = test_exact_atomic_replacement_and_denials();
  int redirected = test_redirect_source_lifetime();
  if (redirected != 0) return 200 + redirected;
  return first_result != 0 ? first_result :
       second_result != 0 ? 100 + second_result : 0;
}
