"""Read effective tool configuration only; never start a thread or model turn."""
import json
import os
import re
from pathlib import Path
import subprocess
import sys

workspace_root = os.environ.get("LOCALCUT_WORKSPACE_ROOT")
if not workspace_root:
    raise SystemExit("LOCALCUT_WORKSPACE_ROOT must be set to an existing absolute directory")
workspace_root = Path(workspace_root)
if not workspace_root.is_absolute() or not workspace_root.is_dir():
    raise SystemExit("LOCALCUT_WORKSPACE_ROOT must be an existing absolute directory")
root = workspace_root / "codex-probe"
for directory in ("logs", "state", "tmp"):
    (root / directory).mkdir(parents=True, exist_ok=True)
environment = dict(os.environ, TEMP=str(root / "tmp"), TMP=str(root / "tmp"), PYTHONDONTWRITEBYTECODE="1")
disabled_features = ("shell_tool", "plugins", "apps", "remote_plugin", "hooks", "multi_agent",
                     "computer_use", "browser_use", "in_app_browser", "image_generation",
                     "view_image", "code_mode")
args = [sys.argv[1], "app-server", "--stdio", "-c", "mcp_servers={}", "-c", 'web_search="disabled"',
        "-c", "features.code_mode_host=true",
        "-c", "log_dir=" + json.dumps((root / "logs").as_posix()),
        "-c", "sqlite_home=" + json.dumps((root / "state").as_posix())]
for feature in disabled_features:
    args.extend(["-c", "features." + feature + "=false"])
messages = [
    {"id": 1, "method": "initialize", "params": {
        "clientInfo": {"name": "kdenlive-isolation-probe", "version": "0.1"},
        "capabilities": {"experimentalApi": True}}},
    {"method": "initialized"},
    {"id": 2, "method": "config/read", "params": {"includeLayers": False}},
]
for attempt in range(2):
    peer = subprocess.Popen(args, cwd=root, env=environment, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    def rpc(message):
        peer.stdin.write((json.dumps(message) + "\n").encode())
        peer.stdin.flush()
        if "id" in message:
            for line in peer.stdout:
                response = json.loads(line)
                if response.get("id") == message["id"]:
                    return response
    try:
        for message in messages:
            response = rpc(message)
        config = response.get("result", {}).get("config", {})
        servers = config.get("mcp_servers", {})
        if attempt == 0:
            for name in servers:
                assert re.fullmatch(r"[A-Za-z0-9_-]+", name), "MCP name requires supported escaping"
                args.extend(["-c", "mcp_servers." + name + ".enabled=false"])
            continue
        inventory = rpc({"id": 3, "method": "mcpServerStatus/list", "params": {}})
        entries = inventory.get("result", {}).get("data", [])
        print(json.dumps({"rpcError": "error" in response,
            "features": {name: config.get("features", {}).get(name)
                         for name in (*disabled_features, "code_mode_host")},
            "mcpCount": len(servers), "mcpEnabled": [entry.get("enabled") for entry in servers.values()],
            "web_search": config.get("web_search"), "inventoryError": "error" in inventory,
            "inventoryServerCount": len(entries),
            "inventoryToolCount": sum(len(entry.get("tools", {})) for entry in entries),
            "inventoryNextCursor": inventory.get("result", {}).get("nextCursor")}, ensure_ascii=False))
    except OSError:
        detail = peer.stderr.read(8192).decode("utf-8", "replace")
        print(json.dumps({"attempt": attempt, "exit": peer.poll(),
            "diagnosticMarkers": [marker for marker in ("config", "TOML", "already", "error", "key", "unexpected", "parse", "duplicate", "lock", "failed") if marker.lower() in detail.lower()]}))
        raise
    finally:
        peer.kill()
        peer.wait()
