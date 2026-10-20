#!/usr/bin/env python3
"""
ai_bridge.py - connects ps1-writer to an AI model.

The Pico shows up on the PC as two USB serial ports: "ps1-writer log" and
"ps1-writer AI". This program opens the AI port, waits for questions typed
in the PlayStation's chat panel (F2), sends each question to a language
model together with the current document, and streams the answer back.

    pip install pyserial
    python3 ai_bridge.py --backend anthropic        # Claude (ANTHROPIC_API_KEY)
    python3 ai_bridge.py --backend openai --url http://localhost:11434/v1 --model llama3.1
                                                     # any OpenAI-compatible server
                                                     # (Ollama, LM Studio, OpenAI...)
    python3 ai_bridge.py --backend claude-code      # Claude through Claude Code, on a
                                                     # Claude subscription (Pro / Max)
    python3 ai_bridge.py --backend echo             # no model: tests the link

Protocol on the serial port (see pico/ai.h):
    Pico -> PC:  ASK <paragraph> <offset> <doc bytes> <question bytes>\\n
                 then the document and the question (UTF-8)
                 CANCEL\\n
    PC -> Pico:  HELLO ...\\n,  TXT <n>\\n + n bytes,  END\\n,  ERR <n>\\n + n bytes
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is missing: pip install pyserial")

USB_VID, USB_PID = 0xCAFE, 0x4021
CURSOR = "<<CURSOR>>"
HISTORY_MESSAGES = 20           # earlier questions and answers kept for context

SYSTEM_PROMPT = """You are a writing assistant built into ps1-writer, a plain-text \
editor running on a PlayStation. The user is writing a long document, for \
example a master's thesis.

The current document is given below between <document> tags. The marker \
{cursor} shows where the user's cursor is.

The editor shows plain text only: 80 columns, Markdown-style headings with #, \
no other formatting. It can only display Latin-1 characters, so avoid emoji, \
typographic quotes and other symbols outside Latin-1.

When the user asks you to write, continue or rewrite text, reply with only \
that text, ready to be inserted at the cursor (the user presses F4 to insert \
your answer, or F5 to replace the whole document with it). Do not add an \
introduction or closing remarks. For other questions, answer briefly; the \
chat panel is small.

