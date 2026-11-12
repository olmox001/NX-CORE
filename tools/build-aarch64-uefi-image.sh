#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GRUB_MANIFEST="$ROOT/config/grub.env"
set -a
SDK_ROOT="${NEXS_SDK_ROOT:-$("$ROOT/tools/resolve-sdk.sh" --print)}"
source "$SDK_ROOT/config/toolchain-versions.env"
source "$GRUB_MANIFEST"
set +a

BUILD_ROOT="${NEXS_BUILD_ROOT:-$ROOT/build}"
case "$BUILD_ROOT" in
    /*) ;;
    *) echo "[aarch64-uefi] NEXS_BUILD_ROOT must be absolute: $BUILD_ROOT" >&2; exit 1 ;;
esac
BUILD="$BUILD_ROOT/aarch64"
GRUB_STAGE="$BUILD_ROOT/cache/grub/prebuilt/arm64-efi"
GRUB_MKIMAGE="$GRUB_STAGE/bin/grub-mkimage"
GRUB_MODULE_DIR="$GRUB_STAGE/lib/grub/arm64-efi"
LOG_DIR="$BUILD_ROOT/logs"
EFI_ROOT="$BUILD/grub-efi"
EFI_IMAGE="$EFI_ROOT/efi.img"
ISO_STAGE="$EFI_ROOT/iso"
ISO="$EFI_ROOT/nexsos-aarch64-uefi.iso"
KERNEL="${NEXS_KERNEL_IMAGE:-$BUILD/kernel.bin}"
DTB="${NEXS_DTB_PATH:-$BUILD/virt.dtb}"
CATALOG="$ROOT/config/user-projects.conf"
CATALOG_TOOL="$ROOT/tools/user-project-catalog.py"
USER_IMAGES="${NEXS_USER_IMAGE_DIR:-$BUILD/user-images}"
LOADER_OUT="$EFI_ROOT/user-loaders"
LOADER_STAGE="$ISO_STAGE/boot/user-loaders"
HOST_CLANG="${NEXS_HOST_CLANG:-$(command -v clang || true)}"
LLD_LINK="${NEXS_LLD_LINK:-$(command -v lld-link || true)}"

if [[ -z "$LLD_LINK" ]]; then
    for candidate in /usr/local/opt/llvm/bin/lld-link /opt/homebrew/opt/llvm/bin/lld-link; do
        if [[ -x "$candidate" ]]; then LLD_LINK="$candidate"; break; fi
    done
fi
if [[ -z "$LLD_LINK" ]] && command -v brew >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm 2>/dev/null || true)"
    if [[ -n "$LLVM_PREFIX" && -x "$LLVM_PREFIX/bin/lld-link" ]]; then
        LLD_LINK="$LLVM_PREFIX/bin/lld-link"
    fi
fi

for tool in xorriso mformat mmd mcopy python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "[aarch64-uefi] missing required tool: $tool" >&2
        exit 1
    }
done
[[ -n "$HOST_CLANG" && -x "$HOST_CLANG" ]] || {
    echo "[aarch64-uefi] clang is required" >&2
    exit 1
}
[[ -n "$LLD_LINK" && -x "$LLD_LINK" ]] || {
    echo "[aarch64-uefi] lld-link is required" >&2
    exit 1
}
[[ -s "$KERNEL" ]] || {
    echo "[aarch64-uefi] missing kernel image: $KERNEL" >&2
    exit 1
}
[[ -s "$DTB" ]] || {
    echo "[aarch64-uefi] missing Device Tree: $DTB" >&2
    exit 1
}

"$ROOT/tools/build-grub-arm64.sh"
[[ -x "$GRUB_MKIMAGE" && -f "$GRUB_MODULE_DIR/chain.mod" ]] || {
    echo "[aarch64-uefi] reusable GRUB arm64-efi stage is incomplete" >&2
    exit 1
}

mkdir -p "$BUILD" "$EFI_ROOT" "$LOADER_OUT" "$LOG_DIR"
EFI_CFLAGS=(
    --target=aarch64-pc-windows-msvc
    -ffreestanding -fno-stack-protector -fno-builtin -fno-exceptions
    -fno-unwind-tables -fno-asynchronous-unwind-tables -mgeneral-regs-only
    -O2 -Wall -Wextra -Werror
    -I"$ROOT/HAL" -I"$ROOT/HAL/arch/aarch64/include"
    -I"$ROOT/HAL/include" -I"$ROOT/include"
)

"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11     -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_stub.c"     -o "$BUILD/efi_stub.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11     -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_exit_adapter.c"     -o "$BUILD/efi_exit_adapter.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11     -c "$ROOT/HAL/capability/uefi_exit.c"     -o "$BUILD/uefi_exit.obj"
"$HOST_CLANG" "${EFI_CFLAGS[@]}" -std=c11     -c "$ROOT/HAL/capability/uefi_memory_map.c"     -o "$BUILD/uefi_memory_map.obj"
"$HOST_CLANG" --target=aarch64-pc-windows-msvc -ffreestanding     -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables     -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_handoff.S"     -o "$BUILD/efi_handoff.obj"

KERNEL_ABS="$(cd "$(dirname "$KERNEL")" && pwd)/$(basename "$KERNEL")"
DTB_ABS="$(cd "$(dirname "$DTB")" && pwd)/$(basename "$DTB")"

case "$ISO_STAGE" in
    "$BUILD_ROOT"/aarch64/grub-efi/iso) ;;
    *) echo "unsafe ISO staging path: $ISO_STAGE" >&2; exit 1 ;;
esac
rm -rf "$ISO_STAGE" "$LOADER_OUT"
mkdir -p "$ISO_STAGE/boot/grub" "$LOADER_STAGE" "$LOADER_OUT" "$EFI_ROOT/EFI/BOOT"

while IFS='|' read -r project_id title image marker; do
    [[ -n "$project_id" && -n "$title" && -n "$image" ]] || {
        echo "[aarch64-uefi] malformed validated catalog row" >&2
        exit 1
    }
    ROOTFS="$USER_IMAGES/$image"
    [[ -s "$ROOTFS" ]] || {
        echo "[aarch64-uefi] missing project image: $ROOTFS" >&2
        exit 1
    }
    ROOTFS_ABS="$(cd "$(dirname "$ROOTFS")" && pwd)/$(basename "$ROOTFS")"
    PAYLOAD_OBJ="$BUILD/efi_payload-$project_id.obj"
    EFI_LOADER="$LOADER_OUT/$project_id.efi"
    "$HOST_CLANG" --target=aarch64-pc-windows-msvc -ffreestanding \
        -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
        -DNEXS_EFI_KERNEL_IMAGE="\"$KERNEL_ABS\"" \
        -DNEXS_EFI_DTB_IMAGE="\"$DTB_ABS\"" \
        -DNEXS_EFI_ROOTFS_IMAGE="\"$ROOTFS_ABS\"" \
        -c "$ROOT/HAL/arch/aarch64/boot/uefi/efi_payload.S" \
        -o "$PAYLOAD_OBJ"
    "$LLD_LINK" /subsystem:efi_application /entry:efi_main /nodefaultlib         /machine:arm64 /out:"$EFI_LOADER"         "$BUILD/efi_stub.obj" "$BUILD/efi_exit_adapter.obj"         "$BUILD/uefi_exit.obj" "$BUILD/uefi_memory_map.obj"         "$BUILD/efi_handoff.obj" "$PAYLOAD_OBJ"
    cp "$EFI_LOADER" "$LOADER_STAGE/$project_id.efi"
done < <(python3 "$CATALOG_TOOL" --catalog "$CATALOG" list)

cat > "$EFI_ROOT/embedded.cfg" <<'GRUB_EMBEDDED'
search --no-floppy --file --set=root /boot/grub/grub.cfg
configfile /boot/grub/grub.cfg
GRUB_EMBEDDED

"$GRUB_MKIMAGE" -O arm64-efi     -d "$GRUB_MODULE_DIR"     -o "$EFI_ROOT/EFI/BOOT/BOOTAA64.EFI"     -p /boot/grub -c "$EFI_ROOT/embedded.cfg"     normal chain iso9660 search search_fs_file configfile echo     fat part_gpt help reboot halt

python3 - "$EFI_IMAGE" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
path.parent.mkdir(parents=True, exist_ok=True)
with path.open("wb") as stream:
    stream.truncate(8 * 1024 * 1024)
PY
mformat -i "$EFI_IMAGE" -v NEXSAA64 ::
mmd -i "$EFI_IMAGE" ::/EFI
mmd -i "$EFI_IMAGE" ::/EFI/BOOT
mcopy -i "$EFI_IMAGE" "$EFI_ROOT/EFI/BOOT/BOOTAA64.EFI" ::/EFI/BOOT/BOOTAA64.EFI

cp "$EFI_IMAGE" "$ISO_STAGE/boot/grub/efi.img"
cat > "$ISO_STAGE/boot/grub/grub.cfg" <<'GRUB_HEADER'
set timeout=5
echo "NEXSOS_GRUB_ARM64_PROJECT_CATALOG"
GRUB_HEADER
python3 "$CATALOG_TOOL" --catalog "$CATALOG" grub-aarch64     --loader-dir /boot/user-loaders >> "$ISO_STAGE/boot/grub/grub.cfg"

xorriso -as mkisofs -R -J -V NEXSAA64     -o "$ISO" -e boot/grub/efi.img -no-emul-boot "$ISO_STAGE"     > "$LOG_DIR/xorriso-aarch64-uefi.log" 2>&1
xorriso -indev "$ISO" -report_el_torito plain     > "$LOG_DIR/aarch64-uefi-el-torito.txt" 2>&1
grep -Eiq 'Platform Id: *0xEF|UEFI' "$LOG_DIR/aarch64-uefi-el-torito.txt" || {
    echo "[aarch64-uefi] ISO has no UEFI El Torito boot entry" >&2
    cat "$LOG_DIR/aarch64-uefi-el-torito.txt" >&2
    exit 1
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}
{
    echo "architecture=aarch64"
    echo "firmware=UEFI"
    echo "bootloader=GNU GRUB ${NEXS_GRUB_VERSION:-2.14} (arm64-efi)"
    echo "catalog=kernel/config/user-projects.conf"
    echo "kernel_sha256=$(sha256_file "$KERNEL")"
    echo "dtb_sha256=$(sha256_file "$DTB")"
    while IFS='|' read -r project_id title image marker; do
        echo "loader_${project_id}_sha256=$(sha256_file "$LOADER_OUT/$project_id.efi")"
    done < <(python3 "$CATALOG_TOOL" --catalog "$CATALOG" list)
    echo "iso_sha256=$(sha256_file "$ISO")"
} > "$EFI_ROOT/manifest.txt"

echo "[aarch64-uefi] catalog bootable ISO ready: $ISO"
