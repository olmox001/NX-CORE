# NX-CORE

NX-CORE is the standalone NexsOS1 kernel repository. It owns the kernel,
architecture HALs, kernel drivers, kernel tests, kernel linker scripts,
bootloader inputs, symbol generation, and all kernel build rules.

It does not require the NexsOS userland or the NexsOS1 integration superproject to compile and boot the kernel. NX-CORE does use the **NX-SDK development-tool plane** for the pinned compiler/toolchain contract and shared build/debug tooling. This is not a userland API/runtime dependency: kernel/HAL sources use NX-CORE-owned headers and the kernel image does not link the SDK runtime.

## Prerequisites

- macOS or Linux host.
- NX-SDK development tooling, resolved from the integrated superproject, an installed SDK, `NEXS_SDK_ROOT`, or the standalone SDK cache. The canonical toolchain lock is SDK-owned.
- QEMU for boot tests.
- Pinned external GRUB source at `external/grub` plus the required host ISO tools for amd64 boot media.
- For aarch64 UEFI media: curl, make, gawk, mtools, xorriso, the pinned AArch64
  cross-toolchain, and an EDK2/AAVMF code+vars firmware pair for QEMU execution.

Resolve/install the SDK-owned development toolchain and check the kernel boundary:

    make setup-deps
    make check ARCH=amd64
    make check ARCH=aarch64
    make check-sdk-boundary ARCH=amd64
    make check-sdk-boundary ARCH=aarch64

## Build independently

    make clean all ARCH=amd64
    make clean all ARCH=aarch64

Outputs are under build/amd64 and build/aarch64. The standalone build compiles
only NX-CORE and its bootloader inputs; it does not build userland, SDK
libraries, rootfs, disk images, or release packages.

## Boot independently through GRUB

    make boot-iso ARCH=amd64
    make run ARCH=amd64

    make boot-iso ARCH=aarch64
    make run ARCH=aarch64 UEFI_CODE=/path/to/edk2-aarch64-code.fd UEFI_VARS_TEMPLATE=/path/to/edk2-arm-vars.fd

The amd64 route uses the pinned `external/grub` source for GRUB BIOS. The AArch64 route builds GRUB arm64-efi from the same nested upstream source and the project EFI handoff image. Both run without a rootfs or userland image;
K3 reports the expected kernel-alone mode when /sbin/nxinit is absent.

For low-level QEMU diagnostics without GRUB, use `run-direct`. The standalone debug surface also provides `debug`, `debug-direct`, `disasm`, `symbols`, `test-nxpax-core`, `test-nxpax-stress` and `test-debug`.

## Build ownership

- build.mk is the single canonical kernel source manifest and rule set.
- Makefile is the standalone entrypoint and host/target configuration.
- The superproject invokes this Makefile; it does not include or duplicate
  kernel source manifests, flags, or compile/link rules.
- contracts/ contains the local legacy contract snapshot and checksum.
  Future public SDK declarations must be generated from versioned normative
  specs and verified with compatibility tests; do not revive a shared root
  include/abi directory.
- tools/ contains kernel-owned build helpers and host bootstrap scripts.

## Cleaning

    make clean ARCH=amd64
    make clean ARCH=aarch64

Only the selected architecture's module build output is removed.