<document>
{document}
</document>"""


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------- the port

def find_port():
    ports = [p for p in serial.tools.list_ports.comports()
             if p.vid == USB_VID and p.pid == USB_PID]
    if not ports:
        return None
    for p in ports:                          # Linux / macOS name the interface
        if p.interface and "AI" in p.interface:
            return p.device
    for p in ports:                          # Windows: interface number in location
        if p.location and p.location.endswith(".2"):
            return p.device
    ports.sort(key=lambda p: p.device)
    return ports[-1].device if len(ports) > 1 else None


class Link:
    def __init__(self, ser):
        self.ser = ser
        self.lock = threading.Lock()

    def send(self, data):
        with self.lock:
            self.ser.write(data)
            self.ser.flush()

    def send_text(self, kind, text):
        b = text.encode("utf-8")
        self.send(b"%s %d\n" % (kind, len(b)) + b)

    def read_line(self):
        line = b""
        while not line.endswith(b"\n"):
            c = self.ser.read(1)
            if not c:
                return None if not line else line.decode("ascii", "replace").strip()
            line += c
        return line.decode("ascii", "replace").strip()

    def read_exact(self, n, timeout=30):
        data = bytearray()
        end = time.time() + timeout
        while len(data) < n:
            chunk = self.ser.read(n - len(data))
            if chunk:
                data += chunk
                end = time.time() + timeout
            elif time.time() > end:
                raise IOError("timeout reading from the Pico")
        return bytes(data)


# ---------------------------------------------------------------- models

def http_stream(url, headers, body):
    """POST json, yield the 'data:' lines of a server-sent event stream."""
    req = urllib.request.Request(url, data=json.dumps(body).encode("utf-8"),
                                 headers={"content-type": "application/json", **headers})
    try:
        resp = urllib.request.urlopen(req, timeout=300)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        raise RuntimeError(f"HTTP {e.code}: {detail}")
    with resp:
        for raw in resp:
            line = raw.decode("utf-8", "replace").strip()
            if line.startswith("data:"):
                yield line[5:].strip()


def ask_anthropic(args, system, messages, cancelled):
    key = args.api_key or os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        raise RuntimeError("no API key: set ANTHROPIC_API_KEY or use --api-key")
    body = {"model": args.model or "claude-sonnet-5-5", "max_tokens": args.max_tokens,
            "system": system, "messages": messages, "stream": True}
    headers = {"x-api-key": key, "anthropic-version": "2023-06-01"}
    for data in http_stream(args.url or "https://api.anthropic.com/v1/messages", headers, body):
        if cancelled():
            return
        ev = json.loads(data)
        if ev.get("type") == "content_block_delta" and ev["delta"].get("type") == "text_delta":
            yield ev["delta"]["text"]
        elif ev.get("type") == "error":
            raise RuntimeError(ev.get("error", {}).get("message", "API error"))


def ask_openai(args, system, messages, cancelled):
    key = args.api_key or os.environ.get("OPENAI_API_KEY", "")
    base = (args.url or "http://localhost:11434/v1").rstrip("/")
    body = {"model": args.model or "llama3.1", "max_tokens": args.max_tokens, "stream": True,
            "messages": [{"role": "system", "content": system}] + messages}
    headers = {"authorization": f"Bearer {key}"} if key else {}
    for data in http_stream(base + "/chat/completions", headers, body):
        if cancelled():
            return
        if data == "[DONE]":
            return
        ev = json.loads(data)
        for ch in ev.get("choices", []):
            text = (ch.get("delta") or {}).get("content")
            if text:
                yield text


def ask_claude_code(args, system, messages, cancelled):
    """Claude through the Claude Code command line ('claude -p'), which uses
    the account it is logged in with (a Pro / Max subscription counts against
    the plan's usage limits). No tools, no MCP servers: it only writes text."""
    exe = args.claude_bin or shutil.which("claude")
    if not exe:
        raise RuntimeError("Claude Code ('claude') not found: install it and log in once")

    # instructions as the system prompt; the (possibly long) document and the
    # conversation on stdin, because command lines have a length limit
    instructions, _, document = system.partition("<document>")
    parts = ["<document>" + document]
    if len(messages) > 1:
        parts.append("Earlier in this conversation:\n" + "\n\n".join(
            ("User: " if m["role"] == "user" else "Assistant: ") + m["content"]
            for m in messages[:-1]))
    parts.append("Request:\n" + messages[-1]["content"])
    stdin_text = "\n\n".join(parts)

    cmd = [exe, "-p", "--output-format", "stream-json", "--verbose",
           "--include-partial-messages", "--tools", "", "--strict-mcp-config",
           "--no-session-persistence", "--system-prompt", instructions.strip()]
    if args.model:
        cmd += ["--model", args.model]
    env = dict(os.environ)
    if not args.keep_api_key:
        env.pop("ANTHROPIC_API_KEY", None)   # else Claude Code bills that key instead

    with tempfile.TemporaryDirectory() as empty:       # no project files around
        p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, cwd=empty, env=env,
                             text=True, encoding="utf-8", errors="replace")
        threading.Thread(target=lambda: (p.stdin.write(stdin_text), p.stdin.close()),
                         daemon=True).start()
        streamed, final, failed = False, None, None
        try:
            for line in p.stdout:
                if cancelled():
                    return
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                if ev.get("type") == "stream_event":
                    e = ev.get("event", {})
                    d = e.get("delta", {})
                    if e.get("type") == "content_block_delta" and d.get("type") == "text_delta":
                        streamed = True
                        yield d["text"]
                elif "result" in ev and "is_error" in ev:
                    if ev["is_error"]:
                        failed = str(ev.get("result") or "Claude Code reported an error")
                    else:
                        final = ev.get("result")
            p.wait()
            if failed:
                raise RuntimeError(failed[:180])
            if not streamed and final:
                yield final
            elif not streamed:
                err = p.stderr.read().strip().splitlines()
                raise RuntimeError("Claude Code: " + (err[-1] if err else f"exit code {p.returncode}"))
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()


def ask_echo(args, system, messages, cancelled):
    doc = system.split("<document>\n", 1)[1].rsplit("\n</document>", 1)[0]
    words = len(doc.replace(CURSOR, " ").split())
    around = doc.split(CURSOR)[0][-60:].replace("\n", " ")
    reply = (f"You asked: {messages[-1]['content']}\n"
             f"The document has {words} words. Text before the cursor: ...{around}")
    for word in reply.split(" "):
        if cancelled():
            return
        time.sleep(0.05)
        yield word + " "


BACKENDS = {"anthropic": ask_anthropic, "openai": ask_openai,
            "claude-code": ask_claude_code, "echo": ask_echo}


# ---------------------------------------------------------------- questions

