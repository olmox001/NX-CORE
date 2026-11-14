# NX-CORE standalone build entrypoint.
# This repository owns the kernel source manifest and rules in build.mk.
.DEFAULT_GOAL := all

ARCH ?= aarch64
BUILD ?= debug
BUILD_ROOT ?= build
# MDEBUG enables a separate boot-time userland test shell, not a different HAL.
MDEBUG ?= 0
ifneq ($(filter $(MDEBUG),0 1),$(MDEBUG))
  $(error MDEBUG must be 0 or 1)
endif
HOST_OS ?= $(shell uname -s)
HOST_CC ?= cc
ifeq ($(HOST_OS),Darwin)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else ifeq ($(HOST_OS),Linux)
  AMD64_CROSS_COMPILE ?= x86_64-elf-
  AARCH64_CROSS_COMPILE ?= aarch64-none-elf-
else
  $(error Unsupported host OS $(HOST_OS))
endif
NXSDK_ROOT ?= $(shell tools/resolve-sdk.sh --print 2>/dev/null)
ifneq ($(strip $(NXSDK_ROOT)),)
  include $(NXSDK_ROOT)/config/toolchain-versions.env
endif

ifeq ($(findstring amd64,$(ARCH)),amd64)
  override ARCH := amd64
  CROSS_COMPILE ?= $(AMD64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AMD64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=large
  HAL_ARCH_DIR := HAL/arch/amd64
  QEMU := qemu-system-x86_64
else ifeq ($(findstring aarch64,$(ARCH)),aarch64)
  override ARCH := aarch64
  CROSS_COMPILE ?= $(AARCH64_CROSS_COMPILE)
  ARCH_CFLAGS := -DARCH_AARCH64 -mcpu=cortex-a57
  HAL_ARCH_DIR := HAL/arch/aarch64
  QEMU := qemu-system-aarch64
else
  $(error ARCH must be amd64 or aarch64)
endif

KERNEL_DIR := .
HAL_ROOT := HAL
BOOT_DIR := $(HAL_ARCH_DIR)/boot
BUILD_DIR := $(BUILD_ROOT)/$(ARCH)
CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
AR := $(CROSS_COMPILE)ar
LD := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

COMMON_FLAGS := -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
  -Wmissing-prototypes -Wstrict-prototypes -Wconversion -Wsign-conversion \
  -ffreestanding -fno-builtin \
  -nostdlib -nostartfiles -fno-common -fstack-protector-strong \
  -fno-pic -fno-pie -fno-omit-frame-pointer -O2 -g -DNX_STRICT
# Kernel/HAL diagnostics are blocking by default.  One explicit switch owns
# the policy: every warning class already enabled above/below, including
# conversion/sign-conversion, is fatal; assembler warnings are fatal as well.
# NX_WARNINGS_AS_ERRORS=0 is reserved for deliberate local porting experiments.
# Production/default builds must never add -Wno-error demotions.
NX_WARNINGS_AS_ERRORS ?= 1
ifeq ($(NX_WARNINGS_AS_ERRORS),1)
COMMON_FLAGS += -Werror -Wa,--fatal-warnings
endif
KERNEL_INCLUDE := -I$(HAL_ROOT) -I$(KERNEL_DIR)/include -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
  -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api -I$(BUILD_DIR)/generated
KERNEL_HARDEN_FLAGS := -Wundef -Wformat=2 -Wcast-align -Wpointer-arith \
  -Wswitch-enum -Wdouble-promotion -Wvla -Wmissing-declarations
KERNEL_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) $(KERNEL_INCLUDE) \
  $(KERNEL_HARDEN_FLAGS) -DKERNEL -DNX_MDEBUG_BOOT=$(MDEBUG)

