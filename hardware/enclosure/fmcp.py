#!/usr/bin/env python3
"""Client for the MCP server built into Autodesk Fusion (Streamable HTTP on 127.0.0.1:27182/mcp).

Fusion serves it while it runs; it is not registered as a Claude Code connector, so this
script talks to it directly.

  fmcp.py --script build_enclosure.py      run a Fusion Python script (modifies the design)
  fmcp.py --script inspect.py --ro         run it read-only (allowed while a dialog is open)
  fmcp.py --shot out.png iso-top-right     screenshot of the active viewport
  fmcp.py <tool> '<json args>'             any tool call
"""
import base64, json, os, sys, urllib.error, urllib.request

URL = os.environ.get("FUSION_MCP_URL", "http://127.0.0.1:27182/mcp")
SESS = os.path.join(os.path.expanduser("~"), ".cache", "fusion_mcp_session")


def _post(body, sid=None):
    h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream",
         "MCP-Protocol-Version": "2025-06-18"}
    if sid:
        h["MCP-Session-Id"] = sid
    r = urllib.request.urlopen(urllib.request.Request(URL, json.dumps(body).encode(), h), timeout=900)
    return r.headers.get("MCP-Session-Id"), r.read().decode("utf-8", "replace")


def _session(fresh=False):
    if not fresh and os.path.exists(SESS):
        return open(SESS).read().strip()
    sid, _ = _post({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "smartmeteo-fmcp", "version": "1"}}})
    _post({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    os.makedirs(os.path.dirname(SESS), exist_ok=True)
    open(SESS, "w").write(sid)
    return sid


def call(method, params):
    body = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    try:
        _, raw = _post(body, _session())
    except urllib.error.HTTPError as e:      # stale session after a Fusion restart
        if e.code not in (400, 404):
            raise
        _, raw = _post(body, _session(fresh=True))
    if raw.startswith(("event:", "data:")):
        raw = "\n".join(l[5:] for l in raw.splitlines() if l.startswith("data:"))
    return json.loads(raw)


def tool(name, args, img_out="shot.png"):
    res = call("tools/call", {"name": name, "arguments": args})
    if "result" not in res:
        print(json.dumps(res, indent=1)); return False
    for c in res["result"].get("content", []):
        if c.get("type") == "image":
            open(img_out, "wb").write(base64.b64decode(c["data"])); print("image ->", img_out)
        elif c.get("type") == "text":
            t = c["text"]
            try:                              # script results come back as JSON text
                d = json.loads(t)
                t = d.get("message") or d.get("error") or t
            except ValueError:
                pass
            print(t)
    return not res["result"].get("isError")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "--script":
        ok = tool("fusion_mcp_execute", {"featureType": "script", "object": {
            "script": open(a[1]).read(), "readOnly": "--ro" in a}})
    elif a[0] == "--shot":
        ok = tool("fusion_mcp_read", {"queryType": "screenshot", "direction": a[2] if len(a) > 2 else "current",
                                      "width": 1600, "height": 1100, "transparentBackground": False}, a[1])
    else:
        ok = tool(a[0], json.loads(a[1]) if len(a) > 1 else {})
    sys.exit(0 if ok else 2)
