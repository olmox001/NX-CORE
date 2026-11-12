#include <kernel/ext4.h>
#include <kernel/object_boot.h>
#include <kernel/registry.h>
#include <kernel/vfs.h>
#include <stdarg.h>

const struct fs_ops ext4_fs_ops = {.name = "test-ext4"};
int console_loglevel = 7;
static int registry_result;
static int registry_calls;
static const char *captured_key;
static const char *captured_value;

static int same(const char *left, const char *right) {
  unsigned index = 0U;
  for (; index < 128U; ++index) {
    if (left[index] != right[index])
      return 0;
    if (left[index] == '\0')
      return 1;
  }
  return 0;
}

int vfs_register_fs(const struct fs_ops *ops) { return ops == &ext4_fs_ops ? 0 : -1; }
void vfs_init(void) {}
void registry_init(void) {}
void registry_mount_vfs(void) {}
void procfs_init(void) {}
int registry_set(const char *key, const char *value, int owner_pid) {
  if (owner_pid != 0)
    return -1;
  ++registry_calls;
  captured_key = key;
  captured_value = value;
  return registry_result;
}
int printk(const char *fmt, ...) {
  (void)fmt;
  return 0;
}
void panic(const char *fmt, ...) {
  (void)fmt;
  __builtin_trap();
}

static int test_generated_entry_is_published(void) {
  registry_result = 0;
  registry_calls = 0;
  captured_key = 0;
  captured_value = 0;
  if (nx_object_registry_start() != 0 || registry_calls != 1)
    return 1;
  if (!same(captured_key, NX_BOOT_REGISTRY_KEY))
    return 2;
  return same(captured_value, "/krnl/extension/nxmanageDD") ? 0 : 3;
}

static int test_registry_failure_is_fail_closed(void) {
  registry_result = -1;
  registry_calls = 0;
  captured_key = 0;
  captured_value = 0;
  if (nx_object_registry_start() == 0 || registry_calls != 1)
    return 1;
  if (!same(captured_key, NX_BOOT_REGISTRY_KEY))
    return 2;
  return same(captured_value, "/krnl/extension/nxmanageDD") ? 0 : 3;
}

int main(void) {
  if (test_generated_entry_is_published() != 0)
    return 1;
  if (test_registry_failure_is_fail_closed() != 0)
    return 2;
  return 0;
}
