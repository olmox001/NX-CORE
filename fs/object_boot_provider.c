/*
 * Storage provider for the object/registry boot contract.
 * This is the filesystem adapter, NOT the core K3 bootstrap policy.
 *
 * It is the sole owner of converting protected on-image boot metadata into
 * machine registry objects. Core and nxmanageDD consume object identities,
 * never VFS configuration files.
 */
#include <kernel/object_boot.h>
#include <boot_entry.h>
#include <kernel/ext4.h>
#include <kernel/procfs.h>
#include <kernel/registry.h>
#include <kernel/vfs.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#define NX_BOOT_MANAGER_CONFIG "/system/boot/manager.conf"
#define NX_BOOT_IMAGES_CONFIG "/system/boot/images.conf"
#define NX_BOOT_IMAGE_PREFIX "system.boot.image."
#define NX_BOOT_INIT_PREFIX "system.boot.image.init."
#define NX_BOOT_SERVICE_PREFIX "system.boot.image.service."

enum {
  NX_BOOT_CATALOG_BYTES = 2048,
  NX_BOOT_LINE_BYTES = 128,
  NX_BOOT_IMAGE_NAME_MAX = 24,
  NX_BOOT_IMAGE_PATH_MAX = 72
};

void nx_object_storage_mount(void) {
  if (vfs_register_fs(&ext4_fs_ops) != 0)
    panic("BOOT-OBJECT: storage provider registration failed");
  vfs_init();
  pr_info("%s", "BOOT-OBJECT: storage provider mounted\n");
}

static int nx_boot_image_name_valid(const char *name) {
  if (name == NULL || name[0] == '\0')
    return 0;
  for (unsigned i = 0U; i < NX_BOOT_IMAGE_NAME_MAX; ++i) {
    const char ch = name[i];
    if (ch == '\0')
      return 1;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '_' || ch == '-'))
      return 0;
  }
  return 0;
}

static int nx_boot_image_path_valid(const char *kind, const char *path) {
  const char *rest = NULL;
  if (strcmp(kind, "init") == 0 && strncmp(path, "/sbin/", 6U) == 0)
    rest = path + 6;
  if (strcmp(kind, "service") == 0 && strncmp(path, "/sys/bin/", 9U) == 0)
    rest = path + 9;
  if (strcmp(kind, "service") == 0 &&
      strncmp(path, "/sys/services/", 14U) == 0)
    rest = path + 14;
  if (rest == NULL || rest[0] == '\0')
    return 0;
  for (unsigned i = 0U; i < NX_BOOT_IMAGE_PATH_MAX; ++i) {
    const char ch = rest[i];
    if (ch == '\0')
      return 1;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
      return 0;
  }
  return 0;
}

static int nx_boot_publish_image(char line[NX_BOOT_LINE_BYTES]) {
  char *kind = line;
  char *name = NULL;
  char *path = NULL;
  unsigned separators = 0U;
  for (unsigned i = 0U; i < NX_BOOT_LINE_BYTES && line[i] != '\0'; ++i) {
    if (line[i] == '|') {
      line[i] = '\0';
      if (separators == 0U)
        name = line + i + 1U;
      else if (separators == 1U)
        path = line + i + 1U;
      ++separators;
    }
  }
  if (separators != 2U || name == NULL || path == NULL ||
      (strcmp(kind, "init") != 0 && strcmp(kind, "service") != 0) ||
      !nx_boot_image_name_valid(name) ||
      !nx_boot_image_path_valid(kind, path))
    return -1;

  char key[64] = {0};
  int count = snprintf(key, sizeof(key), "%s%s.%s", NX_BOOT_IMAGE_PREFIX,
                       kind, name);
  if (count <= 0 || count >= (int)sizeof(key) ||
      registry_set(key, path, 0) != 0)
    return -1;
  return strcmp(kind, "init") == 0 ? 1 : 2;
}

static int nx_boot_import_images(void) {
  char catalog[NX_BOOT_CATALOG_BYTES] = {0};
  int size = vfs_read_file(NX_BOOT_IMAGES_CONFIG, catalog,
                           sizeof(catalog) - 1U, 0U);
  if (size <= 0 || size >= (int)sizeof(catalog))
    return -1;
  catalog[size] = '\0';

  unsigned start = 0U;
  unsigned init_count = 0U;
  unsigned service_count = 0U;
  int status = 0;
  for (unsigned i = 0U; i < NX_BOOT_CATALOG_BYTES; ++i) {
    if (status == 0 && i <= (unsigned)size &&
        (catalog[i] == '\n' || catalog[i] == '\0')) {
      catalog[i] = '\0';
      if (i > start && catalog[i - 1U] == '\r')
        catalog[i - 1U] = '\0';
      char *line = catalog + start;
      if (line[0] != '\0' && line[0] != '#') {
        int published = nx_boot_publish_image(line);
        if (published < 0)
          status = -1;
        else if (published == 1)
          ++init_count;
        else
          ++service_count;
      }
      start = i + 1U;
    }
  }
  return status == 0 && init_count > 0U && service_count > 0U ? 0 : -1;
}

static int nx_boot_import_init_image(void) {
  char config[64] = {0};
  int size = vfs_read_file(NX_BOOT_MANAGER_CONFIG, config,
                           sizeof(config) - 1U, 0U);
  if (size <= 5 || size >= (int)sizeof(config))
    return -1;
  config[size] = '\0';
  if (config[size - 1] == '\n')
    config[--size] = '\0';
  if (size > 0 && config[size - 1] == '\r')
    config[--size] = '\0';
  if (strncmp(config, "init=", 5U) != 0 ||
      !nx_boot_image_name_valid(config + 5))
    return -1;

  char key[64] = {0};
  char path[NX_BOOT_IMAGE_PATH_MAX] = {0};
  int count = snprintf(key, sizeof(key), "%s%s", NX_BOOT_INIT_PREFIX,
                       config + 5);
  if (count <= 0 || count >= (int)sizeof(key) ||
      registry_get(key, path, sizeof(path)) != 0)
    return -1;
  return registry_set(NX_BOOT_INIT_IMAGE_KEY, config + 5, 0);
}

static int nx_boot_provider_import(void) {
  if (registry_set(NX_BOOT_REGISTRY_KEY, NEXS_BOOT_CONFIG_ENTRY, 0) != 0)
    return -1;
  if (nx_boot_import_images() != 0)
    return -1;
  return nx_boot_import_init_image();
}

int nx_object_registry_start(void) {
  registry_init();
  registry_mount_vfs();
  procfs_init();
  int result = nx_boot_provider_import();
  if (result != 0) {
    pr_err("%s", "BOOT-OBJECT: cannot import machine boot metadata\n");
    return -1;
  }
  pr_info("%s", "BOOT-OBJECT: machine boot objects published\n");
  return 0;
}
