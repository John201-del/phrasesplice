# SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
"""Check edit argument rejection against an open editor's local MCP bridge.

Usage: python edit_contract_mcp.py MCP_CONFIG REPORT_JSON
Every request uses an unsupported operation, so no timeline edit can succeed.
The report contains case names and error codes, never project/media contents.
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import uuid


def main():
    config_path, report_path = map(Path, sys.argv[1:])
    config = json.loads(config_path.read_text(encoding="utf-8-sig"))["mcpServers"]["localcut"]
    report_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = report_path.parent / "tmp"
    temporary.mkdir(exist_ok=True)
    env = dict(os.environ, TEMP=str(temporary), TMP=str(temporary),
               PATH=str(Path(config["command"]).parent) + os.pathsep + os.environ.get("PATH", ""))
    process = subprocess.Popen([config["command"], *config["args"]], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               encoding="utf-8", env=env,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    responses = queue.Queue()
    threading.Thread(target=lambda: [responses.put(line) for line in process.stdout], daemon=True).start()
    sequence = 0
    records = []

    def rpc(method, params, notification=False):
        nonlocal sequence
        sequence += 1
        request = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notification:
            request["id"] = sequence
        process.stdin.write(json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n")
        process.stdin.flush()
        if notification:
            return None
        while True:
            reply = json.loads(responses.get(timeout=20))
            if reply.get("id") == sequence:
                if "error" in reply:
                    raise RuntimeError("MCP protocol request failed")
                return reply["result"]

    def tool(name, arguments):
        reply = rpc("tools/call", {"name": name, "arguments": arguments})
        return json.loads(next(item["text"] for item in reply["content"] if item["type"] == "text"))

    def snapshot():
        state = tool("read_project_state", {})
        if not state.get("ok") or not state.get("editable"):
            raise RuntimeError("An editable project must be open")
        return state

    # Malformed fields, distinct from correctly typed but stale identifiers.
    cases = []
    missing = object()
    for key in ("sessionId", "projectId", "sequenceId"):
        for label, value in (("missing", missing), ("null", None), ("empty", ""),
                             ("blank", "  "), ("number", 1), ("boolean", True),
                             ("array", []), ("object", {})):
            cases.append((key + "-" + label, key, value, "invalid_request"))
    for label, value in (("missing", missing), ("null", None), ("string", "2"),
                         ("boolean", True), ("array", []), ("object", {}),
                         ("fraction", 2.5), ("negative", -1), ("overflow", 1e100)):
        cases.append(("expectedRevision-" + label, "expectedRevision", value, "invalid_request"))
    for key in ("sessionId", "projectId", "sequenceId"):
        cases.append((key + "-stale", key, str(uuid.uuid4()), "stale_state"))
    cases.append(("revision-stale", "expectedRevision", -2, "stale_state"))
    cases.append(("valid-context-unsupported-operation", None, None, "unsupported_operation"))
    try:
        rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                           "clientInfo": {"name": "LocalCutEditContractTest", "version": "1"}})
        rpc("notifications/initialized", {}, notification=True)
        for name, key, value, expected in cases:
            before = snapshot()
            arguments = {field: before[field] for field in ("sessionId", "projectId", "sequenceId")}
            arguments.update(expectedRevision=before["revision"], requestId=str(uuid.uuid4()),
                             operations=[{"type": "localcut_contract_unsupported"}])
            if value is missing:
                del arguments[key]
            elif key:
                arguments[key] = before["revision"] + 1 if name == "revision-stale" else value
            result = tool("apply_edit_batch", arguments)
            after = snapshot()
            unchanged = {k: v for k, v in before.items() if k != "revision"} == {
                k: v for k, v in after.items() if k != "revision"}
            revision_unchanged = before["revision"] == after["revision"]
            explained = expected != "invalid_request" or (
                key in result.get("message", "") and "read_project_state" in result.get("message", ""))
            passed = (result.get("ok") is False and result.get("code") == expected and unchanged
                      and explained and (revision_unchanged or expected == "unsupported_operation"))
            records.append({"case": name, "expected": expected, "actual": result.get("code"),
                            "projectUnchanged": unchanged, "revisionUnchanged": revision_unchanged,
                            "explained": explained, "passed": passed})
        passed = sum(record["passed"] for record in records)
        report = {"passed": passed, "failed": len(records) - passed, "skipped": 0, "cases": records}
        report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("passed", "failed", "skipped")}))
        return 0 if passed == len(records) else 1
    finally:
        process.stdin.close()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
