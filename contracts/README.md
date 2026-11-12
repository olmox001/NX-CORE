# NX-CORE local contract snapshot

NX-CORE source and runtime remain independent from userland. A standalone NX-CORE checkout may use NX-SDK development tooling (toolchain locks, setup/build/debug tools), but the kernel image must not include or link NX-SDK userland APIs, libc/POSIX surfaces, or runtime libraries.

`contracts/version.json` defines the kernel contract scopes. The `abi` scope covers every header in `include/abi/`; changing any of those headers requires increasing its revision. The `support` scope covers `include/api/` loader/compiler support headers and is fully checksummed but is not a userland ABI revision.

`tools/check-contract-snapshot.py` verifies that each configured scope snapshot enumerates every header exactly once, rejects stale or missing entries, validates SHA-256 content hashes, and checks revision bumps against both the current worktree and the previous committed revision when Git history is available. `make check-contracts` runs this gate.

The NexsOS1 integration repository performs the additional cross-repository compatibility check against the pinned NX-SDK gitlink. `make check-sdk-boundary` enforces that SDK use by NX-CORE stays in the development-tool plane; kernel and HAL compile against their kernel-owned contract snapshots, not NX-SDK userland headers.