# Make changes to MDEBUG recompile the K3 composition root reliably, even
# when the same output tree is reused. All other kernel objects remain intact.
.PHONY: mdebug-config-force
mdebug-config-force:
MDEBUG_CONFIG := $(BUILD_DIR)/mdebug-config.value
$(MDEBUG_CONFIG): mdebug-config-force
	@mkdir -p $(dir $@)
	@printf '%s\n' "$(MDEBUG)" > "$@.tmp"; \
	  if test -r "$@" && cmp -s "$@.tmp" "$@"; then rm "$@.tmp"; \
	  else mv "$@.tmp" "$@"; fi
$(BUILD_DIR)/main.o $(BUILD_DIR)/core/fault.o: $(MDEBUG_CONFIG)


BOOT_ENTRY_CONFIG := $(KERNEL_DIR)/config/boot-entry.conf
BOOT_PROJECT_CATALOG := $(KERNEL_DIR)/config/user-projects.conf
BOOT_ENTRY_HEADER := $(BUILD_DIR)/generated/boot_entry.h
$(BOOT_ENTRY_HEADER): $(BOOT_ENTRY_CONFIG) $(BOOT_PROJECT_CATALOG) $(KERNEL_DIR)/tools/user-project-catalog.py
	@mkdir -p $(dir $@)
	@python3 $(KERNEL_DIR)/tools/user-project-catalog.py --catalog $(BOOT_PROJECT_CATALOG) \
	  boot-header --entry $(BOOT_ENTRY_CONFIG) > "$@.tmp"
	@mv "$@.tmp" "$@"

$(BUILD_DIR)/$(KERNEL_DIR)/fs/object_boot_provider.o: $(BOOT_ENTRY_HEADER)

KERNEL_ELF := $(BUILD_DIR)/kernel.elf
KERNEL_BIN := $(BUILD_DIR)/kernel.bin
BOOTLOADER_ELF := $(BUILD_DIR)/bootloader.elf
BOOTLOADER_BIN := $(BUILD_DIR)/bootloader.bin
FATAL_LINK_FLAGS := $(if $(filter 1,$(NX_WARNINGS_AS_ERRORS)),--fatal-warnings)
LDFLAGS_BOOT := -nostdlib -static -z noexecstack $(FATAL_LINK_FLAGS) \
  -T $(BOOT_DIR)/linker.ld
LDFLAGS_KERN := -nostdlib -static -z noexecstack $(FATAL_LINK_FLAGS) \
  -T $(HAL_ARCH_DIR)/kernel.ld -Map $(BUILD_DIR)/kernel.map

include build.mk
-include $(DEPS)

.PHONY: all hal check check-contracts check-sdk-boundary check-object-boot-provider check-virtio-ring-layout check-virtio-dma-lifecycle check-gpu-mode-contract check-virtio-gpu-edid check-driver-dma-claim setup-deps require-sdk print-sdk-root clean run boot debug debug-direct disasm symbols test-debug test-nxpax-core test-nxpax-stress help
all: kernel bootloader
hal: $(HAL_LIB)

require-sdk:
	@tools/resolve-sdk.sh --check >/dev/null

print-sdk-root:
	@tools/resolve-sdk.sh --print

check: require-sdk check-contracts check-sdk-boundary check-object-boot-provider check-virtio-ring-layout check-virtio-dma-lifecycle check-gpu-mode-contract check-virtio-gpu-edid check-driver-dma-claim
	@set -eu; for tool in "$(CC)" "$(LD)" "$(OBJCOPY)" "$(CROSS_COMPILE)nm"; do \
	  command -v "$$tool" >/dev/null || { echo "missing tool: $$tool" >&2; exit 1; }; done
	@case "$(ARCH):$$($(CC) -dumpmachine)" in \
	  amd64:x86_64-elf|aarch64:aarch64-none-elf) ;; \
	  *) echo "compiler target mismatch for $(ARCH): $$($(CC) -dumpmachine)" >&2; exit 1;; esac
	@expected_gcc="$(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_GCC_VERSION),$(NEXS_AARCH64_GCC_VERSION))"; actual_gcc="$$($(CC) -dumpfullversion -dumpversion)"; test "$$actual_gcc" = "$$expected_gcc" || { echo "GCC $$actual_gcc; expected $$expected_gcc" >&2; exit 1; }
	@expected_binutils="$(if $(filter amd64,$(ARCH)),$(NEXS_X86_64_BINUTILS_VERSION),$(NEXS_AARCH64_BINUTILS_VERSION))"; actual_binutils="$$($(LD) --version | awk 'NR==1 {print $$NF}')"; test "$$actual_binutils" = "$$expected_binutils" || { echo "binutils $$actual_binutils; expected $$expected_binutils" >&2; exit 1; }
	@echo "NX-CORE $(ARCH): $$($(CC) --version | head -1)"

