# BDFS target capability API

The portable target API is a userspace contract. It deliberately includes no Linux kernel headers and does not detect or mutate the host system.

## Negotiation

Callers provide a versioned `bdfs_target_context`, an adapter-declared `bdfs_capability_set`, and a versioned `bdfs_capability_request`. Each capability is explicitly `BDFS_CAP_SUPPORTED`, `BDFS_CAP_UNSUPPORTED`, or `BDFS_CAP_UNKNOWN`. Zero-initialized/omitted declarations are unknown, never implicitly supported.

`bdfs_target_negotiate()` is deterministic and side-effect-free. Required capabilities fail closed when unknown or unsupported. Callers may set `required_kernel_abi` to reject a target whose declared ABI does not exactly match the requested ABI. The target's OS family and kernel family are validated as a pair:

- Linux OS → Linux kernel
- FreeBSD OS → FreeBSD kernel
- Darwin OS → XNU kernel

Android/GKI and ChromiumOS are represented as Linux-kernel downstream targets. They are not separate kernel families.

## Metadata fidelity

A `bdfs_metadata_report` accounts for each required metadata field as exactly one of preserved, translated, unsupported, omitted-by-policy, or unknown. Overlapping dispositions and out-of-range bits are invalid.

Permissive validation accepts explicit loss dispositions so a caller can present the report to an operator. Strict validation rejects required metadata that is unsupported, omitted, or unknown. Explicit translation is accepted; adapters must describe the translation in their higher-level report before the operation is authorized.

## Safety boundary

This API only negotiates declared capabilities and validates reports. It does not mount filesystems, load kernel modules, write partitions, change boot state, flash devices, or infer capabilities from an OS name. A profile existing on disk does not prove that a native adapter exists.
