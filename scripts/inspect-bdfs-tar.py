#!/usr/bin/env python3
"""Inspect a tar archive for unsafe members without extracting any content."""
import argparse
import posixpath
import sys
import tarfile
from pathlib import Path


def inspect(path: Path) -> list[str]:
    if path.is_symlink() or not path.is_file():
        return ["archive: input must be a regular, non-symlink file"]
    try:
        with tarfile.open(path, mode="r:*") as archive:
            members = archive.getmembers()
    except (OSError, tarfile.TarError) as exc:
        return [f"archive: malformed or unreadable tar: {exc}"]

    errors: list[str] = []
    seen: set[str] = set()
    for member in members:
        name = member.name
        normalized = posixpath.normpath(name)
        if (
            not name
            or name.startswith("/")
            or "\\" in name
            or normalized == ".."
            or normalized.startswith("../")
            or normalized.startswith("/")
        ):
            errors.append(f"member {name!r}: unsafe path")
            continue
        if normalized in seen:
            errors.append(f"member {name!r}: duplicate path")
            continue
        seen.add(normalized)

        if member.issym() or member.islnk():
            target = member.linkname
            if not target or target.startswith("/") or "\\" in target:
                errors.append(f"link {name!r}: absolute or ambiguous target")
                continue
            resolved = (
                posixpath.normpath(posixpath.join(posixpath.dirname(normalized), target))
                if member.issym()
                else posixpath.normpath(target)
            )
            if resolved == ".." or resolved.startswith("../") or resolved.startswith("/"):
                errors.append(f"link {name!r}: target escapes archive root")
        elif not (member.isfile() or member.isdir()):
            errors.append(f"member {name!r}: unsupported special file type")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    args = parser.parse_args()
    errors = inspect(args.archive)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Tar archive passed safety inspection: {args.archive}")
    print("No archive members were extracted or executed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