setup-deps:
	@set -eu; sdk="$$(tools/resolve-sdk.sh --ensure)"; \
	if [ "$(HOST_OS)" = "Darwin" ]; then "$$sdk/tools/setup-toolchain-macos.sh"; \
	elif [ "$(HOST_OS)" = "Linux" ]; then "$$sdk/tools/setup-toolchain-linux.sh"; \
	else echo "Unsupported host OS: $(HOST_OS)" >&2; exit 1; fi

check-contracts:
	@tools/check-contract-snapshot.sh
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -I$(HAL_ROOT)/include \
	  tools/check-boot-info-layout.c -o "$(BUILD_DIR)/host-tests/check-boot-info-layout"
	@"$(BUILD_DIR)/host-tests/check-boot-info-layout"

check-object-boot-provider: $(BOOT_ENTRY_HEADER)
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(BUILD_DIR)/generated -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  fs/object_boot_provider.c test/object_boot_provider_test.c \
	  -o "$(BUILD_DIR)/host-tests/object_boot_provider_test"
	@"$(BUILD_DIR)/host-tests/object_boot_provider_test"

check-virtio-ring-layout:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  -I$(HAL_ROOT) -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
	  test/virtio_ring_layout_test.c -o "$(BUILD_DIR)/host-tests/virtio_ring_layout_test"
	@"$(BUILD_DIR)/host-tests/virtio_ring_layout_test"

check-virtio-dma-lifecycle:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  -I$(HAL_ROOT) -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
	  test/virtio_dma_lifecycle_test.c drivers/virtio/virtio_dma.c \
	  -o "$(BUILD_DIR)/host-tests/virtio_dma_lifecycle_test"
	@"$(BUILD_DIR)/host-tests/virtio_dma_lifecycle_test"

check-gpu-mode-contract:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  -I$(HAL_ROOT) -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
	  test/gpu_mode_contract_test.c drivers/gpu/gpu_core.c \
	  -o "$(BUILD_DIR)/host-tests/gpu_mode_contract_test"
	@"$(BUILD_DIR)/host-tests/gpu_mode_contract_test"

check-virtio-gpu-edid:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  test/virtio_gpu_edid_test.c drivers/gpu/virtio_gpu_edid.c \
	  -o "$(BUILD_DIR)/host-tests/virtio_gpu_edid_test"
	@"$(BUILD_DIR)/host-tests/virtio_gpu_edid_test"

check-driver-dma-claim:
	@mkdir -p "$(BUILD_DIR)/host-tests"
	@$(HOST_CC) -std=c11 -Wall -Wextra -Werror -include test/host_kernel_compat.h \
	  -I$(KERNEL_DIR)/include -I$(KERNEL_DIR)/include/abi -I$(KERNEL_DIR)/include/api \
	  -I$(HAL_ROOT) -I$(HAL_ROOT)/include -I$(HAL_ARCH_DIR)/include \
	  -D_KERNEL_PRINTK_H '-Dpr_info(...)=((void)0)' \
	  '-Dpr_warn(...)=((void)0)' '-Dpr_err(...)=((void)0)' \
	  core/hal_bus.c test/driver_dma_claim_test.c \
	  -o "$(BUILD_DIR)/host-tests/driver_dma_claim_test"
	@"$(BUILD_DIR)/host-tests/driver_dma_claim_test"

