#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# Copyright (c) 2026 Cody Gu <gujiaqi@iscas.ac.cn>
# SPDX-License-Identifier: Apache-2.0

"""Remove rebuildable Zephyr build trees while preserving test evidence."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import fnmatch
import json
import os
from pathlib import Path
import shutil
import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_ROOT = REPO_ROOT.parent / "build"

DEFAULT_EVIDENCE_PATTERNS = (
    "*.log",
    "twister.json",
    "testplan.json",
    "twister_report.xml",
    "twister_suite_report.xml",
    "twister.xml",
)
DEFAULT_REPORT_PATTERNS = (
    "twister.json",
    "testplan.json",
    "twister_report.xml",
    "twister_suite_report.xml",
    "twister.xml",
)
DEFAULT_PROTECTED_PATTERNS = ("hpm-openocd-*",)


@dataclass
class TreeStats:
    """Summary of a top-level directory below the build root."""

    files: int = 0
    size: int = 0
    evidence_files: int = 0
    evidence_size: int = 0
    report_files: int = 0
    removable_files: int = 0
    removable_size: int = 0
    has_build_marker: bool = False
    has_source_checkout: bool = False


@dataclass
class Action:
    """An action selected for one top-level directory."""

    path: Path
    operation: str
    stats: TreeStats
    reason: str = ""


def matches_patterns(
    path: Path, relative_path: Path, patterns: tuple[str, ...]
) -> bool:
    """Return whether a path matches a file name or relative-path pattern."""

    relative = relative_path.as_posix()
    return any(
        fnmatch.fnmatch(path.name, pattern) or fnmatch.fnmatch(relative, pattern)
        for pattern in patterns
    )


def is_build_marker(path: Path) -> bool:
    """Return whether a file marks a generated build tree."""

    if path.name in {"CMakeCache.txt", "build.ninja"}:
        return True
    return path.name == ".config" and path.parent.name == "zephyr"


def scan_tree(
    root: Path,
    evidence_patterns: tuple[str, ...],
    report_patterns: tuple[str, ...],
) -> TreeStats:
    """Scan a tree without following symbolic links."""

    stats = TreeStats()
    for dirpath, dirnames, filenames in os.walk(root, topdown=True, followlinks=False):
        directory = Path(dirpath)
        if ".git" in dirnames:
            stats.has_source_checkout = True
            dirnames.remove(".git")

        for filename in filenames:
            path = directory / filename
            relative = path.relative_to(root)
            try:
                size = path.lstat().st_size
            except FileNotFoundError:
                continue

            stats.files += 1
            stats.size += size
            if matches_patterns(path, relative, evidence_patterns):
                stats.evidence_files += 1
                stats.evidence_size += size
                if matches_patterns(path, relative, report_patterns):
                    stats.report_files += 1
            else:
                stats.removable_files += 1
                stats.removable_size += size
            if is_build_marker(path):
                stats.has_build_marker = True

        for dirname in dirnames:
            path = directory / dirname
            if path.is_symlink():
                stats.files += 1
                stats.removable_files += 1

    return stats


def classify_tree(
    path: Path,
    stats: TreeStats,
    protected_patterns: tuple[str, ...],
    prune_source_checkouts: bool,
) -> Action:
    """Classify a top-level directory as delete, prune, or skip."""

    if matches_patterns(path, Path(path.name), protected_patterns):
        return Action(path, "skip", stats, "matches protected pattern")
    if not stats.has_build_marker and stats.report_files == 0:
        return Action(path, "skip", stats, "no build-system marker")
    if stats.has_source_checkout and not prune_source_checkouts:
        return Action(path, "skip", stats, "contains a source checkout")
    if stats.evidence_files > 0:
        return Action(path, "prune", stats, "preserve test evidence")
    return Action(path, "delete", stats, "no test evidence")


def prune_tree(
    root: Path,
    evidence_patterns: tuple[str, ...],
) -> tuple[int, int, int, list[str]]:
    """Remove non-evidence files and empty directories from a tree."""

    removed_files = 0
    removed_bytes = 0
    removed_directories = 0
    errors: list[str] = []

    for dirpath, dirnames, filenames in os.walk(root, topdown=False, followlinks=False):
        directory = Path(dirpath)
        for filename in filenames:
            path = directory / filename
            relative = path.relative_to(root)
            if matches_patterns(path, relative, evidence_patterns):
                continue
            try:
                size = path.lstat().st_size
                path.unlink()
                removed_files += 1
                removed_bytes += size
            except FileNotFoundError:
                continue
            except OSError as error:
                errors.append(f"failed to remove {path}: {error}")

        for dirname in dirnames:
            path = directory / dirname
            try:
                if path.is_symlink():
                    path.unlink()
                    removed_files += 1
                else:
                    path.rmdir()
                    removed_directories += 1
            except FileNotFoundError:
                continue
            except OSError:
                # The directory is not empty, usually because evidence was kept.
                continue

    return removed_files, removed_bytes, removed_directories, errors


def apply_action(
    action: Action,
    evidence_patterns: tuple[str, ...],
) -> tuple[int, int, int, list[str]]:
    """Apply a selected action and return removal statistics."""

    if action.operation == "delete":
        try:
            shutil.rmtree(action.path)
        except OSError as error:
            return 0, 0, 0, [f"failed to remove {action.path}: {error}"]
        return action.stats.files, action.stats.size, 1, []

    if action.operation == "prune":
        return prune_tree(action.path, evidence_patterns)

    return 0, 0, 0, []


def build_parser() -> argparse.ArgumentParser:
    """Create the command-line parser."""

    parser = argparse.ArgumentParser(
        description=(
            "Remove generated Zephyr/Twister build trees while preserving logs and "
            "Twister reports. The default is a dry run."
        )
    )
    parser.add_argument(
        "--build-root",
        type=Path,
        default=DEFAULT_BUILD_ROOT,
        help="directory containing generated build trees (default: %(default)s)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="delete selected artifacts; without this option only print the plan",
    )
    parser.add_argument(
        "--protect",
        action="append",
        default=[],
        metavar="GLOB",
        help="additional top-level directory pattern to keep in full",
    )
    parser.add_argument(
        "--preserve",
        action="append",
        default=[],
        metavar="GLOB",
        help="additional file-name or relative-path pattern to preserve",
    )
    parser.add_argument(
        "--prune-source-checkouts",
        action="store_true",
        help="allow cleaning build markers found inside source checkouts",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="print skipped directories as well as cleanup actions",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="write the cleanup plan and summary as JSON",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Run the cleanup command."""

    args = build_parser().parse_args(argv)
    build_root = args.build_root.resolve()
    if not build_root.is_dir():
        print(f"error: build root is not a directory: {build_root}", file=sys.stderr)
        return 2
    if build_root.parent == build_root:
        print("error: refusing to clean a filesystem root", file=sys.stderr)
        return 2

    evidence_patterns = DEFAULT_EVIDENCE_PATTERNS + tuple(args.preserve)
    protected_patterns = DEFAULT_PROTECTED_PATTERNS + tuple(args.protect)

    actions: list[Action] = []
    for entry in sorted(build_root.iterdir(), key=lambda path: path.name):
        if not entry.is_dir() or entry.is_symlink():
            continue
        stats = scan_tree(entry, evidence_patterns, DEFAULT_REPORT_PATTERNS)
        actions.append(
            classify_tree(entry, stats, protected_patterns, args.prune_source_checkouts)
        )

    totals = {
        "deleted_files": 0,
        "deleted_bytes": 0,
        "deleted_directories": 0,
        "preserved_evidence_files": 0,
    }
    errors: list[str] = []
    action_records = []

    for action in actions:
        if action.operation == "skip" and not args.verbose:
            continue
        if action.operation == "prune":
            totals["preserved_evidence_files"] += action.stats.evidence_files

        record = {
            "path": str(action.path),
            "operation": action.operation,
            "reason": action.reason,
            "files": action.stats.files,
            "bytes": action.stats.size,
        }
        action_records.append(record)

        if not args.json:
            print(
                f"{action.operation.upper():6} {action.path} "
                f"({action.stats.files} files, {action.stats.size} bytes): {action.reason}"
            )

        if not args.apply or action.operation == "skip":
            continue

        removed_files, removed_bytes, removed_directories, action_errors = apply_action(
            action, evidence_patterns
        )
        totals["deleted_files"] += removed_files
        totals["deleted_bytes"] += removed_bytes
        totals["deleted_directories"] += removed_directories
        errors.extend(action_errors)

    summary = {
        "build_root": str(build_root),
        "dry_run": not args.apply,
        **totals,
        "errors": errors,
    }
    if args.json:
        print(json.dumps({"summary": summary, "actions": action_records}, indent=2))
    else:
        mode = "dry run" if not args.apply else "applied"
        print(
            f"{mode}: {totals['deleted_files']} files, "
            f"{totals['deleted_bytes']} bytes, {totals['deleted_directories']} directories"
        )
        print(f"preserved evidence files: {totals['preserved_evidence_files']}")
        for error in errors:
            print(f"error: {error}", file=sys.stderr)

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
