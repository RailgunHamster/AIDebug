#!/usr/bin/env python3
"""
aidbg: Command-line client for Godot's built-in `ai_debug` module (MCP Streamable HTTP).

Examples:
  python aidbg.py --port 7651 --token XXXX info
  python aidbg.py eval-cs "Root.FindChild(\"Player\", true, false).Get(\"position\")"
  python aidbg.py patch-cs --target "Riichi.Game.Rules:CanRon(Riichi.Game.Hand)" --kind prefix --file patch.cs
  python aidbg.py disconnect
"""

from __future__ import annotations

import argparse
import base64
import glob
import json
import os
import sys
import urllib.error
import urllib.request
from typing import Any


def discover_session(port: int | None = None) -> dict[str, Any]:
    """Auto-discover port and token from recent user://ai_debug/session.json files on Windows/Linux/macOS."""
    candidates: list[str] = []
    if os.name == "nt":
        appdata = os.environ.get("APPDATA", "")
        if appdata:
            candidates += glob.glob(os.path.join(appdata, "Godot", "app_userdata", "*", "ai_debug", "session.json"))
    else:
        home = os.path.expanduser("~")
        candidates += glob.glob(
            os.path.join(home, ".local", "share", "godot", "app_userdata", "*", "ai_debug", "session.json")
        )
        candidates += glob.glob(
            os.path.join(
                home, "Library", "Application Support", "Godot", "app_userdata", "*", "ai_debug", "session.json"
            )
        )

    candidates.sort(key=lambda p: os.path.getmtime(p) if os.path.exists(p) else 0, reverse=True)
    for path in candidates:
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if port is None or data.get("port") == port:
                return data
        except Exception:
            continue
    return {}


def parse_value_arg(raw: str) -> Any:
    try:
        return json.loads(raw)
    except Exception:
        return raw


