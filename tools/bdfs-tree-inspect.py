#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only POSIX root-tree inspector; emits a deterministic JSON manifest."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import platform
import stat
import sys
from collections import Counter
from pathlib import Path
from typing import Any

VERSION = "0.1.0"
FIDELITY_FIELDS = (
    "owner", "mode", "timestamps", "xattrs", "acls",
    "security_labels", "file_capabilities", "sparse_extents",
    "hard_links", "symlinks", "special_objects",
    "case_semantics", "path_encoding",
)


class ScanError(Exception):
    """Raised when a stable, bounded read-only scan cannot be completed."""


def _b64(data: bytes) -> str:
    return base64.b64encode(data).decode("ascii")


def _same_object(before: os.stat_result, after: os.stat_result) -> bool:
    return (
        before.st_dev == after.st_dev
        and before.st_ino == after.st_ino
        and before.st_mode == after.st_mode
        and before.st_size == after.st_size
        and before.st_mtime_ns == after.st_mtime_ns
        and before.st_ctime_ns == after.st_ctime_ns
    )


def _base_metadata(st: os.stat_result) -> dict[str, Any]:
    return {
        "mode": stat.S_IMODE(st.st_mode),
        "uid": st.st_uid,
        "gid": st.st_gid,
        "size_bytes": st.st_size if stat.S_ISREG(st.st_mode) else None,
        "mtime_ns": st.st_mtime_ns,
        "ctime_ns": st.st_ctime_ns,
        "nlink": st.st_nlink,
    }


def _xattrs(fd: int) -> tuple[dict[str, str], str]:
    attrs: dict[str, str] = {}
    try:
        names = sorted(os.listxattr(fd))
        for name in names:
            value = os.getxattr(fd, name)
            attrs[name] = _b64(value)
        return attrs, "preserved"
    except (AttributeError, OSError):
        return attrs, "unknown"


def _sparse_status(fd: int, size: int) -> str:
    if size == 0:
        return "preserved"
    seek_data = getattr(os, "SEEK_DATA", None)
    seek_hole = getattr(os, "SEEK_HOLE", None)
    if seek_data is None or seek_hole is None:
        return "unknown"
    try:
        pos = 0
        while pos < size:
            try:
                data = os.lseek(fd, pos, seek_data)
            except OSError as exc:
                # ENXIO means no more data; the remaining range is a hole.
                if exc.errno == getattr(os, "ENXIO", 6):
                    break
                raise
            hole = os.lseek(fd, data, seek_hole)
            if hole <= data:
                return "unknown"
            pos = hole
        return "preserved"
    except OSError:
        return "unknown"


def _fidelity(xattr_status: str, kind: str, sparse_status: str) -> dict[str, str]:
    result = {field: "unknown" for field in FIDELITY_FIELDS}
    result.update({
        "owner": "preserved",
        "mode": "preserved",
        "timestamps": "preserved",
        "xattrs": xattr_status,
        "acls": xattr_status,
        "security_labels": xattr_status,
        "file_capabilities": xattr_status,
        "sparse_extents": sparse_status,
        "path_encoding": "preserved",
    })
    if kind in ("symlink", "directory", "file", "fifo", "socket",
                "character_device", "block_device"):
        result["symlinks"] = "preserved" if kind == "symlink" else "preserved"
        result["special_objects"] = "preserved"
    if kind == "file":
        result["hard_links"] = "preserved"
    return result


def _open_checked(parent_fd: int, name: str, flags: int,
                  before: os.stat_result) -> int:
    flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(name, flags, dir_fd=parent_fd)
    after = os.fstat(fd)
    if not _same_object(before, after):
        os.close(fd)
        raise ScanError(f"source entry changed during scan: {name!r}")
    return fd