def with_cursor(doc, para, off, limit):
    lines = doc.split("\n")
    para = min(para, len(lines) - 1)
    off = min(off, len(lines[para]))
    lines[para] = lines[para][:off] + CURSOR + lines[para][off:]
    text = "\n".join(lines)
    if len(text) > limit:                    # too long: the part around the cursor
        at = text.index(CURSOR)
        start = max(0, min(at - limit // 2, len(text) - limit))
        text = "[...]\n" + text[start:start + limit] + "\n[...]"
    return text


class Bridge:
    def __init__(self, args):
        self.args = args
        self.history = []
        self.worker = None
        self.cancel_flag = threading.Event()

    def cancel(self):
        if self.worker and self.worker.is_alive():
            self.cancel_flag.set()
            self.worker.join(timeout=5)

    def handle_ask(self, link, para, off, doc, question):
        self.cancel()
        q = question.strip()
        if q.lower() in ("/new", "/reset", "/clear"):
            self.history = []
            link.send_text(b"TXT", "Started a new conversation.")
            link.send(b"END\n")
            log("history cleared")
            return
        log(f"question ({len(doc)} chars of document, cursor {para}:{off}): {q[:70]}")
        system = SYSTEM_PROMPT.format(cursor=CURSOR,
                                      document=with_cursor(doc, para, off, self.args.max_doc_chars))
        messages = self.history[-HISTORY_MESSAGES:] + [{"role": "user", "content": q}]
        self.cancel_flag = threading.Event()
        flag = self.cancel_flag
        self.worker = threading.Thread(target=self.answer, daemon=True,
                                       args=(link, system, messages, q, flag))
        self.worker.start()

    def answer(self, link, system, messages, q, flag):
        out, pending, last = [], "", time.time()
        try:
            for piece in BACKENDS[self.args.backend](self.args, system, messages, flag.is_set):
                if flag.is_set():
                    return
                out.append(piece)
                pending += piece
                if len(pending) > 200 or time.time() - last > 0.3:
                    link.send_text(b"TXT", pending)
                    pending, last = "", time.time()
            if flag.is_set():
                return
            if pending:
                link.send_text(b"TXT", pending)
            link.send(b"END\n")
            text = "".join(out)
            self.history += [{"role": "user", "content": q}, {"role": "assistant", "content": text}]
            log(f"answered ({len(text)} chars)")
        except Exception as e:                    # report any failure to the PS1
            log("error:", e)
            if not flag.is_set():
                try:
                    link.send_text(b"ERR", str(e)[:180])
                except Exception:
                    pass


def serve(args, port):
    log(f"opening {port}")
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        ser.dtr = True
        link = Link(ser)
        bridge = Bridge(args)
        link.send(b"HELLO ps1-writer ai_bridge 1\n")
        last_hello = time.time()
        log(f"ready - backend: {args.backend}. Press F2 on the PlayStation to chat.")
        while True:
            if time.time() - last_hello > 5:
                link.send(b"HELLO ps1-writer ai_bridge 1\n")
                last_hello = time.time()
            line = link.read_line()
            if not line:
                continue
            parts = line.split()
            if parts[0] == "ASK" and len(parts) == 5:
                para, off, dlen, qlen = (int(x) for x in parts[1:])
                doc = link.read_exact(dlen).decode("utf-8", "replace")
                question = link.read_exact(qlen).decode("utf-8", "replace")
                bridge.handle_ask(link, para, off, doc, question)
            elif parts[0] == "CANCEL":
                log("cancelled by the user")
                bridge.cancel()


def main():
    ap = argparse.ArgumentParser(description="AI bridge for ps1-writer")
    ap.add_argument("--port", help="serial port (default: find the 'ps1-writer AI' port)")
    ap.add_argument("--backend", choices=BACKENDS, default="anthropic")
    ap.add_argument("--model", help="model name (anthropic: claude-sonnet-5-5, openai: llama3.1, "
                                    "claude-code: Claude Code's own default)")
    ap.add_argument("--url", help="API address (openai default: http://localhost:11434/v1)")
    ap.add_argument("--api-key", help="API key (default: ANTHROPIC_API_KEY / OPENAI_API_KEY)")
    ap.add_argument("--max-tokens", type=int, default=2000, help="longest answer, in tokens")
    ap.add_argument("--claude-bin", help="claude-code: path of the 'claude' program")
    ap.add_argument("--keep-api-key", action="store_true",
                    help="claude-code: let Claude Code use ANTHROPIC_API_KEY (API billing) "
                         "instead of the logged-in account")
    ap.add_argument("--max-doc-chars", type=int, default=200000,
                    help="send at most this much of the document (around the cursor)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    args = ap.parse_args()

    if args.list:
        for p in serial.tools.list_ports.comports():
            vp = f"{p.vid:04X}:{p.pid:04X}" if p.vid else "-"
            print(f"{p.device:20} {vp:10} {p.interface or ''} {p.location or ''}")
        return

    while True:
        port = args.port or find_port()
        if not port:
            log("waiting for the Pico ('ps1-writer AI' serial port)...")
            time.sleep(3)
            continue
        try:
            serve(args, port)
        except (serial.SerialException, OSError) as e:
            log(f"connection lost ({e}); retrying")
            time.sleep(2)
        except KeyboardInterrupt:
            return


if __name__ == "__main__":
    main()