check-sdk-boundary:
	@set -eu; \
	case "$(KERNEL_CFLAGS)" in *"$(NXSDK_ROOT)/include"*) \
	  echo "NX-CORE boundary violation: kernel CFLAGS include NX-SDK headers" >&2; exit 1;; esac; \
	case "$(LDFLAGS_KERN) $(KERN_OBJECTS) $(HAL_LIB)" in *libnxsdk*|*"$(NXSDK_ROOT)/runtime"*) \
	  echo "NX-CORE boundary violation: kernel image links NX-SDK userland runtime" >&2; exit 1;; esac; \
	if { find core lib drivers HAL -type f \
	       \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.s' \) -print; \
	     printf '%s\n' main.c; } | xargs grep -n -E \
	       '#[[:space:]]*include[[:space:]]*[<"]([^">]*/)?system/include|NEXS_SDK_ROOT|NXSDK_ROOT' 2>/dev/null; then \
	  echo "NX-CORE boundary violation: compiled kernel/HAL source reaches into NX-SDK implementation" >&2; exit 1; \
	fi; \
	echo "NX-CORE SDK boundary: PASS (SDK development tools only; no userland API/runtime in kernel image)"

clean:
	rm -rf "$(BUILD_DIR)"

UEFI_CODE ?= $(firstword $(wildcard /usr/local/share/qemu/edk2-aarch64-code.fd /usr/local/Cellar/qemu/*/share/qemu/edk2-aarch64-code.fd /opt/homebrew/share/qemu/edk2-aarch64-code.fd /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI_CODE.fd))
UEFI_VARS_TEMPLATE ?= $(firstword $(wildcard /usr/local/share/qemu/edk2-arm-vars.fd /usr/local/Cellar/qemu/*/share/qemu/edk2-arm-vars.fd /opt/homebrew/share/qemu/edk2-arm-vars.fd /usr/share/AAVMF/AAVMF_VARS.fd /usr/share/qemu-efi-aarch64/QEMU_VARS.fd))
.PHONY: boot-iso run-direct grub-arm64-tools
grub-arm64-tools:
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-grub-arm64.sh

ifeq ($(ARCH),amd64)
boot-iso: kernel
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-amd64-grub-iso.sh
run: boot-iso
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -cdrom $(BUILD_DIR)/grub-bios.iso
run-direct: kernel
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -kernel "$(KERNEL_ELF)"
else
$(BUILD_DIR)/virt.dtb:
	@mkdir -p $(dir $@)
	$(QEMU) -M virt -cpu cortex-a57 -m 5G -smp 4 \
	  -device virtio-gpu-device -device virtio-keyboard-device \
	  -device virtio-mouse-device -machine dumpdtb=$@
boot-iso: kernel $(BUILD_DIR)/virt.dtb
	@NEXS_BUILD_ROOT="$(abspath $(BUILD_ROOT))" tools/build-aarch64-uefi-image.sh
run: boot-iso
	@test -f "$(UEFI_CODE)" || { echo "Missing AArch64 EDK2 code image; set UEFI_CODE" >&2; exit 1; }
	@test -f "$(UEFI_VARS_TEMPLATE)" || { echo "Missing AArch64 EDK2 vars template; set UEFI_VARS_TEMPLATE" >&2; exit 1; }
	@cp "$(UEFI_VARS_TEMPLATE)" "$(BUILD_DIR)/grub-efi/vars.fd"
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -boot order=d \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file="$(UEFI_CODE)" \
	  -drive if=pflash,format=raw,unit=1,file="$(BUILD_DIR)/grub-efi/vars.fd" \
	  -device virtio-gpu-device,id=nxgpu0 -device virtio-keyboard-device,id=nxkbd0,display=nxgpu0 \
	  -device virtio-mouse-device,id=nxmouse0,display=nxgpu0 \
	  -device virtio-scsi-device,id=scsi0 \
	  -drive if=none,file=$(BUILD_DIR)/grub-efi/nexsos-aarch64-uefi.iso,id=cdrom,media=cdrom,format=raw \
	  -device scsi-cd,drive=cdrom
run-direct: kernel $(BUILD_DIR)/virt.dtb
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio \
	  -display none -no-reboot -dtb $(BUILD_DIR)/virt.dtb -kernel "$(KERNEL_BIN)"
endif

debug: boot-iso
ifeq ($(ARCH),amd64)
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio -display none -no-reboot -s -S -cdrom $(BUILD_DIR)/grub-bios.iso
else
	@test -f "$(UEFI_CODE)" || { echo "Missing AArch64 EDK2 code image; set UEFI_CODE" >&2; exit 1; }
	@test -f "$(UEFI_VARS_TEMPLATE)" || { echo "Missing AArch64 EDK2 vars template; set UEFI_VARS_TEMPLATE" >&2; exit 1; }
	@cp "$(UEFI_VARS_TEMPLATE)" "$(BUILD_DIR)/grub-efi/vars.fd"
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio -display none -no-reboot -s -S -boot order=d \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file="$(UEFI_CODE)" \
	  -drive if=pflash,format=raw,unit=1,file="$(BUILD_DIR)/grub-efi/vars.fd" \
	  -device virtio-scsi-device,id=scsi0 \
	  -drive if=none,file=$(BUILD_DIR)/grub-efi/nexsos-aarch64-uefi.iso,id=cdrom,media=cdrom,format=raw \
	  -device scsi-cd,drive=cdrom
endif

debug-direct: kernel
ifeq ($(ARCH),amd64)
	$(QEMU) -machine q35 -cpu max -m 5G -smp 4 -serial stdio -display none -no-reboot -s -S -kernel "$(KERNEL_ELF)"
else
	$(QEMU) -machine virt -cpu cortex-a57 -m 5G -smp 4 -serial stdio -display none -no-reboot -s -S -dtb $(BUILD_DIR)/virt.dtb -kernel "$(KERNEL_BIN)"
endif

disasm: kernel bootloader
	$(OBJDUMP) -d $(KERNEL_ELF) > $(BUILD_DIR)/kernel.disasm
	$(OBJDUMP) -d $(BOOTLOADER_ELF) > $(BUILD_DIR)/bootloader.disasm

symbols: kernel
	$(CROSS_COMPILE)nm -n $(KERNEL_ELF) > $(BUILD_DIR)/kernel.symbols

test-nxpax-core:
	@mkdir -p $(BUILD_ROOT)/tests/nxpax
	@clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wpedantic \
	  -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -IHAL/include -Iinclude test/nxpax_core_test.c core/nxpax.c HAL/core/nxpax.c \
	  -o $(BUILD_ROOT)/tests/nxpax/core_test
	@$(BUILD_ROOT)/tests/nxpax/core_test

test-nxpax-stress:
	@mkdir -p $(BUILD_ROOT)/tests/nxpax
	@clang -std=c17 -O1 -g -Wall -Wextra -Werror -Wpedantic \
	  -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
	  -IHAL/include HAL/tests/unit/nxpax_stress_test.c HAL/core/nxpax.c \
	  -o $(BUILD_ROOT)/tests/nxpax/stress_test
	@$(BUILD_ROOT)/tests/nxpax/stress_test

test-debug: test-nxpax-core test-nxpax-stress
	@$(MAKE) -C HAL test-host

boot: run

help:
	@echo "NX-CORE standalone: make [ARCH=amd64|aarch64] all"
	@echo "Targets: hal kernel bootloader machine-extension check run/boot debug debug-direct disasm symbols test-debug clean"

# NX-CORE-owned machine extension. It runs in user mode but remains a kernel
# repository artifact; NX-VIOLA never reaches into kernel source to build it.
NXSDK_SOURCE_LAYOUT := $(wildcard $(NXSDK_ROOT)/lib/lib.c)
NXSDK_MACHINE_BUILD_ROOT := $(abspath $(BUILD_ROOT))/sdk-machine
ifeq ($(strip $(NXSDK_SOURCE_LAYOUT)),)
  NXSDK_MACHINE_LIB := $(firstword $(wildcard $(NXSDK_ROOT)/runtime/$(ARCH)/lib/libnxsdk-core.a $(NXSDK_ROOT)/runtime/$(ARCH)/lib/libnxsdk.a))
  NXSDK_MACHINE_CRT := $(NXSDK_ROOT)/runtime/$(ARCH)/crt/syscall.o
else
  NXSDK_MACHINE_LIB := $(NXSDK_MACHINE_BUILD_ROOT)/$(ARCH)/libnxsdk-core.a
  NXSDK_MACHINE_CRT := $(NXSDK_MACHINE_BUILD_ROOT)/$(ARCH)/system/arch/$(ARCH)/syscall.o
endif
MACHINE_EXT_OBJ := $(BUILD_DIR)/user/extension/nxmanageDD/main.o
MACHINE_EXT_ELF := $(BUILD_DIR)/nxmanageDD.elf
NXSDK_ROOT_FOR_EXT := $(NXSDK_ROOT)
-include $(NXSDK_ROOT)/config/extension-paths.mk
-include $(NXSDK_ROOT)/config/userkit-paths.mk
ifeq ($(ARCH),amd64)
MACHINE_EXT_STACK_FLAGS := -Wa,--noexecstack
else
MACHINE_EXT_STACK_FLAGS :=
endif
MACHINE_EXT_CFLAGS := $(COMMON_FLAGS) $(ARCH_CFLAGS) $(MACHINE_EXT_STACK_FLAGS) \
  -I$(NXSDK_ROOT)/include/contract -I$(NXSDK_ROOT)/include/api \
  $(NXSDK_EXTENSION_INCLUDE_FLAGS) $(NXSDK_USERKIT_INCLUDE_FLAGS) -ffunction-sections -fdata-sections

.PHONY: machine-sdk machine-extension
machine-sdk: require-sdk
ifneq ($(strip $(NXSDK_SOURCE_LAYOUT)),)
	+$(MAKE) -C $(NXSDK_ROOT) ARCH=$(ARCH) BUILD_ROOT="$(NXSDK_MACHINE_BUILD_ROOT)" core-archive
else
	@test -n "$(NXSDK_MACHINE_LIB)" && test -s "$(NXSDK_MACHINE_LIB)" || { echo "NX-CORE: installed SDK core/full runtime missing for $(ARCH)" >&2; exit 1; }
	@test -s "$(NXSDK_MACHINE_CRT)" || { echo "NX-CORE: installed SDK startup object missing for $(ARCH)" >&2; exit 1; }
endif

$(MACHINE_EXT_OBJ): user/extension/nxmanageDD/main.c | machine-sdk
	@mkdir -p $(dir $@)
	@$(CC) $(MACHINE_EXT_CFLAGS) -MMD -MP -c $< -o $@

$(MACHINE_EXT_ELF): $(MACHINE_EXT_OBJ) | machine-sdk
	@$(CC) $(MACHINE_EXT_CFLAGS) -Wl,--gc-sections -Wl,-Ttext=0x80000000 -e _start \
	  -o $@ $(MACHINE_EXT_OBJ) $(NXSDK_MACHINE_CRT) \
	  -Wl,--start-group $(NXSDK_MACHINE_LIB) -lgcc -Wl,--end-group

machine-extension: $(MACHINE_EXT_ELF)
	@echo "NX-CORE machine extension: $(MACHINE_EXT_ELF)"
