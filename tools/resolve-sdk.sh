#!/usr/bin/env bash
set -euo pipefail
CORE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$CORE_ROOT/config/sdk-dependency.env"
MODE="${1:---print}"
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
INSTALL_PREFIX="${NEXS_SDK_INSTALL_PREFIX:-$DATA_HOME/nexsos/sdk}"
CACHE_ROOT="${NEXS_SDK_CACHE_ROOT:-$CORE_ROOT/.nxsdk}"
CACHE_SOURCE="$CACHE_ROOT/source"

valid_tools_sdk() {
  local root="$1" tools_abi=0
  [ -n "$root" ] || return 1
  [ -f "$root/config/toolchain-versions.env" ] || return 1
  [ -x "$root/tools/setup-toolchain-linux.sh" ] || return 1
  [ -x "$root/tools/setup-toolchain-macos.sh" ] || return 1
  if [ -f "$root/config/sdk-version.env" ]; then
    # shellcheck disable=SC1090
    source "$root/config/sdk-version.env"
    tools_abi="${NEXS_SDK_TOOLS_ABI:-0}"
    [ "$tools_abi" -ge "$NEXS_SDK_REQUIRED_TOOLS_ABI" ] || return 1
  fi
}

integrated_sdk() {
  local super control sdk
  super="$(git -C "$CORE_ROOT" rev-parse --show-superproject-working-tree 2>/dev/null || true)"
  [ -n "$super" ] || return 1
  control="$super/tools/setup-dependencies.sh"
  [ -x "$control" ] || return 1
  sdk="$($control --locate system 2>/dev/null)" || return 1
  valid_tools_sdk "$sdk" || return 1
  printf '%s\n' "$sdk"
}

find_sdk() {
  local candidate
  if [ -n "${NEXS_SDK_ROOT:-}" ] && valid_tools_sdk "$NEXS_SDK_ROOT"; then
    (cd "$NEXS_SDK_ROOT" && pwd); return 0
  fi
  if candidate="$(integrated_sdk 2>/dev/null)"; then printf '%s\n' "$candidate"; return 0; fi
  for candidate in "$INSTALL_PREFIX/current" "$CACHE_SOURCE"; do
    if valid_tools_sdk "$candidate"; then (cd "$candidate" && pwd); return 0; fi
  done
  return 1
}

ensure_sdk() {
  local found
  if found="$(find_sdk 2>/dev/null)"; then printf '%s\n' "$found"; return 0; fi
  command -v git >/dev/null 2>&1 || { echo "NX-CORE: git is required to bootstrap NX-SDK development tools" >&2; return 1; }
  mkdir -p "$CACHE_ROOT"
  if [ ! -d "$CACHE_SOURCE/.git" ]; then
    git clone "$NEXS_SDK_REPOSITORY" "$CACHE_SOURCE"
  fi
  git -C "$CACHE_SOURCE" fetch origin "$NEXS_SDK_REF"
  git -C "$CACHE_SOURCE" checkout --detach FETCH_HEAD
  valid_tools_sdk "$CACHE_SOURCE" || {
    echo "NX-CORE: fetched NX-SDK does not provide the required development-tool ABI" >&2
    return 1
  }
  (cd "$CACHE_SOURCE" && pwd)
}

case "$MODE" in
  --print) find_sdk ;;
  --ensure) ensure_sdk ;;
  --check)
    sdk="$(find_sdk)" || { echo "NX-CORE: NX-SDK development tools not found; run 'make setup-deps'" >&2; exit 1; }
    printf 'NX-CORE SDK tools: %s\n' "$sdk"
    ;;
  *) echo "usage: $0 [--print|--ensure|--check]" >&2; exit 2 ;;
esac
