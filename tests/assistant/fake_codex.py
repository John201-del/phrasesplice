"""Offline app-server peer. Every test talks to this through QProcess stdio."""
import json
import os
import sys
import time

scenario = sys.argv[1]
initialized = False
model_listed = False
turn = "turn-1"
disabled_features = ("shell_tool", "plugins", "apps", "remote_plugin", "hooks", "multi_agent",
                     "computer_use", "browser_use", "in_app_browser", "image_generation",
                     "view_image", "code_mode")


def send(value, fragmented=False):
    payload = (json.dumps(value, ensure_ascii=False) + "\n").encode("utf-8")
    if fragmented:
        # Split inside a Chinese UTF-8 sequence, as well as across JSON tokens.
        offset = payload.find("你".encode("utf-8")) + 1
        if offset <= 0:
            offset = 7
        sys.stdout.buffer.write(payload[:offset])
        sys.stdout.buffer.flush()
        time.sleep(0.025)
        payload = payload[offset:]
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


def notify(method, params):
    send({"method": method, "params": params})


def complete(status="completed"):
    notify("turn/completed", {"threadId": "thread-1", "turn": {
        "id": turn, "items": [], "status": status}})


for line in sys.stdin.buffer:
    message = json.loads(line)
    method = message.get("method")
    params = message.get("params", {})
    request_id = message.get("id")
    try:
        if method == "initialize":
            for feature in disabled_features:
                assert "features." + feature + "=false" in sys.argv
            assert "features.code_mode_host=true" in sys.argv
            assert params["clientInfo"]["name"]
            assert params["capabilities"]["experimentalApi"] is True
            assert os.environ["TEMP"].replace("\\", "/").startswith(os.getcwd().replace("\\", "/"))
            send({"id": request_id, "result": {"userAgent": "offline-peer"}}, True)
        elif method == "initialized":
            initialized = True
        elif method == "config/read":
            assert initialized
            config = {"features": {name: False for name in disabled_features},
                      "mcp_servers": {}, "web_search": "disabled"}
            config["features"]["code_mode_host"] = True
            if scenario.startswith("catalog-"):
                config["model"] = "retired-global-model"
            for name in ("log_dir", "sqlite_home"):
                override = next(arg for arg in sys.argv if arg.startswith(name + "="))
                config[name] = json.loads(override.split("=", 1)[1])
            if scenario == "unsafe":
                config["mcp_servers"] = {"inherited": {"enabled": True}}
            if scenario == "inherited":
                config["mcp_servers"] = {"inherited": {
                    "enabled": "mcp_servers.inherited.enabled=false" not in sys.argv}}
            send({"id": request_id, "result": {"config": config, "origins": {}}})
        elif method == "mcpServerStatus/list":
            send({"id": request_id, "result": {"data": [], "nextCursor": None}})
        elif method == "model/list":
            assert scenario.startswith("catalog-")
            assert params.get("includeHidden") is False
            model_listed = True
            default = {"model": "catalog-default", "isDefault": True, "hidden": False}
            other = {"model": "other-model", "isDefault": False, "hidden": False}
            data, cursor = [default], None
            if scenario == "catalog-empty":
                data = []
            elif scenario == "catalog-ambiguous":
                data = [default, {"model": "second-default", "isDefault": True, "hidden": False}]
            elif scenario == "catalog-loop":
                data, cursor = [other], "repeat"
            elif scenario == "catalog-pages" and not params.get("cursor"):
                data, cursor = [other], "second-page"
            elif scenario == "catalog-pages":
                assert params["cursor"] == "second-page"
            send({"id": request_id, "result": {"data": data, "nextCursor": cursor}})
        elif method == "thread/start":
            assert initialized
            assert params["cwd"]
            assert params["developerInstructions"] == "仅允许剪辑工具"
            if scenario.startswith("catalog-"):
                assert model_listed
                assert params["model"] == "catalog-default"
            else:
                assert params["model"] == "offline-model"
            assert params["dynamicTools"][0]["name"] == "timeline.read"
            assert params["environments"] == []
            assert params["sandbox"] == "read-only"
            assert params["ephemeral"] is True
            send({"id": request_id, "result": {"thread": {"id": "thread-1"}}})
        elif method == "turn/start":
            assert params["threadId"] == "thread-1"
            assert params["input"][0]["type"] == "text"
            assert params["input"][0]["text"] == "剪辑请求"
            if scenario == "exit":
                sys.stderr.write("Bearer private-token prompt=个人提示词\n")
                sys.stderr.flush()
                sys.exit(7)
            if scenario == "error":
                send({"id": request_id, "error": {"code": -32000,
                    "message": "secret personal prompt must not escape"}})
                continue
            if scenario != "cancel-pending":
                send({"id": request_id, "result": {"turn": {
                    "id": turn, "items": [], "status": "inProgress"}}})
            if scenario.startswith("turn-error-"):
                infos = {
                    "model": "modelNotFound", "quota": "usageLimitExceeded",
                    "auth": {"httpConnectionFailed": {"httpStatusCode": 401}},
                    "http-quota": {"httpConnectionFailed": {"httpStatusCode": 429}},
                    "network": "streamDisconnected", "context": "contextWindowExceeded",
                    "overload": "serverOverloaded", "unknown": "unexpectedServerKind"}
                notify("turn/completed", {"threadId": "thread-1", "turn": {
                    "id": turn, "items": [], "status": "failed", "error": {
                        "codexErrorInfo": infos[scenario.removeprefix("turn-error-")],
                        "message": "Bearer private-token personal prompt must not escape"}}})
            elif scenario == "malformed":
                sys.stdout.buffer.write(b"not-json\n")
                sys.stdout.buffer.flush()
            elif scenario in ("cancel", "cancel-pending", "cancel-no-completion", "hold"):
                if scenario == "cancel-pending":
                    time.sleep(0.1)
                    send({"id": request_id, "result": {"turn": {
                        "id": turn, "items": [], "status": "inProgress"}}})
            elif scenario == "unknown":
                send({"id": "unknown-request", "method": "unexpected/request", "params": {}})
            elif scenario in ("tool", "tool-image"):
                send({"id": "tool-request", "method": "item/tool/call", "params": {
                    "threadId": "thread-1", "turnId": turn, "callId": "call-1",
                    "tool": "timeline.read", "arguments": {"中文": "值"}}}, True)
            else:
                send({"method": "item/agentMessage/delta", "params": {
                    "threadId": "thread-1", "turnId": turn, "itemId": "item-1",
                    "delta": "你好，剪辑"}}, True)
                complete()
                complete()  # Completion must be emitted once.
        elif method == "turn/interrupt":
            assert params == {"threadId": "thread-1", "turnId": turn}
            send({"id": request_id, "result": {}})
            if scenario != "cancel-no-completion":
                complete("interrupted")
        elif method is None and request_id == "tool-request":
            result = message["result"]
            assert result["success"] is True
            if scenario == "tool-image":
                assert result["contentItems"] == [{"type": "inputImage", "imageUrl": "data:image/png;base64,AA=="}]
            else:
                assert result["contentItems"][0]["type"] == "inputText"
                assert json.loads(result["contentItems"][0]["text"]) == {"ok": True}
            complete()
        elif method is None and request_id == "unknown-request":
            assert message["error"]["code"] == -32601
            complete("failed")
    except (AssertionError, KeyError) as error:
        send({"id": request_id, "error": {"code": -32602, "message": "peer assertion"}})
