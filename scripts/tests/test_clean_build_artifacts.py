#!/usr/bin/env python3
# Copyright (c) 2026 The HPM Zephyr Module Contributors
# SPDX-License-Identifier: Apache-2.0

"""Tests for scripts/clean_build_artifacts.py."""

from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import clean_build_artifacts  # noqa: E402


class CleanBuildArtifactsTest(unittest.TestCase):
    """Exercise cleanup selection and deletion behavior."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.build_root = Path(self.tmp.name) / "build"
        self.build_root.mkdir()

    def write(self, relative: str, content: str = "data") -> Path:
        path = self.build_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def run_cleaner(self, *arguments: str) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            result = clean_build_artifacts.main(
                ["--build-root", str(self.build_root), *arguments]
            )
        return result, stdout.getvalue(), stderr.getvalue()

    def test_dry_run_does_not_modify_tree(self) -> None:
        artifact = self.write("with-log/CMakeCache.txt")
        log = self.write("with-log/test.uart.log")

        result, output, _ = self.run_cleaner()

        self.assertEqual(result, 0)
        self.assertTrue(artifact.exists())
        self.assertTrue(log.exists())
        self.assertIn("dry run", output)

    def test_prune_keeps_logs_and_removes_build_files(self) -> None:
        artifact = self.write("with-log/zephyr/app.obj")
        log = self.write("with-log/test.uart.log")
        report = self.write("with-log/twister.json")
        self.write("with-log/CMakeCache.txt")
        self.write("with-log/build.ninja")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertFalse(artifact.exists())
        self.assertTrue(log.exists())
        self.assertTrue(report.exists())
        self.assertFalse((self.build_root / "with-log/CMakeCache.txt").exists())

    def test_build_tree_without_evidence_is_deleted(self) -> None:
        self.write("no-evidence/build.ninja")
        self.write("no-evidence/zephyr/zephyr.elf")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertFalse((self.build_root / "no-evidence").exists())

    def test_directory_without_build_marker_or_twister_report_is_untouched(
        self,
    ) -> None:
        evidence = self.write("reports/test.log")
        other = self.write("reports/notes.md")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertTrue(evidence.exists())
        self.assertTrue(other.exists())

    def test_evidence_only_twister_directory_is_pruned(self) -> None:
        self.write("evidence-only/twister.json")
        stale = self.write("evidence-only/stale.elf")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertFalse(stale.exists())
        self.assertTrue((self.build_root / "evidence-only/twister.json").exists())

    def test_protected_directory_is_untouched(self) -> None:
        marker = self.write("hpm-openocd-custom/CMakeCache.txt")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertTrue(marker.exists())

    def test_source_checkout_is_untouched_by_default(self) -> None:
        marker = self.write("checkout/build.ninja")
        self.write("checkout/.git/HEAD")
        self.write("checkout/.git/objects/config")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertTrue(marker.exists())
        self.assertTrue((self.build_root / "checkout/.git/objects/config").exists())

    def test_top_level_assets_are_untouched(self) -> None:
        top_level = self.write("manual.uart.log")
        patch = self.write("fix.patch")

        result, _, stderr = self.run_cleaner("--apply")

        self.assertEqual(result, 0, stderr)
        self.assertTrue(top_level.exists())
        self.assertTrue(patch.exists())

    def test_json_output_contains_summary(self) -> None:
        self.write("tree/CMakeCache.txt")

        stdout = io.StringIO()
        with redirect_stdout(stdout):
            result = clean_build_artifacts.main(
                ["--build-root", str(self.build_root), "--json"]
            )

        self.assertEqual(result, 0)
        output = json.loads(stdout.getvalue())
        self.assertTrue(output["summary"]["dry_run"])
        self.assertEqual(
            output["summary"]["build_root"], str(self.build_root.resolve())
        )


if __name__ == "__main__":
    unittest.main()
