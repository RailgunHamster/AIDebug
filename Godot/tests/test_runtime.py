"""No-project-installation regressions against a built .NET Godot executable.

GODOT_AI_DEBUG_TEST_ENGINE selects the executable. Optional
GODOT_AI_DEBUG_TEST_NATIVE_DLL enables the Windows x64 ABI regression.
"""

import concurrent.futures
import itertools
import json
import os
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path


class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        executable = os.environ.get("GODOT_AI_DEBUG_TEST_ENGINE")
        if not executable:
            raise unittest.SkipTest("Set GODOT_AI_DEBUG_TEST_ENGINE to a built .NET executable")
        cls.temp = tempfile.TemporaryDirectory(prefix="godot-ai-debug-test-")
        cls.project = Path(cls.temp.name)
        cls.token = secrets.token_hex(16)
        cls.ids = itertools.count(1)
        cls.user_dir = None
        cls.editor_process = None
        cls.bridge = os.environ.get("GODOT_AI_DEBUG_TEST_BRIDGE") == "1"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            cls.port = reservation.getsockname()[1]
        if cls.bridge:
            debug_port = cls.port
            cls.port = 7651
        cls.url = f"http://127.0.0.1:{cls.port}/mcp"
        cls.project.joinpath("project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="AIDebugTest-' + cls.token + '"\n'
            'run/main_scene="res://main.tscn"\nrun/max_fps=120\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            "[ai_debug]\nshow_overlay=false\n",
            encoding="utf-8",
        )
        cls.project.joinpath("main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://main.gd" id="1"]\n'
            '[node name="Probe" type="Node3D"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        cls.project.joinpath("main.gd").write_text("extends Node3D\nvar value: int = 42\n", encoding="utf-8")
        cls.output = cls.project.joinpath("engine.log").open("w", encoding="utf-8")
        engine_args = [executable, "--headless", "--path", cls.temp.name]
        child_env = os.environ.copy()
        child_env.pop("GODOT_AI_DEBUG", None)
        child_env["GODOT_AI_DEBUG_TOKEN"] = cls.token
        child_env["DOTNET_JitNoInline"] = "1"
        if cls.bridge:
            cls.editor_process = subprocess.Popen(
                engine_args + ["--editor", "--debug-server", f"tcp://127.0.0.1:{debug_port}"],
                env=child_env,
                stdout=cls.output,
                stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
            )
            deadline = time.monotonic() + 30
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", debug_port), timeout=0.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        cls.editor_process.kill()
                        cls.editor_process.wait(timeout=5)
                        raise RuntimeError("Editor debugger did not start")
                    time.sleep(0.1)
            engine_args += ["--remote-debug", f"tcp://127.0.0.1:{debug_port}"]
        else:
            engine_args += [f"--ai-debug-port={cls.port}", f"--ai-debug-token={cls.token}"]
        cls.process = subprocess.Popen(
            engine_args,
            env=child_env,
            stdout=cls.output,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        cls.addClassCleanup(cls.cleanup)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                raise RuntimeError(cls.project.joinpath("engine.log").read_text(encoding="utf-8"))
            try:
                cls.rpc("ping")
                break
            except (OSError, urllib.error.URLError):
                time.sleep(0.05)
        else:
            raise RuntimeError("AI Debug did not start")
        cls.user_dir = Path(cls.tool("run_gd", code="return OS.get_user_data_dir()")["result"])

    @classmethod
    def cleanup(cls):
        if cls.process.poll() is None:
            try:
                cls.tool("run_gd", code="ctx.tree.quit(); return true")
                cls.process.wait(timeout=5)
            except (OSError, TimeoutError, subprocess.TimeoutExpired):
                cls.process.kill()
                cls.process.wait(timeout=5)
        cls.output.close()
        if cls.editor_process is not None:
            cls.editor_process.terminate()
            cls.editor_process.wait(timeout=5)
        if cls.user_dir and cls.user_dir.name == "AIDebugTest-" + cls.token:
            expected_parent = (
                Path(os.environ["APPDATA"]) / "Godot" / "app_userdata"
                if os.name == "nt"
                else Path.home() / ".local/share/godot/app_userdata"
            )
            if cls.user_dir.resolve().parent == expected_parent.resolve():
                shutil.rmtree(cls.user_dir)
        cls.temp.cleanup()

    @classmethod
    def rpc(cls, method, params=None):
        message = {"jsonrpc": "2.0", "id": next(cls.ids), "method": method, "params": params or {}}
        request = urllib.request.Request(
            cls.url,
            data=json.dumps(message).encode(),
            headers={"Content-Type": "application/json", "X-AIDebug-Token": cls.token},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=15) as response:
            result = json.load(response)
        if result.get("id") != message["id"]:
            raise AssertionError(f"Response delivered to the wrong client: {result}")
        return result

    @classmethod
    def tool(cls, name, **args):
        result = cls.rpc("tools/call", {"name": name, "arguments": args})["result"]
        text = result["content"][0]["text"]
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            data = {"error": text}
        data["isError"] = result.get("isError", False)
        return data

    def setUp(self):
        self.assertTrue(self.tool("reset")["ok"])

    def test_generic_tools(self):
        tools = self.rpc("tools/list")["result"]["tools"]
        self.assertEqual(len(tools), 21)
        self.assertFalse(any(t["name"].startswith(("snapshot_", "input_")) for t in tools))
        self.assertTrue(self.tool("snapshot_save")["isError"])

    def test_forwarded_tool_schema(self):
        if not self.bridge:
            self.skipTest("AIGuide forwarding is exercised in bridge mode")
        message = {"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}
        request = urllib.request.Request(
            "http://127.0.0.1:7650/mcp",
            data=json.dumps(message).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            forwarded = json.load(response)["result"]["tools"]
        schemas = {t["name"][9:]: t["inputSchema"] for t in forwarded if t["name"].startswith("ai_debug_")}
        actual = {t["name"]: t["inputSchema"] for t in self.rpc("tools/list")["result"]["tools"]}
        self.assertEqual(schemas, actual)
        message.update(method="tools/call", params={"name": "ai_debug_info", "arguments": {}})
        request = urllib.request.Request(
            "http://127.0.0.1:7650/mcp",
            data=json.dumps(message).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            forwarded_info = json.loads(json.load(response)["result"]["content"][0]["text"])
        self.assertEqual(forwarded_info["project_name"], "AIDebugTest-" + self.token)

    def test_builtin_requires_no_project_installation(self):
        self.assertFalse((self.project / "addons").exists())
        self.assertFalse(list(self.project.glob("*.csproj")))
        self.assertNotIn("[editor_plugins]", (self.project / "project.godot").read_text())
        self.assertEqual(self.tool("eval_cs", code="return 6 * 7;")["result"], 42)

    def test_gd_and_properties(self):
        self.assertEqual(self.tool("eval_gd", expression="value + 1", self="/root/Probe")["result"], 43)
        self.assertTrue(self.tool("set", node="/root/Probe", property="value", value=99)["ok"])
        self.tool("reset")
        self.assertEqual(self.tool("get", node="/root/Probe", property="value")["value"], 99)
        self.tool("set", node="/root/Probe", property="value", value=42)
        result = self.tool("set", node="/root/Probe", property="position:x", value=3.5)
        self.assertEqual(result["value"], 3.5)
        self.assertEqual(self.tool("get", node="/root/Probe", property="position:x")["value"], 3.5)

    def test_cs_session_and_compile_error(self):
        self.assertTrue(self.tool("eval_cs", code="int probeValue = 42;")["ok"])
        self.assertTrue(self.tool("eval_cs", code="this is not valid C#")["isError"])
        self.assertEqual(self.tool("eval_cs", code="return probeValue;")["result"], 42)

    def test_cs_main_async_and_worker_guard(self):
        self.assertEqual(
            self.tool("eval_cs", code="await Root.ToSignal(Tree, SceneTree.SignalName.ProcessFrame); return 42;")[
                "result"
            ],
            42,
        )
        self.assertTrue(self.tool("eval_cs", code="return Tree.Root.Name;", thread="worker")["isError"])

    def test_worker_connections_and_busy(self):
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(
                self.tool, "eval_cs", code="System.Threading.Thread.Sleep(600); return 123;", thread="worker"
            )
            time.sleep(0.15)
            busy = self.tool("eval_cs", code="return 0;")
            self.assertTrue(busy.get("busy"), busy)
            for _ in range(8):
                self.rpc("ping")
            self.assertEqual(future.result()["result"], 123)

    def test_worker_timeout_and_reset(self):
        result = self.tool(
            "eval_cs", code="System.Threading.Thread.Sleep(1200); return 1;", thread="worker", timeout_ms=100
        )
        self.assertTrue(result["timed_out"], result)
        self.assertTrue(result["execution_may_continue"])
        start = time.monotonic()
        reset = self.tool("reset")
        self.assertLess(time.monotonic() - start, 0.5)
        self.assertTrue(reset["execution_may_continue"])
        self.assertTrue(self.tool("eval_cs", code="return 0;").get("busy"))
        time.sleep(1.2)
        self.assertEqual(self.tool("eval_cs", code="return 42;")["result"], 42)

    def test_reset_pending_evaluation(self):
        with concurrent.futures.ThreadPoolExecutor() as pool:
            future = pool.submit(
                self.tool, "eval_cs", code="System.Threading.Thread.Sleep(500); return 1;", thread="worker"
            )
            time.sleep(0.15)
            self.tool("reset")
            self.assertTrue(future.result()["cancelled"])
        time.sleep(0.5)

    def test_harmony_and_native_constant(self):
        code = 'return Godot.AIDebug.PatchHelpers.Node("/root/Probe") == null;'
        self.assertFalse(self.tool("eval_cs", code=code)["result"])
        patch = self.tool(
            "patch_cs",
            target="Godot.AIDebug.PatchHelpers:Node(string)",
            kind="prefix",
            code="__result = null; return false;",
        )
        self.assertTrue(patch["ok"], patch)
        self.assertTrue(self.tool("eval_cs", code=code)["result"])
        self.assertTrue(self.tool("unpatch", id=patch["id"])["ok"])
        self.assertFalse(self.tool("eval_cs", code=code)["result"])
        if os.name == "nt":
            self.assertTrue(
                self.tool("hook_native", symbol="SceneTree::get_node_count", mode="return_const", return_const=777)[
                    "ok"
                ]
            )
            self.assertEqual(
                self.tool("eval_gd", expression="get_tree().get_node_count()", self="/root/Probe")["result"], 777
            )
            self.assertTrue(self.tool("unhook_native", id="all")["ok"])
            self.assertNotEqual(
                self.tool("eval_gd", expression="get_tree().get_node_count()", self="/root/Probe")["result"], 777
            )

    def test_native_log_four_floats(self):
        dll = os.environ.get("GODOT_AI_DEBUG_TEST_NATIVE_DLL")
        if not dll:
            self.skipTest("Set GODOT_AI_DEBUG_TEST_NATIVE_DLL to the compiled native_fixture library")
        code = (
            "using System.Runtime.InteropServices; "
            "public delegate double ProbeFunction(double a, double b, double c, double d); "
            f"var probeLibrary = NativeLibrary.Load({json.dumps(str(Path(dll).resolve()))}); "
            'var probeFunction = Marshal.GetDelegateForFunctionPointer<ProbeFunction>(NativeLibrary.GetExport(probeLibrary, "probe_four_doubles")); '
            "return probeFunction(1, 2, 3, 4);"
        )
        self.assertEqual(self.tool("eval_cs", code=code)["result"], 30)
        hook = self.tool("hook_native", symbol=Path(dll).name + "!probe_four_doubles", mode="log")
        self.assertTrue(hook["ok"], hook)
        for _ in range(3):
            self.assertEqual(self.tool("eval_cs", code="return probeFunction(1, 2, 3, 4);")["result"], 30)
        hooks = self.tool("list_native_hooks")["native_hooks"]
        self.assertGreaterEqual(hooks[0]["call_count"], 3)
        self.assertTrue(self.tool("unhook_native", id=hook["id"])["ok"])

    def test_http_validation(self):
        for header, status in [
            ("Content-Length: -1", 400),
            ("Content-Length: 9999999999", 413),
            ("Origin: https://example.com\r\nContent-Length: 0", 403),
        ]:
            with socket.create_connection(("127.0.0.1", self.port), timeout=3) as connection:
                connection.sendall(
                    (
                        f"POST /mcp HTTP/1.1\r\nHost: localhost\r\nX-AIDebug-Token: {self.token}\r\n{header}\r\n\r\n"
                    ).encode()
                )
                self.assertIn(f" {status} ".encode(), connection.recv(1024).split(b"\r\n")[0])

    def test_z_shutdown_with_running_worker(self):
        result = self.tool(
            "eval_cs", code="System.Threading.Thread.Sleep(30000); return 0;", thread="worker", timeout_ms=150
        )
        self.assertTrue(result.get("execution_may_continue"), result)
        self.tool("run_gd", code="ctx.tree.quit(); return true")
        self.process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
