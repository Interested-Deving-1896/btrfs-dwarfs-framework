# Read-only POSIX root-tree inspection

`tools/bdfs-tree-inspect.py` is the first host-side source adapter for the root-tree ingestion work in issue #75. It emits a deterministic JSON manifest from a POSIX directory tree; it does not create a DwarFS image or claim that the captured tree is bootable on another OS.

## Usage

```sh
python3 tools/bdfs-tree-inspect.py --source /mnt/rootfs > root-tree-manifest.json
python3 tools/bdfs-tree-inspect.py --source /mnt/rootfs --output /tmp/root-tree-manifest.json
```

The source is opened as a directory without following a symlink. Traversal uses directory file descriptors, `O_NOFOLLOW`, and no-follow metadata queries. Regular files are hashed through opened file descriptors and checked for identity/metadata changes during the read. Symlink targets are recorded as bytes and are never traversed. Entries on a different device from the selected root are recorded as mount boundaries and are not traversed.

## Manifest properties

- Every included path has a display path and a base64-encoded raw path, preserving undecodable path bytes.
- Regular files carry SHA-256 hashes; hard-linked files share a stable group reference.
- Symlink targets are recorded as base64 bytes.
- File types, mode, ownership IDs, size and nanosecond timestamps are recorded.
- Readable extended attributes are stored as base64 values.
- Special objects are represented by type only; the inspector never recreates device nodes, sockets or FIFOs.
- Per-field fidelity is explicit: preserved, translated, unsupported, omitted-by-policy or unknown. This first inspector reports unknown where the host API cannot establish fidelity.

## Strict mode and limitations

`--strict` fails if any entry has unknown/unsupported/omitted metadata fidelity. It is intentionally conservative: case-sensitivity semantics and some sparse extent details may remain unknown, so strict mode may reject otherwise readable trees.

This first adapter is read-only and POSIX-oriented. It does not yet write content-addressed chunks, package DwarFS, snapshot a changing source filesystem, capture every platform-specific ACL/security model, cross mount boundaries, or restore a tree. Those are later phases of #75. A successful manifest is an inspection result, not a verified BDFS artifact.
