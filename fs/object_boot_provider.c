/*
 * Storage provider for the object/registry boot contract.
 * This is the filesystem adapter, NOT the core K3 bootstrap policy.
 *
 * It is the sole owner of converting an on-image boot file into the
 * authoritative machine namespace object named by NX_BOOT_REGISTRY_KEY.
 * The kernel composition root never reads a path or parses a VFS manifest.
 */
#include <kernel/object_boot.h>
#include <boot_entry.h>
#include <kernel/ext4.h>
#include <kernel/procfs.h>
#include <kernel/registry.h>
#include <kernel/vfs.h>
#include <kernel/printk.h>
#include <kernel/string.h>

void nx_object_storage_mount(void) {
  if (vfs_register_fs(&ext4_fs_ops) != 0)
    panic("BOOT-OBJECT: storage provider registration failed");
  vfs_init();
  pr_info("%s", "BOOT-OBJECT: storage provider mounted\n");
}

static int nx_boot_provider_import(void) {
  return registry_set(NX_BOOT_REGISTRY_KEY, NEXS_BOOT_CONFIG_ENTRY, 0);
}

int nx_object_registry_start(void) {
  registry_init();
  registry_mount_vfs();
  procfs_init();
  int result = nx_boot_provider_import();
  if (result != 0) {
    pr_err("%s", "BOOT-OBJECT: cannot import boot entry to registry\n");
    return -1;
  }
  pr_info("%s", "BOOT-OBJECT: boot entry published to registry object\n");
  return 0;
}
