/*
 * Storage adapter for the object/registry boot contract.
 *
 * GRUB selects the root image. This adapter imports only the selected image's
 * startup locator into the machine registry. It does not build an executable
 * catalog and nxmanageDD never reads VFS configuration.
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

enum { NX_BOOT_MANAGER_BYTES = NX_BOOT_EXEC_PATH_MAX + 8U };

void nx_object_storage_mount(void) {
  if (vfs_register_fs(&ext4_fs_ops) != 0)
    panic("BOOT-OBJECT: storage provider registration failed");
  vfs_init();
  pr_info("%s", "BOOT-OBJECT: storage provider mounted\n");
}

static int nx_boot_import_init_exec(void) {
  char config[NX_BOOT_MANAGER_BYTES] = {0};
  int size = vfs_read_file(NX_BOOT_MANAGER_CONFIG, config,
                           sizeof(config) - 1U, 0U);
  if (size <= 5 || size >= (int)sizeof(config))
    return -1;
  config[size] = '\0';
  if (config[size - 1] == '\n')
    config[--size] = '\0';
  if (size > 0 && config[size - 1] == '\r')
    config[--size] = '\0';
  const char *exec = config + 5;
  if (strncmp(config, "init=", 5U) != 0 || exec[0] != '/' ||
      exec[1] == '\0')
    return -1;
  return registry_set(NX_BOOT_INIT_EXEC_KEY, exec, 0);
}

static int nx_boot_provider_import(void) {
  if (registry_set(NX_BOOT_REGISTRY_KEY, NEXS_BOOT_CONFIG_ENTRY, 0) != 0)
    return -1;
  return nx_boot_import_init_exec();
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
