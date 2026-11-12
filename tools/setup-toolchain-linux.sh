#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_ROOT="${NEXS_SDK_ROOT:-$("$ROOT/tools/resolve-sdk.sh" --ensure)}"
exec "$SDK_ROOT/tools/setup-toolchain-linux.sh" "$@"
