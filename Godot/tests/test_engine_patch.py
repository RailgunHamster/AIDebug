"""Validate the source integration patch independently of any installer."""

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

GODOT = Path(__file__).resolve().parents[1]
BASE = "cafc17a192acf67903910a68aeb3c076c0907a4b"
TARGETS = (
    "modules/mono/mono_gd/gd_mono.h",
    "modules/mono/mono_gd/gd_mono.cpp",
    "modules/mono/build_scripts/build_assemblies.py",
)


class EnginePatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = os.environ.get("GODOT_AI_DEBUG_SOURCE")
        if not source:
            raise unittest.SkipTest("Set GODOT_AI_DEBUG_SOURCE to a checkout containing the supported base commit")
        cls.originals = {
            relative: subprocess.check_output(["git", "show", BASE + ":" + relative], cwd=source)
            for relative in TARGETS
        }

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="aidebug-engine-patch-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.engine = self.root / "Engine with spaces"
        for relative, content in self.originals.items():
            target = self.engine / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content)
        args = ["git", "apply", "--unidiff-zero", str(GODOT / "patches/godot-4.8-beta2-ai-debug.patch")]
        subprocess.run([*args[:2], "--check", *args[2:]], cwd=self.engine, check=True, capture_output=True)
        subprocess.run(args, cwd=self.engine, check=True, capture_output=True)
        name = "aidebug_build_patch_test"
        spec = importlib.util.spec_from_file_location(name, self.engine / TARGETS[2])
        self.build = importlib.util.module_from_spec(spec)
        sys.modules[name] = self.build
        self.addCleanup(sys.modules.pop, name, None)
        spec.loader.exec_module(self.build)

    def build_module(self, module, environment_path):
        managed = module / "managed/Godot.AIDebug"
        managed.mkdir(parents=True)
        csproj = managed / "Godot.AIDebug.csproj"
        csproj.write_text('<Project Sdk="Microsoft.NET.Sdk" />\n')
        binaries = managed / "bin/Release/net10.0"
        binaries.mkdir(parents=True)
        (binaries / "Godot.AIDebug.dll").write_bytes(b"assembly-output")
        (binaries / "Godot.AIDebug.pdb").write_bytes(b"symbols-output")
        output = self.root / "bin"
        with (
            patch.dict(os.environ, {"GODOT_AI_DEBUG_MODULE_DIR": environment_path}),
            patch.object(self.build, "generate_sdk_package_versions"),
            patch.object(self.build, "build_godot_api", return_value=0),
            patch.object(self.build, "run_msbuild", return_value=0) as msbuild,
        ):
            result = self.build.build_all(
                msbuild_tool=None,
                module_dir=str(self.engine / "modules/mono"),
                output_dir=str(output),
                godot_platform="windows",
                dev_debug=False,
                push_nupkgs_local=None,
                precision="single",
                no_deprecated=False,
                werror=False,
            )
        self.assertEqual(result, 0)
        calls = [call for call in msbuild.call_args_list if call.kwargs["sln"] == str(csproj)]
        self.assertEqual(len(calls), 1)
        self.assertIn("/p:GodotSourceRoot=" + str(self.engine), calls[0].kwargs["msbuild_args"])
        self.assertEqual((output / "GodotSharp/AIDebug/Godot.AIDebug.dll").read_bytes(), b"assembly-output")
        self.assertEqual((output / "GodotSharp/AIDebug/Godot.AIDebug.pdb").read_bytes(), b"symbols-output")

    def test_external_module_builds_from_environment(self):
        module = self.root / "External module with spaces/ai_debug"
        self.build_module(module, str(module))

    def test_builtin_module_builds_without_external_path(self):
        self.build_module(self.engine / "modules/ai_debug", "")


if __name__ == "__main__":
    unittest.main(verbosity=2)
