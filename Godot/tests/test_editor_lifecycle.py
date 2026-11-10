"""Exercise graceful editor shutdown before, during and after a debug session."""

import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path


def free_port():
    with socket.socket() as server:
        server.bind(("127.0.0.1", 0))
        return server.getsockname()[1]


class EditorLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        editor = os.environ.get("GODOT_AI_DEBUG_TEST_ENGINE")
        if not editor:
            raise unittest.SkipTest("Set GODOT_AI_DEBUG_TEST_ENGINE to an integrated Godot editor")
        cls.editor = Path(editor).resolve()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aidebug-editor-lifecycle-")
        self.addCleanup(temporary.cleanup)
        self.project = Path(temporary.name).resolve() / "Project with spaces"
        self.project.mkdir()
        self.token = secrets.token_hex(16)
        self.name = "AIDebugLifecycle-" + secrets.token_hex(12)
        self.env = os.environ.copy()
        self.env.pop("GODOT_AI_DEBUG", None)
        self.env["GODOT_AI_DEBUG_TOKEN"] = self.token
        self.env["GODOT_AI_GUIDE_PORT"] = str(free_port())
        self.project.joinpath("project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="' + self.name + '"\n'
            'config/features=PackedStringArray("4.8", "GL Compatibility")\n'
            'run/main_scene="res://main.tscn"\nrun/max_fps=120\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
            "[ai_debug]\nshow_overlay=false\n",
            encoding="utf-8",
        )
        self.project.joinpath("main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://main.gd" id="1"]\n'
            '[node name="Probe" type="Node"]\nscript=ExtResource("1")\n',
            encoding="utf-8",
        )
        self.project.joinpath("main.gd").write_text(
            'extends Node\nfunc _process(_delta):\n\tif FileAccess.file_exists("res://quit.flag"):\n'
            "\t\tget_tree().quit()\n",
            encoding="utf-8",
        )
        self.addCleanup(self.clean_user_directory)

    def clean_user_directory(self):
        parent = (
            Path(os.environ["APPDATA"]) / "Godot/app_userdata"
            if os.name == "nt"
            else Path.home() / ".local/share/godot/app_userdata"
        )
        directory = parent / self.name
        if directory.is_dir() and directory.resolve().parent == parent.resolve() and directory.name == self.name:
            shutil.rmtree(directory)

    @staticmethod
    def stop_process(process):
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)

    def start(self, name, *arguments):
        log = self.project / (name + ".log")
        output = log.open("w", encoding="utf-8")
        self.addCleanup(output.close)
        process = subprocess.Popen(
            [str(self.editor), "--headless", "--path", str(self.project), *arguments],
            env=self.env,
            stdout=output,
            stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        self.addCleanup(self.stop_process, process)
        return process, log

    def assert_clean_exit(self, process, log):
        code = process.wait(timeout=30)
        content = log.read_text(encoding="utf-8", errors="replace").replace(self.token, "<redacted>")
        content = re.sub(r"Token: \S+", "Token: <redacted>", content)
        self.assertEqual(code, 0, content[-6000:])
        self.assertNotIn("ERROR:", content, content[-6000:])
        self.assertNotIn("ObjectDB instances leaked", content, content[-6000:])

    def test_empty_editor_exits_cleanly(self):
        process, log = self.start("editor", "--editor", "--quit-after", "120")
        self.assert_clean_exit(process, log)

    def bridge_info(self):
        request = urllib.request.Request(
            "http://127.0.0.1:7651/mcp",
            data=json.dumps(
                {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "info", "arguments": {}}}
            ).encode(),
            headers={"Content-Type": "application/json", "X-AIDebug-Token": self.token},
            method="POST",
        )
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        with opener.open(request, timeout=1) as response:
            result = json.load(response)["result"]
        if result.get("isError"):
            return None
        return json.loads(result["content"][0]["text"])

    def debug_session_exit(self, game_first):
        # The bridge uses the engine's fixed editor port. Do not disturb another editor.
        with socket.socket() as probe:
            try:
                probe.bind(("127.0.0.1", 7651))
            except OSError:
                self.skipTest("Port 7651 is occupied by another process")
        debugger_port = free_port()
        editor, editor_log = self.start(
            "editor",
            "--editor",
            "--debug-server",
            f"tcp://127.0.0.1:{debugger_port}",
            "--max-fps",
            "120",
            "--quit-after",
            "1200",
        )
        deadline = time.monotonic() + 15
        while True:
            self.assertIsNone(editor.poll(), "Editor exited before debugger startup")
            try:
                with socket.create_connection(("127.0.0.1", debugger_port), timeout=0.1):
                    break
            except OSError:
                self.assertLess(time.monotonic(), deadline, "Debugger startup timed out")
                time.sleep(0.05)
        game, game_log = self.start("game", "--remote-debug", f"tcp://127.0.0.1:{debugger_port}")
        deadline = time.monotonic() + 15
        while True:
            self.assertIsNone(game.poll(), "Game exited before the debug session started")
            try:
                info = self.bridge_info()
                if info and info.get("project_name") == self.name:
                    break
            except (OSError, ValueError, KeyError):
                pass
            self.assertLess(time.monotonic(), deadline, "Editor bridge startup timed out")
            time.sleep(0.05)
        with socket.create_connection(("127.0.0.1", 7651), timeout=2) as pending:
            pending.sendall(b"POST /mcp HTTP/1.1\r\n")
            if game_first:
                self.project.joinpath("quit.flag").touch()
                self.assert_clean_exit(game, game_log)
            else:
                self.assertIsNone(game.poll(), "Debug session must still be active")
            self.assert_clean_exit(editor, editor_log)
            try:
                self.assertEqual(pending.recv(1), b"", "Bridge client was not disconnected")
            except ConnectionResetError:
                pass
        if not game_first:
            self.project.joinpath("quit.flag").touch()
            self.assert_clean_exit(game, game_log)
        with socket.socket() as probe:
            probe.settimeout(1)
            self.assertNotEqual(probe.connect_ex(("127.0.0.1", 7651)), 0, "Bridge listener survived editor exit")

    def test_editor_exits_after_game_stops(self):
        self.debug_session_exit(game_first=True)

    def test_editor_exits_with_active_debug_session(self):
        self.debug_session_exit(game_first=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
