"""Run a Python file inside the open Unreal Editor via the MCPython TCP bridge (127.0.0.1:12029), used while the unreal MCP is not registered for this folder. Usage: python ue_bridge.py code.py
Prints the editor-side output (unwrapped), capped at 8000 chars."""
import json, socket, sys

code = open(sys.argv[1], encoding='utf-8').read()
with socket.create_connection(('127.0.0.1', 12029), timeout=600) as s:
    s.sendall(json.dumps({'type': 'python', 'code': code}).encode('utf-8'))
    buf = b''
    while chunk := s.recv(65536):
        buf += chunk
r = json.loads(buf.decode('utf-8', 'replace'))
try:
    inner = json.loads(r['result'])
    out = inner.get('raw_result', inner)
except Exception:
    out = r
print(str(out).replace('\r\n\n', '\n')[:8000])