def call_mcp(
    host: str, port: int, token: str, method: str, params: dict[str, Any], timeout: float = 30.0
) -> dict[str, Any]:
    url = f"http://{host}:{port}/mcp"
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": method,
        "params": params,
    }
    body = json.dumps(payload).encode("utf-8")
    headers = {
        "Content-Type": "application/json; charset=utf-8",
        "X-AIDebug-Token": token,
    }
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            resp_text = resp.read().decode("utf-8", errors="replace")
            return json.loads(resp_text) if resp_text else {}
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        print(f"HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
        sys.exit(1)
    except urllib.error.URLError as e:
        print(f"Connection failed ({url}): {e.reason}", file=sys.stderr)
        sys.exit(1)


def send_disconnect(host: str, port: int, token: str, timeout: float = 10.0) -> None:
    url = f"http://{host}:{port}/mcp"
    headers = {"X-AIDebug-Token": token}
    req = urllib.request.Request(url, headers=headers, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            print(json.dumps({"ok": True, "status": resp.status, "disconnected": True}, indent=2, ensure_ascii=False))
    except urllib.error.HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")
        print(f"HTTP {e.code} {e.reason}: {err_body}", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")

    parser = argparse.ArgumentParser(prog="aidbg", description="Godot ai_debug CLI client")
    parser.add_argument(
        "--host", default=os.environ.get("AIDBG_HOST", "127.0.0.1"), help="Server host (default: 127.0.0.1)"
    )
    parser.add_argument(
        "--port", type=int, default=int(os.environ.get("AIDBG_PORT", "0")), help="Server port (default: 7651)"
    )
    parser.add_argument("--token", default=os.environ.get("AIDBG_TOKEN", ""), help="Auth token (X-AIDebug-Token)")
    parser.add_argument("--timeout", type=float, default=30.0, help="HTTP request timeout in seconds")
    parser.add_argument("--raw", action="store_true", help="Print raw JSON-RPC envelope")

    sub = parser.add_subparsers(dest="cmd", required=True)

    # info
    sub.add_parser("info", help="Show engine, project, .NET runtime, and active patch info")

    # tree
    p_tree = sub.add_parser("tree", help="Export scene tree")
    p_tree.add_argument("--root", default="", help="Starting node path")
    p_tree.add_argument("--depth", type=int, default=6, help="Max depth")
    p_tree.add_argument("--props", action="store_true", help="Include property summary")
    p_tree.add_argument("--properties", nargs="*", help="Specific property names to include")

    # get
    p_get = sub.add_parser("get", help="Read a node property")
    p_get.add_argument("node", help="Node path or #ObjectID")
    p_get.add_argument("property", help="Property name")

    # set
    p_set = sub.add_parser("set", help="Write a node property")
    p_set.add_argument("node", help="Node path or #ObjectID")
    p_set.add_argument("property", help="Property name")
    p_set.add_argument("value", help="Value (JSON or GDScript Variant string like 'Vector3(1,2,3)')")

    # call
    p_call = sub.add_parser("call", help="Call a node method")
    p_call.add_argument("node", help="Node path or #ObjectID")
    p_call.add_argument("method", help="Method name")
    p_call.add_argument("args", nargs="*", help="Method arguments (JSON or string)")

    # eval-gd
    p_eval_gd = sub.add_parser("eval-gd", aliases=["eval_gd"], help="Evaluate a GDScript Expression")
    p_eval_gd.add_argument("expression", help="GDScript expression string")
    p_eval_gd.add_argument("--self", dest="self_node", default="", help="Node path for `self`")

    # run-gd
    p_run_gd = sub.add_parser("run-gd", aliases=["run_gd"], help="Run a temporary GDScript snippet")
    p_run_gd.add_argument("code", nargs="?", default="", help="GDScript code string")
    p_run_gd.add_argument("--file", "-f", default="", help="Read GDScript code from file")

    # eval-cs
    p_eval_cs = sub.add_parser("eval-cs", aliases=["eval_cs"], help="Evaluate C# code via Roslyn Scripting")
    p_eval_cs.add_argument("code", nargs="?", default="", help="C# code string")
    p_eval_cs.add_argument("--file", "-f", default="", help="Read C# code from file")
    p_eval_cs.add_argument("--thread", choices=["main", "worker"], default="main", help="Execution thread")
    p_eval_cs.add_argument("--timeout-ms", type=int, default=10000, help="Execution timeout in ms")
    p_eval_cs.add_argument(
        "--reset-session", action="store_true", help="Reset Roslyn script session state before running"
    )

    # patch-cs
    p_patch_cs = sub.add_parser("patch-cs", aliases=["patch_cs"], help="Apply a Harmony C# method patch")
    p_patch_cs.add_argument("--target", "-t", required=True, help="Target method: 'Namespace.Type:Method(ParamTypes)'")
    p_patch_cs.add_argument(
        "--kind", "-k", choices=["prefix", "postfix", "finalizer", "transpiler", "replace"], default="prefix"
    )
    p_patch_cs.add_argument("--code", "-c", default="", help="Inline C# patch code")
    p_patch_cs.add_argument("--file", "-f", default="", help="Read C# patch code from file")

    # unpatch
    p_unpatch = sub.add_parser("unpatch", help="Revert a patch by ID or all patches")
    p_unpatch.add_argument("id", nargs="?", default="all", help="Patch ID (e.g. p1) or 'all'")

    # list-patches
    sub.add_parser("list-patches", aliases=["list_patches"], help="List active Harmony patches")

    # pause
    p_pause = sub.add_parser("pause", help="Pause or resume game")
    p_pause.add_argument("state", nargs="?", choices=["true", "false", "1", "0", "on", "off"], default=None)

    # step
    p_step = sub.add_parser("step", help="Step N frames while paused")
    p_step.add_argument("frames", nargs="?", type=int, default=1, help="Number of frames to step")

    # time-scale
    p_ts = sub.add_parser("time-scale", aliases=["time_scale"], help="Get or set Engine.time_scale")
    p_ts.add_argument("scale", nargs="?", type=float, default=None, help="New time scale")

    # screenshot
    p_shot = sub.add_parser("screenshot", help="Capture main viewport screenshot")
    p_shot.add_argument("--out", "-o", default="", help="Save PNG to local path (or pass to engine)")
    p_shot.add_argument("--max-width", type=int, default=0, help="Resize if wider than max_width")

    # logs
    p_logs = sub.add_parser("logs", help="Fetch recent engine logs")
    p_logs.add_argument("--limit", "-n", type=int, default=100, help="Max log lines")
    p_logs.add_argument("--level", default="", help="Filter by level (info, warning, error)")
    p_logs.add_argument("--clear", action="store_true", help="Clear log buffer after reading")

    # dump
    p_dump = sub.add_parser("dump", help="Write Windows minidump (mini or full)")
    p_dump.add_argument("--mode", choices=["mini", "full"], default="mini")
    p_dump.add_argument("--path", default="", help="Optional output .dmp path")

    # hook-native (Phase 2)
    p_hn = sub.add_parser(
        "hook-native", aliases=["hook_native"], help="Hook a native C/C++ symbol via MinHook + DbgHelp (Windows x64)"
    )
    p_hn.add_argument("symbol", help="Symbol name (e.g. 'SceneTree::is_paused'), 'module!export', or '0x...'")
    p_hn.add_argument("--mode", choices=["log", "return_const", "dll"], default="log")
    p_hn.add_argument("--return-const", type=int, default=0, help="Constant return value for mode=return_const")
    p_hn.add_argument("--dll", default="", help="Replacement DLL path for mode=dll")
    p_hn.add_argument("--export", default="", help="Replacement DLL exported function name")

    # unhook-native (Phase 2)
    p_uhn = sub.add_parser(
        "unhook-native", aliases=["unhook_native"], help="Remove a native hook by ID, symbol, or 'all'"
    )
    p_uhn.add_argument("id", nargs="?", default="all")

    # list-native-hooks (Phase 2)
    sub.add_parser("list-native-hooks", aliases=["list_native_hooks"], help="List active native hooks and logged calls")

    # adb-forward (Phase 2 Android helper)
    p_adb = sub.add_parser("adb-forward", help="Run `adb forward tcp:<port> tcp:<port>` for Android debugging")
    p_adb.add_argument("--device", "-s", default="", help="Optional adb device serial")

    # reset
    sub.add_parser("reset", help="Revert all patches/hooks, reset C# state, and restore pause/time_scale")

    # disconnect
    sub.add_parser("disconnect", help="Send HTTP DELETE /mcp to disconnect session and trigger auto-revert")

    args = parser.parse_args()

    port = args.port
    token = args.token
    if not port or not token:
        sess = discover_session(port if port > 0 else None)
        if not port:
            port = int(sess.get("port", 7651))
        if not token:
            token = str(sess.get("token", ""))

    if args.cmd == "adb-forward":
        import subprocess

        target_port = port if port > 0 else 7651
        adb_cmd = ["adb"]
        if args.device:
            adb_cmd += ["-s", args.device]
        adb_cmd += ["forward", f"tcp:{target_port}", f"tcp:{target_port}"]
        rc = subprocess.call(adb_cmd)
        print(json.dumps({"ok": rc == 0, "command": " ".join(adb_cmd), "port": target_port}, indent=2))
        sys.exit(rc)

    if args.cmd == "disconnect":
        send_disconnect(args.host, port, token, args.timeout)
        return

    tool_name = args.cmd.replace("-", "_")
    tool_args: dict[str, Any] = {}

    if tool_name == "info":
        pass
    elif tool_name == "tree":
        if args.root:
            tool_args["root"] = args.root
        tool_args["depth"] = args.depth
        tool_args["include_properties"] = args.props
        if args.properties:
            tool_args["properties"] = args.properties
    elif tool_name == "get":
        tool_args["node"] = args.node
        tool_args["property"] = args.property
    elif tool_name == "set":
        tool_args["node"] = args.node
        tool_args["property"] = args.property
        tool_args["value"] = parse_value_arg(args.value)
    elif tool_name == "call":
        tool_args["node"] = args.node
        tool_args["method"] = args.method
        tool_args["args"] = [parse_value_arg(x) for x in (args.args or [])]
    elif tool_name == "eval_gd":
        tool_args["expression"] = args.expression
        if args.self_node:
            tool_args["self"] = args.self_node
    elif tool_name == "run_gd":
        code = args.code
        if args.file:
            with open(args.file, "r", encoding="utf-8") as f:
                code = f.read()
        tool_args["code"] = code
    elif tool_name == "eval_cs":
        code = args.code
        if args.file:
            with open(args.file, "r", encoding="utf-8") as f:
                code = f.read()
        tool_args["code"] = code
        tool_args["thread"] = args.thread
        tool_args["timeout_ms"] = args.timeout_ms
        if args.reset_session:
            tool_args["reset_session"] = True
    elif tool_name == "patch_cs":
        code = args.code
        if args.file:
            with open(args.file, "r", encoding="utf-8") as f:
                code = f.read()
        tool_args["target"] = args.target
        tool_args["kind"] = args.kind
        tool_args["code"] = code
    elif tool_name == "unpatch":
        tool_args["id"] = args.id
    elif tool_name == "list_patches":
        pass
    elif tool_name == "pause":
        if args.state is not None:
            tool_args["paused"] = args.state in ("true", "1", "on")
    elif tool_name == "step":
        tool_args["frames"] = args.frames
    elif tool_name == "time_scale":
        if args.scale is not None:
            tool_args["scale"] = args.scale
    elif tool_name == "screenshot":
        if args.out:
            tool_args["path"] = os.path.abspath(args.out)
            tool_args["base64"] = False
        else:
            tool_args["base64"] = True
        if args.max_width > 0:
            tool_args["max_width"] = args.max_width
    elif tool_name == "logs":
        tool_args["limit"] = args.limit
        if args.level:
            tool_args["level"] = args.level
        if args.clear:
            tool_args["clear"] = True
    elif tool_name == "dump":
        tool_args["mode"] = args.mode
        if args.path:
            tool_args["path"] = os.path.abspath(args.path)
    elif tool_name == "hook_native":
        tool_args["symbol"] = args.symbol
        tool_args["mode"] = args.mode
        tool_args["return_const"] = args.return_const
        if args.dll:
            tool_args["dll_path"] = os.path.abspath(args.dll)
        if args.export:
            tool_args["export"] = args.export
    elif tool_name == "unhook_native":
        tool_args["id"] = args.id
    elif tool_name == "list_native_hooks":
        pass
    elif tool_name == "reset":
        pass

    resp = call_mcp(
        args.host, port, token, "tools/call", {"name": tool_name, "arguments": tool_args}, timeout=args.timeout
    )

    if args.raw:
        print(json.dumps(resp, indent=2, ensure_ascii=False))
        return

    if "error" in resp:
        print(json.dumps(resp["error"], indent=2, ensure_ascii=False), file=sys.stderr)
        sys.exit(1)

    result = resp.get("result", {})
    is_error = result.get("isError", False)
    content = result.get("content", [])

    for item in content:
        itype = item.get("type")
        if itype == "text":
            print(item.get("text", ""))
        elif itype == "image" and args.cmd == "screenshot" and args.out and not os.path.exists(args.out):
            raw_png = base64.b64decode(item.get("data", ""))
            with open(args.out, "wb") as f:
                f.write(raw_png)

    if is_error:
        sys.exit(1)


if __name__ == "__main__":
    main()
