#!/usr/bin/env python3
"""Merge concatenated GitLab project JSON pages or print a storage report."""
import json
import sys


def merge_pages(raw: str) -> list[dict]:
    decoder = json.JSONDecoder()
    values = []
    offset = 0
    while offset < len(raw):
        while offset < len(raw) and raw[offset].isspace():
            offset += 1
        if offset >= len(raw):
            break
        value, offset = decoder.raw_decode(raw, offset)
        if not isinstance(value, list):
            raise ValueError("GitLab project page must be a JSON array")
        values.extend(value)
    return values


def fmt(value: int) -> str:
    if value >= 1024**3:
        return f"{value / 1024**3:.1f}G"
    if value >= 1024**2:
        return f"{value / 1024**2:.1f}M"
    if value >= 1024:
        return f"{value / 1024:.1f}K"
    return f"{value}B"


def report(projects: list[dict]) -> None:
    print(f"Total projects found: {len(projects)}\n")
    projects.sort(
        key=lambda project: project.get("statistics", {}).get("repository_size", 0),
        reverse=True,
    )
    print(f"{'Project':<70} {'Repo':>10} {'LFS':>10} {'Artifacts':>10} {'Total':>10}")
    print("-" * 115)
    totals = [0, 0, 0, 0]
    for project in projects:
        stats = project.get("statistics", {})
        sizes = [
            stats.get("repository_size", 0),
            stats.get("lfs_objects_size", 0),
            stats.get("job_artifacts_size", 0),
            stats.get("storage_size", 0),
        ]
        if not sizes[3]:
            continue
        name = project.get("path_with_namespace", project.get("name", "?"))
        if len(name) > 68:
            name = name[:65] + "..."
        print(f"{name:<70} {fmt(sizes[0]):>10} {fmt(sizes[1]):>10} {fmt(sizes[2]):>10} {fmt(sizes[3]):>10}")
        totals = [total + size for total, size in zip(totals, sizes)]
    print("-" * 115)
    print(f"{'TOTAL':<70} {fmt(totals[0]):>10} {fmt(totals[1]):>10} {fmt(totals[2]):>10} {fmt(totals[3]):>10}")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "report"
    projects = merge_pages(sys.stdin.read())
    if mode == "merge":
        print(json.dumps(projects))
    elif mode == "report":
        report(projects)
    else:
        raise SystemExit(f"unknown mode: {mode}")
