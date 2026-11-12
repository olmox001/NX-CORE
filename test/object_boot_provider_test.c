#include <kernel/ext4.h>
#include <kernel/object_boot.h>
#include <kernel/registry.h>
#include <kernel/vfs.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

const struct fs_ops ext4_fs_ops = {.name = "test-ext4"};
int console_loglevel = 7;

enum { TEST_RECORDS = 16, TEST_KEY = 64, TEST_VALUE = 96 };
struct test_record {
  char key[TEST_KEY];
  char value[TEST_VALUE];
};

static struct test_record records[TEST_RECORDS];
static unsigned record_count;
static const char *manager_config;
static const char *images_config;
static int forced_registry_failure;

static int text_copy(char *dst, unsigned cap, const char *src) {
  if (dst == NULL || src == NULL || cap == 0U)
    return -1;
  for (unsigned i = 0U; i < cap; ++i) {
    dst[i] = src[i];
    if (src[i] == '\0')
      return 0;
  }
  dst[cap - 1U] = '\0';
  return -1;
}

static int same(const char *left, const char *right) {
  if (left == NULL || right == NULL)
    return 0;
  for (unsigned i = 0U; i < TEST_VALUE; ++i) {
    if (left[i] != right[i])
      return 0;
    if (left[i] == '\0')
      return 1;
  }
  return 0;
}

static void reset_state(const char *manager, const char *images) {
  record_count = 0U;
  manager_config = manager;
  images_config = images;
  forced_registry_failure = 0;
  for (unsigned i = 0U; i < TEST_RECORDS; ++i) {
    records[i].key[0] = '\0';
    records[i].value[0] = '\0';
  }
}

int vfs_register_fs(const struct fs_ops *ops) {
  return ops == &ext4_fs_ops ? 0 : -1;
}
void vfs_init(void) {}
void registry_init(void) {}
void registry_mount_vfs(void) {}
void procfs_init(void) {}

int vfs_read_file(const char *path, void *buffer, uint32_t size,
                  uint64_t offset) {
  if (offset != 0U || path == NULL || buffer == NULL)
    return -1;
  const char *source = NULL;
  if (same(path, "/system/boot/manager.conf"))
    source = manager_config;
  else if (same(path, "/system/boot/images.conf"))
    source = images_config;
  if (source == NULL)
    return -1;

  unsigned length = 0U;
  for (; length < size && source[length] != '\0'; ++length)
    ((char *)buffer)[length] = source[length];
  return source[length] == '\0' ? (int)length : -1;
}

int registry_set(const char *key, const char *value, int owner_pid) {
  if (owner_pid != 0 || forced_registry_failure != 0 ||
      record_count >= TEST_RECORDS)
    return -1;
  if (text_copy(records[record_count].key, TEST_KEY, key) != 0 ||
      text_copy(records[record_count].value, TEST_VALUE, value) != 0)
    return -1;
  ++record_count;
  return 0;
}

int registry_get(const char *key, char *buffer, size_t size) {
  for (unsigned i = 0U; i < record_count; ++i) {
    if (same(records[i].key, key))
      return text_copy(buffer, (unsigned)size, records[i].value);
  }
  return -1;
}

int printk(const char *fmt, ...) {
  (void)fmt;
  return 0;
}
void panic(const char *fmt, ...) {
  (void)fmt;
  __builtin_trap();
}

static int has_record(const char *key, const char *value) {
  for (unsigned i = 0U; i < record_count; ++i) {
    if (same(records[i].key, key) && same(records[i].value, value))
      return 1;
  }
  return 0;
}

static int test_catalog_and_init_are_published(void) {
  reset_state("init=nxinit\n",
              "init|nxinit|/sbin/nxinit\n"
              "service|notify|/sys/services/nxntfy_srv\n"
              "service|shell|/sys/services/nxttyd\n");
  if (nx_object_registry_start() != 0)
    return 1;
  if (!has_record(NX_BOOT_REGISTRY_KEY, "/krnl/extension/nxmanageDD"))
    return 2;
  if (!has_record("system.boot.image.init.nxinit", "/sbin/nxinit"))
    return 3;
  if (!has_record("system.boot.image.service.notify",
                  "/sys/services/nxntfy_srv"))
    return 4;
  return has_record(NX_BOOT_INIT_IMAGE_KEY, "nxinit") ? 0 : 5;
}

static int test_unknown_init_image_fails_closed(void) {
  reset_state("init=missing\n",
              "init|nxinit|/sbin/nxinit\n"
              "service|notify|/sys/services/nxntfy_srv\n");
  return nx_object_registry_start() != 0 ? 0 : 1;
}

static int test_invalid_service_image_fails_closed(void) {
  reset_state("init=nxinit\n",
              "init|nxinit|/sbin/nxinit\n"
              "service|escape|/sbin/escape\n");
  return nx_object_registry_start() != 0 ? 0 : 1;
}

static int test_registry_failure_fails_closed(void) {
  reset_state("init=nxinit\n",
              "init|nxinit|/sbin/nxinit\n"
              "service|notify|/sys/services/nxntfy_srv\n");
  forced_registry_failure = 1;
  return nx_object_registry_start() != 0 ? 0 : 1;
}

int main(void) {
  if (test_catalog_and_init_are_published() != 0)
    return 1;
  if (test_unknown_init_image_fails_closed() != 0)
    return 2;
  if (test_invalid_service_image_fails_closed() != 0)
    return 3;
  if (test_registry_failure_fails_closed() != 0)
    return 4;
  return 0;
}
