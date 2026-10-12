"""The MCP stdio loop tools/gui_mcp.py and tools/monitor_mcp.py share.

MCP over stdio is newline-delimited JSON-RPC 2.0; this is the whole of the
protocol either server needs. Standard library only, like the servers.
"""

import json
import sys

PROTOCOL = "2025-06-18"


def handle(msg, server_name, tools, run_tool):
    """The response to one request, or None for a method this does not know."""
    method = msg.get("method")
    if method == "initialize":
        want = (msg.get("params") or {}).get("protocolVersion") or PROTOCOL
        return {"protocolVersion": want,
                "capabilities": {"tools": {}},
                "serverInfo": {"name": server_name, "version": "1"}}
    if method == "tools/list":
        return {"tools": tools}
    if method == "tools/call":
        params = msg.get("params") or {}
        try:
            content, is_error = run_tool(params.get("name"),
                                         params.get("arguments") or {})
        except SystemExit as e:
            content, is_error = [{"type": "text", "text": str(e)}], True
        except Exception as e:                       # noqa: BLE001
            content, is_error = [{"type": "text",
                                  "text": "%s: %s" % (type(e).__name__, e)}], True
        return {"content": content, "isError": is_error}
    if method == "ping":
        return {}
    return None


def serve(server_name, tools, run_tool):
    """Answer requests on stdin until it closes. `run_tool(name, args)`
    returns (content list, is_error)."""
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            continue
        if "id" not in msg:
            continue                                  # a notification
        try:
            result = handle(msg, server_name, tools, run_tool)
        except Exception as e:                        # noqa: BLE001
            reply = {"jsonrpc": "2.0", "id": msg["id"],
                     "error": {"code": -32603, "message": str(e)}}
        else:
            if result is None:
                reply = {"jsonrpc": "2.0", "id": msg["id"],
                         "error": {"code": -32601,
                                   "message": "method not found: %s"
                                              % msg.get("method")}}
            else:
                reply = {"jsonrpc": "2.0", "id": msg["id"], "result": result}
        out.write(json.dumps(reply) + "\n")
        out.flush()