def inspect_tree(source: str) -> dict[str, Any]:
    source_path = os.path.abspath(source)
    root_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    root_flags |= getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        root_fd = os.open(source_path, root_flags)
    except OSError as exc:
        raise ScanError(f"cannot open source root without following symlinks: {exc}") from exc

    entries: list[dict[str, Any]] = []
    hardlinks: dict[tuple[int, int], str] = {}
    root_stat = os.fstat(root_fd)
    root_xattrs, root_xattr_status = _xattrs(root_fd)
    root_fidelity = _fidelity(root_xattr_status, "directory", "unknown")
    entries.append({
        "path": ".",
        "path_b64": "",
        "type": "directory",
        "metadata": _base_metadata(root_stat),
        "xattrs_b64": root_xattrs,
        "fidelity": root_fidelity,
    })

    def walk(dir_fd: int, parent_parts: tuple[bytes, ...]) -> None:
        try:
            names = sorted(os.listdir(dir_fd), key=os.fsencode)
        except OSError as exc:
            raise ScanError(f"cannot enumerate directory {parent_parts!r}: {exc}") from exc

        for name in names:
            name_bytes = os.fsencode(name)
            parts = parent_parts + (name_bytes,)
            rel_bytes = b"/".join(parts)
            rel_path = os.fsdecode(rel_bytes)
            try:
                st = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
            except OSError as exc:
                raise ScanError(f"cannot stat {rel_path!r}: {exc}") from exc

            if st.st_dev != root_stat.st_dev:
                entries.append({
                    "path": rel_path,
                    "path_b64": _b64(rel_bytes),
                    "type": "mount_boundary",
                    "metadata": _base_metadata(st),
                    "fidelity": {field: "unknown" for field in FIDELITY_FIELDS},
                    "note": "different device from source root; not traversed",
                })
                continue

            kind: str
            if stat.S_ISDIR(st.st_mode):
                kind = "directory"
                child_fd = _open_checked(
                    dir_fd, name, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0), st
                )
                try:
                    attrs, attr_status = _xattrs(child_fd)
                    entries.append({
                        "path": rel_path,
                        "path_b64": _b64(rel_bytes),
                        "type": kind,
                        "metadata": _base_metadata(st),
                        "xattrs_b64": attrs,
                        "fidelity": _fidelity(attr_status, kind, "unknown"),
                    })
                    walk(child_fd, parts)
                finally:
                    os.close(child_fd)
                continue

            if stat.S_ISREG(st.st_mode):
                kind = "file"
                fd = _open_checked(dir_fd, name, os.O_RDONLY, st)
                try:
                    digest = hashlib.sha256()
                    while True:
                        block = os.read(fd, 1024 * 1024)
                        if not block:
                            break
                        digest.update(block)
                    after = os.fstat(fd)
                    if not _same_object(st, after):
                        raise ScanError(f"file changed while hashing: {rel_path!r}")
                    attrs, attr_status = _xattrs(fd)
                    sparse_status = _sparse_status(fd, st.st_size)
                finally:
                    os.close(fd)

                entry: dict[str, Any] = {
                    "path": rel_path,
                    "path_b64": _b64(rel_bytes),
                    "type": kind,
                    "metadata": _base_metadata(st),
                    "sha256": digest.hexdigest(),
                    "xattrs_b64": attrs,
                    "fidelity": _fidelity(attr_status, kind, sparse_status),
                }
                if st.st_nlink > 1:
                    inode_key = (st.st_dev, st.st_ino)
                    first = hardlinks.setdefault(inode_key, _b64(rel_bytes))
                    entry["hardlink_group"] = first
                    entry["fidelity"]["hard_links"] = "preserved"
                entries.append(entry)
                continue

            if stat.S_ISLNK(st.st_mode):
                kind = "symlink"
                try:
                    target = os.readlink(name, dir_fd=dir_fd)
                    after = os.stat(name, dir_fd=dir_fd, follow_symlinks=False)
                except OSError as exc:
                    raise ScanError(f"cannot read symlink {rel_path!r}: {exc}") from exc
                if not _same_object(st, after):
                    raise ScanError(f"symlink changed during scan: {rel_path!r}")
                target_bytes = os.fsencode(target)
                entries.append({
                    "path": rel_path,
                    "path_b64": _b64(rel_bytes),
                    "type": kind,
                    "target_b64": _b64(target_bytes),
                    "metadata": _base_metadata(st),
                    "fidelity": _fidelity("unknown", kind, "unknown"),
                })
                continue

            if stat.S_ISFIFO(st.st_mode):
                kind = "fifo"
            elif stat.S_ISSOCK(st.st_mode):
                kind = "socket"
            elif stat.S_ISCHR(st.st_mode):
                kind = "character_device"
            elif stat.S_ISBLK(st.st_mode):
                kind = "block_device"
            else:
                kind = "unknown"

            entries.append({
                "path": rel_path,
                "path_b64": _b64(rel_bytes),
                "type": kind,
                "metadata": _base_metadata(st),
                "fidelity": _fidelity("unknown", kind, "unknown"),
                "note": "recorded only; never recreated by this inspector",
            })

    try:
        walk(root_fd, ())
    finally:
        os.close(root_fd)

    entries.sort(key=lambda item: base64.b64decode(item["path_b64"]))
    fidelity_counts = {
        field: dict(sorted(Counter(e["fidelity"][field] for e in entries).items()))
        for field in FIDELITY_FIELDS
    }
    counts = dict(sorted(Counter(e["type"] for e in entries).items()))
    return {
        "manifest_version": 1,
        "tool": {"name": "bdfs-tree-inspect", "version": VERSION},
        "source": {
            "os_family": platform.system().lower(),
            "architecture": platform.machine(),
            "root_device_id": root_stat.st_dev,
        },
        "entries": entries,
        "summary": {
            "entry_count": len(entries),
            "type_counts": counts,
            "fidelity_counts": fidelity_counts,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect a POSIX root tree without following symlinks or modifying it."
    )
    parser.add_argument("--source", required=True, help="directory tree to inspect")
    parser.add_argument("--output", help="write JSON manifest to this file (default: stdout)")
    parser.add_argument(
        "--strict", action="store_true",
        help="fail if any captured metadata fidelity field is unknown or unsupported",
    )
    args = parser.parse_args()

    try:
        manifest = inspect_tree(args.source)
        if args.strict:
            lossy = [
                (entry["path"], field, state)
                for entry in manifest["entries"]
                for field, state in entry["fidelity"].items()
                if state in ("unknown", "unsupported", "omitted_by_policy")
            ]
            if lossy:
                raise ScanError(
                    f"strict fidelity check found {len(lossy)} unknown/unsupported "
                    f"metadata disposition(s); first: {lossy[0]!r}"
                )
        rendered = json.dumps(manifest, sort_keys=True, indent=2, ensure_ascii=True) + "\n"
        if args.output:
            output_path = Path(args.output).resolve()
            source_path = Path(args.source).resolve()
            if output_path == source_path or source_path in output_path.parents:
                raise ScanError("manifest output must be outside the inspected source tree")
            output_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = output_path.with_name(output_path.name + ".tmp")
            temporary.write_text(rendered, encoding="utf-8")
            temporary.replace(output_path)
        else:
            sys.stdout.write(rendered)
    except (OSError, ScanError) as exc:
        print(f"bdfs-tree-inspect: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
