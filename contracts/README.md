# NX-CORE local contract snapshot

NX-CORE builds and checks from its own checkout without NX-SDK or NX-VIOLA.

`contracts/version.json` defines the kernel contract scopes. The `abi` scope covers every header in `include/abi/`; changing any of those headers requires increasing its revision. The `support` scope covers `include/api/` loader/compiler support headers and is fully checksummed but is not a userland ABI revision.

`tools/check-contract-snapshot.py` verifies that each configured scope snapshot enumerates every header exactly once, rejects stale or missing entries, validates SHA-256 content hashes, and checks revision bumps against both the current worktree and the previous committed revision when Git history is available. `make check-contracts` runs this gate.

The NexsOS1 integration repository performs the additional cross-repository compatibility check against the pinned NX-SDK gitlink. NX-CORE never includes headers directly from NX-SDK.
