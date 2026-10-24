#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Negative tests for scripts/hpm_sdk_baseline.py."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/hpm_sdk_baseline.py"


class BaselineTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.baseline = Path(self.tmp.name) / "baseline.env"
        self.baseline.write_text(
            "HPM_SDK_REVISION=0" * 40 + "\n",
            encoding="utf-8",
        )
        self.env = os.environ.copy()
        self.env["HPM_SDK_DIR"] = str(ROOT / ".." / "hpm_sdk")
        self.env["ZEPHYR_BASE"] = str(ROOT / ".." / "zephyr")

    def run_check(self, baseline: Path) -> subprocess.CompletedProcess[str]:
        source = SCRIPT.read_text(encoding="utf-8")
        source = source.replace(
            'BASELINE = ROOT / "modules/hpm_sdk/baseline.env"',
            f"BASELINE = Path({str(baseline)!r})",
        )
        test_script = Path(self.tmp.name) / "baseline_check.py"
        test_script.write_text(source, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(test_script)],
            env=self.env,
            cwd=ROOT,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_empty_baseline_fails(self) -> None:
        self.baseline.write_text("", encoding="utf-8")
        result = self.run_check(self.baseline)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("baseline is missing", result.stderr)

    def test_changed_revision_fails(self) -> None:
        result = self.run_check(self.baseline)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("HPM_SDK_REVISION", result.stderr)


if __name__ == "__main__":
    unittest.main()
