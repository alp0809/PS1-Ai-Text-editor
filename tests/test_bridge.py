#!/usr/bin/env python3
"""
test_bridge.py - runs pc/ai_bridge.py (echo backend) against the Pico's AI
link code (ai_link_test, built from pico/ai.c) over a pseudo-terminal.

pyserial is replaced by a small stand-in that reads and writes the pty, so
this needs nothing beyond Python 3 and a C compiler.
"""
import os
import select
import subprocess
import sys
import threading
import tty
import types

HERE = os.path.dirname(os.path.abspath(__file__))


class FakeSerial:
    """just enough of serial.Serial for ai_bridge, on a file descriptor"""
    def __init__(self, fd, timeout):
        self.fd, self.timeout, self.dtr = fd, timeout, True

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass

    def write(self, data):
        os.write(self.fd, data)

    def flush(self):
        pass

    def read(self, n=1):
        r, _, _ = select.select([self.fd], [], [], self.timeout)
        return os.read(self.fd, n) if r else b""


def test_http_backends(ai_bridge):
    """the Anthropic and OpenAI-style streaming parsers, against a local server"""
    import http.server
    import json
    seen = {}

    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["content-length"])))
            seen[self.path] = (body, self.headers)
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.end_headers()
            if self.path.endswith("/messages"):
                evs = [{"type": "message_start"},
                       {"type": "content_block_delta", "delta": {"type": "text_delta", "text": "Hel"}},
                       {"type": "content_block_delta", "delta": {"type": "text_delta", "text": "lo"}},
                       {"type": "message_stop"}]
                for e in evs:
                    self.wfile.write(b"event: x\ndata: " + json.dumps(e).encode() + b"\n\n")
            else:
                for t in ["Hi", " there"]:
                    e = {"choices": [{"delta": {"content": t}}]}
                    self.wfile.write(b"data: " + json.dumps(e).encode() + b"\n\n")
                self.wfile.write(b"data: [DONE]\n\n")

    srv = http.server.HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_port}"
    msgs = [{"role": "user", "content": "q"}]
    a = types.SimpleNamespace(api_key="k", model=None, max_tokens=50, url=base + "/v1/messages")
    out = "".join(ai_bridge.ask_anthropic(a, "SYS", msgs, lambda: False))
    body, hdr = seen["/v1/messages"]
    assert out == "Hello", out
    assert body["system"] == "SYS" and body["stream"] and hdr.get("x-api-key") == "k", body
    a.url = base + "/v1"
    out = "".join(ai_bridge.ask_openai(a, "SYS", msgs, lambda: False))
    body, _ = seen["/v1/chat/completions"]
    assert out == "Hi there", out
    assert body["messages"][0] == {"role": "system", "content": "SYS"}, body
    srv.shutdown()
    print("HTTP backends ok")


def test_claude_code_backend(ai_bridge, tmp):
    """the claude-code backend, with a stand-in 'claude' program"""
    import stat
    fake = os.path.join(tmp, "claude")
    with open(fake, "w") as f:
        f.write("""#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
data = sys.stdin.read()
ok = ("-p" in args and "--strict-mcp-config" in args and args[args.index("--tools") + 1] == ""
      and "<document>" in data and "Request:\\nq2" in data and "User: q1" in data
      and "<document>" not in args[args.index("--system-prompt") + 1]
      and "ANTHROPIC_API_KEY" not in os.environ)
for t in (["Hel", "lo"] if ok else ["BAD"]):
    ev = {"type": "content_block_delta", "delta": {"type": "text_delta", "text": t}}
    print(json.dumps({"type": "stream_event", "event": ev}), flush=True)
print(json.dumps({"type": "result", "is_error": False, "result": "Hello"}))
""")
    os.chmod(fake, os.stat(fake).st_mode | stat.S_IEXEC)
    os.environ["ANTHROPIC_API_KEY"] = "must-not-be-passed"
    a = types.SimpleNamespace(claude_bin=fake, model=None, keep_api_key=False, max_tokens=50)
    msgs = [{"role": "user", "content": "q1"}, {"role": "assistant", "content": "a1"},
            {"role": "user", "content": "q2"}]
    out = "".join(ai_bridge.ask_claude_code(a, "INSTR\n<document>\nDOC\n</document>", msgs,
                                            lambda: False))
    del os.environ["ANTHROPIC_API_KEY"]
    assert out == "Hello", out
    print("claude-code backend ok")


def main():
    master, slave = os.openpty()
    tty.setraw(master)
    tty.setraw(slave)

    # a pretend SD card with one document
    sd = os.path.join(HERE, "sd_bridge")
    os.makedirs(os.path.join(sd, "ps1writer"), exist_ok=True)
    with open(os.path.join(sd, "ps1writer", "doc.md"), "w", encoding="utf-8") as f:
        f.write("# Title\nSecond paragraph text\nThird\n")

    # stand-in pyserial, then the bridge
    fake = types.ModuleType("serial")
    fake.SerialException = OSError
    fake.Serial = lambda port, baud, timeout: FakeSerial(slave, timeout)
    tools = types.ModuleType("serial.tools")
    lp = types.ModuleType("serial.tools.list_ports")
    lp.comports = lambda: []
    fake.tools, tools.list_ports = tools, lp
    sys.modules.update({"serial": fake, "serial.tools": tools, "serial.tools.list_ports": lp})
    sys.path.insert(0, os.path.join(HERE, "..", "pc"))
    import ai_bridge

    test_http_backends(ai_bridge)
    test_claude_code_backend(ai_bridge, sd)

    args = types.SimpleNamespace(backend="echo", model=None, url=None, api_key=None,
                                 max_tokens=100, max_doc_chars=200000)
    threading.Thread(target=ai_bridge.serve, args=(args, "pty"), daemon=True).start()

    p = subprocess.run([os.path.join(HERE, "ai_link_test"), str(master), sd],
                       pass_fds=(master,), capture_output=True, text=True,
                       encoding="latin-1", timeout=60)
    print(p.stdout)
    if p.returncode != 0 or "OK" not in p.stdout:
        print("bridge test FAILED")
        sys.exit(1)
    print("bridge test passed")


if __name__ == "__main__":
    main()
