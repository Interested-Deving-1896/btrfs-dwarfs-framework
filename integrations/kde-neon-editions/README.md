# KDE Neon Editions integration for BDFS

KDE Neon Editions is the **default operating-system target** in the BDFS target registry for both import and export. The registry is configurable: other OS profiles can be enabled alongside KDE Neon and selected explicitly without replacing the default.

- Environment profile: [`environments/kde-neon-editions/profile.json`](../../environments/kde-neon-editions/profile.json)
- Target registry: [`environments/target-registry.json`](../../environments/target-registry.json)
- Shared artifact envelope: [BDFS artifact manifest schema](../../doc/schemas/bdfs-artifact-manifest.schema.json)
- Target resolver: [`environments/resolve_target.py`](../../environments/resolve_target.py)
- Target project: https://github.com/Interested-Deving-1896/kde-neon-editions

## Directional contract

| Direction | Default selection | Current implementation status |
|---|---|---|
| Import into BDFS from a KDE Neon Editions artifact | `kde-neon-editions` | Target resolution is implemented; artifact inspection/materialization still requires a format-specific adapter |
| Export from BDFS to a KDE Neon Editions target | `kde-neon-editions` | Target resolution is implemented; image construction/materialization still requires a format-specific adapter |

The registry resolver deliberately does not pretend to perform conversion. The KDE Neon Editions repository currently contains edition/build definitions, not a universal import/export interchange format. A later adapter must identify and validate each concrete artifact format (for example a root filesystem tree, OCI image, or filesystem image), preserve provenance and filesystem metadata, and report unsupported or lossy fields.

## Multi-OS configuration

Add another OS as a separate environment profile and a target entry in `environments/target-registry.json`. Give it a unique stable `id`, profile path, adapter path, and explicit `selectionOperations`. Keep KDE Neon as the values of `defaultImportTarget` and `defaultExportTarget` unless an intentional policy change is reviewed. A target being registered or enabled does not establish that its artifacts are compatible or that its adapter can materialize them.

Resolve the defaults:

```sh
python3 environments/resolve_target.py --operation import
python3 environments/resolve_target.py --operation export
```

Select another configured OS explicitly:

```sh
python3 environments/resolve_target.py --operation import --target <target-id>
python3 environments/resolve_target.py --operation export --target <target-id>
```

## Safety and compatibility

- Never convert an Ubuntu-based KDE Neon image into an Ageless/Debian identity or vice versa by inference.
- Validate the source artifact, architecture, base release, package manager, init system, kernel/boot assumptions, and manifest before materialization.
- Keep imported hooks inert. Import is not deployment, activation, boot switching, flashing, or host mutation.
- Require an explicit destination and overwrite policy in any future materializer.
- Exclude credentials, private keys, tokens, and machine-specific identity.
- Record checksums, immutable source revisions, licenses, provenance, and conversion loss. The source repository currently tracks `main`; its profile revision is intentionally marked unpinned until a reproducible release/commit is selected.

## Implementation boundary

This integration currently provides the default target contract, profile metadata, and tested target resolution. It does **not** claim a completed KDE Neon image importer/exporter, ISO builder, package migration, or deployment workflow. Those require concrete source/output format contracts and isolated round-trip tests before being advertised as end-to-end.
