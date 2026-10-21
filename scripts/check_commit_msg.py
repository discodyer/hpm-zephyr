#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Check Conventional Commits and DCO sign-off in a commit message."""

from pathlib import Path
import re
import sys

TYPES = "build|chore|ci|docs|feat|fix|perf|refactor|revert|style|test"
HEADER = re.compile(rf"^(?:{TYPES})(?:\([^)]+\))?!?: .+")
SIGNOFF = re.compile(r"^Signed-off-by: .+ <[^<>]+>$", re.MULTILINE)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_commit_msg.py <commit-msg-file>", file=sys.stderr)
        return 2

    text = Path(sys.argv[1]).read_text(encoding="utf-8")
    subject = text.splitlines()[0] if text else ""
    errors = []

    if not HEADER.match(subject):
        errors.append("subject must use Conventional Commits, e.g. feat(soc): ...")
    if not SIGNOFF.search(text):
        errors.append("missing DCO sign-off; commit with `git commit -s`")

    if errors:
        for error in errors:
            print(f"commit-msg: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
