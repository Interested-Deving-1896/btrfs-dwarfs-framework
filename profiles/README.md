# BDFS target profiles

These versioned JSON profiles describe target identity, kernel family, build
inputs, filesystem capabilities, provenance and the current BDFS compatibility
claim. They are descriptive inputs, **not proof that a kernel adapter exists**.

## Target status

| Profile | Kernel family | Current claim |
|---|---|---|
| `linux-baseline.json` | Linux | Existing Btrfs/DwarFS implementation baseline |
| `chromiumos-linux.json` | Linux | Linux downstream profile only |
| `android-gki-linux.json` | Linux | Linux/GKI downstream profile only |
| `freebsd-native.json` | FreeBSD | Read-only ingestion; native adapter not implemented |
| `ravynos-xnu.json` | XNU | Read-only ingestion; native adapter not implemented |

The standalone `xnu-kernel` concept is represented by the RavynOS/XNU
profile's `kernel_family: xnu`; XNU is a kernel family, not an additional OS
distribution. A separate generic XNU profile can be added when a concrete
SDK/toolchain target is specified.

## Validation

Run:

```sh
./profiles/validate.sh
```

The validator uses only Python's standard library. CI invokes the same script.
The JSON Schema in `target-profile.schema.json` is the interchange contract;
the validator additionally enforces cross-field kernel/OS invariants.

## Safety and interpretation

- `profile-only` means the target is described, not implemented or buildable.
- Filesystem capabilities are explicit booleans; `false` means not asserted
  or not available to the portable BDFS contract, not necessarily impossible.
- `source_revision` and `patchset_digest` are `unresolved` until pinned
  source and patch inputs are actually audited.
- No profile authorizes partition/NVRAM writes, bootloader changes, flashing,
  secure-boot bypass or filesystem migration.
- Linux Kbuild, Btrfs, overlayfs and Linux VFS assumptions must not be reused
  as native FreeBSD or XNU implementations.


The portable capability negotiation contract and metadata-fidelity policy are documented in [`docs/target-capability-api.md`](../docs/target-capability-api.md).
