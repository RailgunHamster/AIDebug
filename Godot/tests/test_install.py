"""Installer regressions. Set GODOT_AI_DEBUG_TEST_SOURCE to a Godot Git checkout."""

import hashlib
import importlib.util
import json
import os
import runpy
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

GODOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("ai_debug_install", GODOT / "install.py")
INSTALL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALL)


class InstallerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("GODOT_AI_DEBUG_TEST_SOURCE")
        if not source:
            raise unittest.SkipTest("Set GODOT_AI_DEBUG_TEST_SOURCE to a checkout containing the supported base commit")
        profile = INSTALL.compatibility()
        cls.originals = {}
        for relative in ["version.py", "SConstruct", *profile["patches"][0]["targets"]]:
            cls.originals[relative] = subprocess.check_output(
                ["git", "show", profile["base_commit"] + ":" + relative], cwd=source
            )

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ai-debug-install-test-")
        self.addCleanup(self.temp.cleanup)
        self.engine = Path(self.temp.name) / "Godot source with spaces"
        self.engine.mkdir()
        for relative, content in self.originals.items():
            path = self.engine / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)

    def snapshot(self):
        return {
            str(p.relative_to(self.engine)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in self.engine.rglob("*")
            if p.is_file()
        }

    def test_check_and_dry_run_do_not_write(self):
        before = self.snapshot()
        self.assertEqual(INSTALL.inspect(self.engine)["patches"][0]["state"], "pending")
        INSTALL.install(self.engine, dry_run=True)
        self.assertEqual(self.snapshot(), before)

    def test_clean_install_is_recognized(self):
        report = INSTALL.install(self.engine)
        self.assertEqual(report["status"], "installed")
        self.assertEqual(report["patches"][0]["state"], "applied")
        record = json.loads((self.engine / INSTALL.MANIFEST).read_text())
        self.assertEqual(Path(record["module_dir"]), INSTALL.MODULE)
        code = (self.engine / "modules/mono/mono_gd/gd_mono.cpp").read_text()
        self.assertIn("String GDMono::ai_debug_invoke", code)
        self.assertNotIn("GDMono::ai_guide_init_editor_plugin", code)

    def test_reinstall_is_byte_identical(self):
        INSTALL.install(self.engine)
        before = self.snapshot()
        INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_windows_line_endings(self):
        for relative in INSTALL.compatibility()["patches"][0]["targets"]:
            path = self.engine / relative
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(INSTALL.install(self.engine)["status"], "installed")

    def test_preserve_other_custom_modules(self):
        (self.engine / "custom.py").write_text('custom_modules = "other/modules,another/modules"\nmy_setting = 42\n')
        INSTALL.install(self.engine)
        installed = runpy.run_path(str(self.engine / "custom.py"))
        self.assertEqual(installed["my_setting"], 42)
        self.assertEqual(
            installed["custom_modules"].split(",")[:2],
            ["other/modules", "another/modules"],
        )
        self.assertEqual(Path(installed["custom_modules"].split(",")[-1]), INSTALL.MODULE.parent)
        INSTALL.install(self.engine)
        self.assertEqual(
            runpy.run_path(str(self.engine / "custom.py"))["custom_modules"],
            installed["custom_modules"],
        )

    def test_conflicting_engine_edits_are_preserved(self):
        target = self.engine / "modules/mono/mono_gd/gd_mono.h"
        target.write_bytes(target.read_bytes() + b"\n// Local engine customization\n")
        before = self.snapshot()
        with self.assertRaises(INSTALL.InstallError):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_partial_patch_is_rejected(self):
        INSTALL.install(self.engine)
        relative = "modules/mono/mono_gd/gd_mono.h"
        (self.engine / relative).write_bytes(self.originals[relative])
        before = self.snapshot()
        with self.assertRaises(INSTALL.InstallError):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_wrong_engine_version_is_rejected(self):
        version = self.engine / "version.py"
        version.write_text(version.read_text().replace("minor = 8", "minor = 9"))
        with self.assertRaises(INSTALL.InstallError):
            INSTALL.install(self.engine)
        self.assertFalse((self.engine / INSTALL.MANIFEST).exists())

    def test_existing_module_is_not_overwritten(self):
        legacy = self.engine / "modules/ai_debug"
        legacy.mkdir()
        (legacy / "local.cpp").write_text("// Owned by a different checkout\n")
        before = self.snapshot()
        with self.assertRaises(INSTALL.InstallError):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_bad_configuration_is_rejected_before_patching(self):
        (self.engine / "custom.py").write_text(INSTALL.BEGIN + "\n")
        before = self.snapshot()
        with self.assertRaises(INSTALL.InstallError):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_write_failure_rolls_back_engine_and_configuration(self):
        before = self.snapshot()
        original = INSTALL.write_if_changed

        def fail_manifest(path, content):
            if path.name == INSTALL.MANIFEST:
                raise OSError("simulated disk write failure")
            return original(path, content)

        with (
            patch.object(INSTALL, "write_if_changed", fail_manifest),
            self.assertRaises(OSError),
        ):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)

    def test_git_ignores_local_installation_paths(self):
        subprocess.run(["git", "init", "-q", str(self.engine)], check=True)
        INSTALL.install(self.engine)
        ignored = subprocess.check_output(["git", "check-ignore", INSTALL.MANIFEST], cwd=self.engine, text=True)
        self.assertEqual(ignored.strip(), INSTALL.MANIFEST)

    def test_git_exclude_write_failure_rolls_back_installation(self):
        subprocess.run(["git", "init", "-q", str(self.engine)], check=True)
        before = self.snapshot()
        original = INSTALL.write_if_changed

        def fail_exclude(path, content):
            if path.name == "exclude":
                raise OSError("simulated Git exclude write failure")
            return original(path, content)

        with patch.object(INSTALL, "write_if_changed", fail_exclude), self.assertRaises(OSError):
            INSTALL.install(self.engine)
        self.assertEqual(self.snapshot(), before)


if __name__ == "__main__":
    unittest.main(verbosity=2)
