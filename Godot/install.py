"""Install this checkout's Godot module and its versioned engine integration."""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
MODULE = ROOT / "modules" / "ai_debug"
MANIFEST = ".ai_debug-install.json"
BEGIN = "# BEGIN AIDebug installation (managed by Godot/install.py)"
END = "# END AIDebug installation"


class InstallError(RuntimeError):
    pass


def run(args, *, cwd, capture=False, env=None):
    result = subprocess.run(
        [str(arg) for arg in args],
        cwd=cwd,
        env=env,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=capture,
        check=False,
    )
    if result.returncode:
        detail = result.stderr.strip() if capture else "see command output"
        raise InstallError(f"Command failed ({result.returncode}): {args[0]}: {detail}")
    return result.stdout if capture else ""


def compatibility():
    return json.loads((ROOT / "compatibility.json").read_text(encoding="utf-8"))


def validate_engine(engine):
    profile = compatibility()
    for relative in ("SConstruct", "version.py", "modules/mono/mono_gd/gd_mono.cpp"):
        if not (engine / relative).is_file():
            raise InstallError(f"Not a Godot source tree: missing {relative}")
    values = {}
    for node in ast.parse((engine / "version.py").read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            try:
                values[node.targets[0].id] = ast.literal_eval(node.value)
            except (ValueError, TypeError):
                pass
    if any(values.get(key) != profile[key] for key in ("major", "minor", "status")):
        raise InstallError(f"This patch profile supports Godot {profile['version']} ({profile['base_commit']}).")
    built_in = engine / "modules" / "ai_debug"
    if built_in.exists() and built_in.resolve() != MODULE.resolve():
        raise InstallError(f"Another ai_debug module exists at {built_in}. Move it aside before installing.")
    return profile


def patch_status(engine, spec):
    patch = ROOT / spec["file"]
    if hashlib.sha256(patch.read_bytes()).hexdigest() != spec["sha256"]:
        raise InstallError(f"Patch checksum mismatch: {patch.name}")
    for relative in spec["targets"]:
        if not (engine / relative).resolve().is_relative_to(engine.resolve()):
            raise InstallError("Patch target escapes the engine source tree")
    for reverse, state in ((True, "applied"), (False, "pending")):
        if not reverse and any(
            hashlib.sha256((engine / relative).read_bytes().replace(b"\r\n", b"\n")).hexdigest() != expected
            for relative, expected in spec["base_sha256"].items()
        ):
            continue  # Zero-context patches are applied only to the exact reviewed upstream files.
        args = ["git", "apply", "--check", "--unidiff-zero"]
        if reverse:
            args.append("--reverse")
        result = subprocess.run(args + [str(patch)], cwd=engine, capture_output=True, check=False)
        if result.returncode == 0:
            return state
    raise InstallError(f"Engine edits conflict with {patch.name}; no files were changed. Resolve the patch manually.")


def custom_configuration(engine):
    path = engine / "custom.py"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    if text.count(BEGIN) != text.count(END) or text.count(BEGIN) > 1:
        raise InstallError("Malformed AIDebug block in custom.py; no files were changed")
    block = (
        BEGIN + "\n"
        "custom_modules = ','.join(dict.fromkeys(\n"
        "    [p for p in globals().get('custom_modules', '').split(',') if p]\n"
        f"    + [{json.dumps(str(MODULE.parent).replace(chr(92), '/'))}]\n"
        "))\n" + END + "\n"
    )
    if BEGIN in text:
        start, end = text.index(BEGIN), text.index(END) + len(END)
        if end < start:
            raise InstallError("Malformed AIDebug block in custom.py")
        return text[:start] + block + text[end:].lstrip("\r\n")
    return text + ("\n" if text and not text.endswith("\n") else "") + block


def inspect(engine):
    profile = validate_engine(engine)
    configuration = custom_configuration(engine)
    patches = [{"file": spec["file"], "state": patch_status(engine, spec)} for spec in profile["patches"]]
    configured = (engine / "custom.py").is_file() and (engine / "custom.py").read_text(
        encoding="utf-8"
    ) == configuration
    installed = False
    if (engine / MANIFEST).is_file():
        recorded = json.loads((engine / MANIFEST).read_text(encoding="utf-8"))
        installed = Path(recorded.get("module_dir", "")).resolve() == MODULE.resolve()
    return {
        "engine": str(engine),
        "module_dir": str(MODULE),
        "profile": profile["version"],
        "patches": patches,
        "configured": configured,
        "status": "installed"
        if installed and configured and all(p["state"] == "applied" for p in patches)
        else "ready_to_install",
    }


def write_if_changed(path, content):
    if not path.exists() or path.read_bytes() != content:
        path.write_bytes(content)


def install(engine, dry_run=False):
    report = inspect(engine)  # Validate every patch and configuration before mutating anything.
    if dry_run:
        return report
    profile = compatibility()
    paths = {engine / relative for spec in profile["patches"] for relative in spec["targets"]}
    paths.update((engine / "custom.py", engine / MANIFEST))
    # --git-path follows a worktree's common Git directory as well as ordinary repositories.
    git_path = subprocess.run(
        ["git", "rev-parse", "--path-format=absolute", "--git-path", "info/exclude"],
        cwd=engine,
        capture_output=True,
        text=True,
        check=False,
    )
    exclude = Path(git_path.stdout.strip()) if git_path.returncode == 0 else None
    if exclude is not None:
        paths.add(exclude)
    before = {path: path.read_bytes() if path.exists() else None for path in paths}
    try:
        for spec, status in zip(profile["patches"], report["patches"]):
            if status["state"] == "pending":
                run(["git", "apply", "--unidiff-zero", ROOT / spec["file"]], cwd=engine)
        write_if_changed(engine / "custom.py", custom_configuration(engine).encode("utf-8"))
        record = {
            "schema": 1,
            "module_dir": str(MODULE),
            "repository_root": str(ROOT.parent),
            "engine_profile": profile["version"],
            "base_commit": profile["base_commit"],
        }
        write_if_changed(engine / MANIFEST, (json.dumps(record, indent=2) + "\n").encode("utf-8"))
        if exclude is not None:
            exclude.parent.mkdir(parents=True, exist_ok=True)
            text = exclude.read_text(encoding="utf-8") if exclude.exists() else ""
            if "/" + MANIFEST not in text.splitlines():
                write_if_changed(exclude, (text.rstrip("\n") + "\n/" + MANIFEST + "\n").encode("utf-8"))
    except Exception:
        for path, content in before.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(content)
        raise
    return inspect(engine)


def managed_build(engine):
    run(
        [
            "dotnet",
            "build",
            MODULE / "managed/Godot.AIDebug/Godot.AIDebug.csproj",
            "-c",
            "Release",
            "--nologo",
            "-v:minimal",
            f"-p:GodotSourceRoot={engine}",
        ],
        cwd=engine,
    )
    output = engine / "bin/GodotSharp/AIDebug"
    output.mkdir(parents=True, exist_ok=True)
    for path in (MODULE / "managed/Godot.AIDebug/bin/Release/net10.0").iterdir():
        if path.suffix in (".dll", ".pdb"):
            shutil.copy2(path, output / path.name)
    guide = engine / "modules/ai_guide/managed/Godot.AIGuide/Godot.AIGuide.csproj"
    if guide.is_file():
        run(
            [
                "dotnet",
                "build",
                guide,
                "-c",
                "Release",
                "--nologo",
                "-v:minimal",
                f"-p:AIDebugModuleDir={MODULE}",
            ],
            cwd=engine,
        )
        guide_output = engine / "bin/GodotSharp/Tools"
        guide_output.mkdir(parents=True, exist_ok=True)
        for suffix in (".dll", ".pdb"):
            source = guide.parent / "bin/Release/net10.0" / ("Godot.AIGuide" + suffix)
            shutil.copy2(source, guide_output / source.name)


def find_editor(engine):
    platform_name = {"Windows": "windows", "Linux": "linuxbsd", "Darwin": "macos"}[platform.system()]
    candidates = [
        p
        for p in (engine / "bin").glob(f"godot.{platform_name}.editor.*.mono*")
        if p.is_file()
        and ".console" not in p.name
        and (p.suffix == ".exe" if os.name == "nt" else p.name.endswith(".mono"))
    ]
    if not candidates:
        raise InstallError("No .NET editor found in engine/bin; build target=editor first")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def build(engine, args):
    install(engine)
    if not args.managed_only:
        platform_name = {"Windows": "windows", "Linux": "linuxbsd", "Darwin": "macos"}[platform.system()]
        run(
            [
                "uvx",
                "scons",
                f"-j{args.jobs}",
                f"platform={platform_name}",
                f"target={args.target}",
                "module_mono_enabled=yes",
                "debug_symbols=yes",
                "accesskit=no",
                *args.scons_arg,
            ],
            cwd=engine,
        )
    if args.native_only:
        return
    if args.managed_only:
        managed_build(engine)
    elif args.target == "editor":
        run(
            [
                find_editor(engine),
                "--headless",
                "--generate-mono-glue",
                engine / "modules/mono/glue",
            ],
            cwd=engine,
        )
        run(
            [
                sys.executable,
                engine / "modules/mono/build_scripts/build_assemblies.py",
                "--godot-output-dir",
                engine / "bin",
            ],
            cwd=engine,
        )
    else:
        managed_build(engine)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "install", "build", "test"))
    parser.add_argument("--engine", required=True, type=Path, help="Godot source directory")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Check install prerequisites without changing files",
    )
    parser.add_argument("--jobs", type=int, default=min(28, os.cpu_count() or 1))
    parser.add_argument("--target", choices=("editor", "template_debug"), default="editor")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--managed-only",
        action="store_true",
        help="Rebuild and deploy only AI Debug / optional AI Guide assemblies",
    )
    mode.add_argument(
        "--native-only",
        action="store_true",
        help="Rebuild only the native editor or debug template",
    )
    parser.add_argument(
        "--scons-arg",
        action="append",
        default=[],
        help="Additional SCons key=value, repeat as needed",
    )
    parser.add_argument(
        "--bridge",
        action="store_true",
        help="Exercise the editor bridge and optional AIGuide forwarding",
    )
    parser.add_argument(
        "--native-dll",
        type=Path,
        help="Compiled tests/native_fixture.c DLL for ABI regression",
    )
    args = parser.parse_args()
    engine = args.engine.expanduser().resolve()
    try:
        if args.action == "check" or args.dry_run:
            print(json.dumps(inspect(engine), indent=2))
        elif args.action == "install":
            print(json.dumps(install(engine), indent=2))
        elif args.action == "build":
            build(engine, args)
        else:
            if inspect(engine)["status"] != "installed":
                raise InstallError("Run install first")
            env = os.environ.copy()
            env["GODOT_AI_DEBUG_TEST_ENGINE"] = str(find_editor(engine))
            env["GODOT_AI_DEBUG_TEST_BRIDGE"] = "1" if args.bridge else "0"
            if args.native_dll:
                env["GODOT_AI_DEBUG_TEST_NATIVE_DLL"] = str(args.native_dll.resolve())
            run([sys.executable, ROOT / "tests/test_runtime.py"], cwd=engine, env=env)
    except (InstallError, OSError, ValueError, KeyError) as error:
        print(f"AI Debug: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
