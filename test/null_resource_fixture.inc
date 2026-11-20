/* Include REAL extracted NX-CORE null provider C bodies into this fixture.
 * Each of its three functions is exercised in positive and negative cases. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <object.h>
struct nx_resource_ops {
  long (*read)(void *, void *, size_t);
  long (*write)(void *, const void *, size_t);
  long (*wait)(void *, long);
  void (*release)(void *);
};
struct kobject {
  int type;
  const struct nx_resource_ops *resource_ops;
};
#include "actual_null_resource.inc"

static int test_eof_semantics(void) {
  char data[3] = {'A','B','C'};
  if (null_read(NULL, data, 3U) != 0 ||
      data[0] != 'A' || data[2] != 'C') return 1;
  return 0;
}
static int test_eof_empty_and_null(void) {
  if (null_read(NULL, NULL, 0U) != 0) return 1;
  if (null_read(NULL, NULL, 1024U) != 0) return 2;
  return 0;
}
static int test_discard_valid(void) {
  if (null_write(NULL, "data", 4U) != 4) return 1;
  if (null_write(NULL, NULL, 0U) != 0) return 2;
  return 0;
}
static int test_discard_errors(void) {
  if (null_write(NULL, NULL, 1U) != -EFAULT) return 1;
  if (null_write(NULL, "data", (size_t)INT64_MAX + 1U) != -EINVAL)
    return 2;
  return 0;
}
static int test_acquire_resource(void) {
  struct kobject object = {0};
  long acquired = nx_null_resource_acquire(OS1_RESOURCE_NULL,
                        OS1_RIGHT_READ | OS1_RIGHT_WRITE, &object);
  if (acquired != 0 || object.resource_ops == NULL ||
      object.resource_ops->read(NULL, NULL, 0U) != 0 ||
      object.resource_ops->write(NULL, "abc", 3U) != 3)
    return 1;
  return 0;
}
static int test_reject_forged_null(void) {
  struct kobject object = {0};
  if (nx_null_resource_acquire("device/not-null",
               OS1_RIGHT_READ, &object) != -ENOENT) return 1;
  if (nx_null_resource_acquire(OS1_RESOURCE_NULL,
               OS1_RIGHT_DESTROY, &object) != -EINVAL) return 2;
  if (nx_null_resource_acquire(OS1_RESOURCE_NULL,
               OS1_RIGHT_DUPLICATE, &object) != -EINVAL) return 3;
  if (nx_null_resource_acquire(OS1_RESOURCE_NULL,
               OS1_RIGHT_READ, NULL) != -EINVAL) return 4;
  if (nx_null_resource_acquire(OS1_RESOURCE_NULL, 0U,
                               &object) != -EINVAL) return 5;
  return 0;
}
int main(void) {
  int result = test_eof_semantics();
  if (result != 0) return result;
  result = test_eof_empty_and_null();
  if (result != 0) return 10 + result;
  result = test_discard_valid();
  if (result != 0) return 20 + result;
  result = test_discard_errors();
  if (result != 0) return 30 + result;
  result = test_acquire_resource();
  if (result != 0) return 40 + result;
  result = test_reject_forged_null();
  return result == 0 ? 0 : 50 + result;
}
