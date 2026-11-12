#include <stddef.h>
#include <stdio.h>
#include <hal/boot_info.h>

int main(void) {
  int failed = 0;
  if (sizeof(struct nxhal_boot_info) != NXHAL_BOOT_INFO_SIZE) failed = 1;
  if (offsetof(struct nxhal_boot_info, fdt_phys) != NXHAL_BOOT_INFO_FDT_PHYS_OFFSET) failed = 1;
  if (offsetof(struct nxhal_boot_info, firmware_map_phys) != NXHAL_BOOT_INFO_FW_MAP_PHYS_OFFSET) failed = 1;
  if (offsetof(struct nxhal_boot_info, boot_module_phys) != NXHAL_BOOT_INFO_MODULE_PHYS_OFFSET) failed = 1;
  if (offsetof(struct nxhal_boot_info, boot_module_size) != NXHAL_BOOT_INFO_MODULE_SIZE_OFFSET) failed = 1;
  if (offsetof(struct nxhal_boot_info, reserved0) != NXHAL_BOOT_INFO_RESERVED0_OFFSET) failed = 1;
  if (failed != 0) {
    fputs("NX-HAL boot-info layout: FAIL\n", stderr);
    return 1;
  }
  puts("NX-HAL boot-info layout: PASS");
  return 0;
}
