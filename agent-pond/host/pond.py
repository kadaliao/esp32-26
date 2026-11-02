#!/usr/bin/env python3
"""Agent 夜池 —— Mac 端探测器 + 串口推送。

找出这台电脑上正在跑的 coding agent（Claude Code / Codex / 其他 CLI），
判断每个此刻在干什么，每秒一行 "@@{json}" 推给 ESP32。

状态（s 字段）：
  tool   正在调用工具（x 字段是工具名）
  think  模型在想 / 在生成
  wait   一轮结束，等你回话
  sleep  很久没动静了

数据来源：
  Claude Code  ~/.claude/projects/<项目>/<会话>.jsonl  （子 agent 在 <会话>/subagents/）
  Codex        ~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl（CLI 和桌面 App 共用）
  其他 CLI     进程表里挂着终端的 gemini / opencode / aider / grok / cursor-agent
只读这些文件，不改任何东西、不联网。
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import re
import select
import struct
import subprocess
import sys
import termios
import time
import zlib

HOME = os.path.expanduser("~")
PORT_GLOB = "/dev/cu.usbmodem*"

SHOW_WINDOW = 30 * 60      # 会话文件 30 分钟内动过才算在场
LIVE_WINDOW = 12 * 3600    # 有对应进程在跑时，放宽到 12 小时
SLEEP_AFTER = 10 * 60      # 10 分钟没动静就算睡着
MAX_AGENTS = 8
TAIL_BYTES = 256 * 1024

OTHER_CLIS = {"gemini", "opencode", "aider", "grok", "cursor-agent", "crush", "amp"}


# ---------------------------------------------------------------- 工具函数


def short_id(text: str) -> str:
    return hashlib.blake2s(text.encode(), digest_size=4).hexdigest()


def tail_lines(path: str, nbytes: int = TAIL_BYTES) -> list[str]:
    try:
        with open(path, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            fh.seek(max(0, size - nbytes))
            data = fh.read()
    except OSError:
        return []
    lines = data.split(b"\n")
    if size > nbytes:
        lines = lines[1:]  # 第一行多半被截断
    return [ln.decode("utf-8", "replace") for ln in lines if ln.strip()]


def first_line(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.readline(1 << 20).decode("utf-8", "replace")
    except OSError:
        return ""


def pretty_tool(name: str) -> str:
    """mcp__server__tool -> tool；exec_command -> shell。屏幕只有 320 宽，越短越好。"""
    if name.startswith("mcp__"):
        name = name.split("__")[-1]
    return {
        "exec_command": "shell", "shell_command": "shell", "local_shell": "shell",
        "apply_patch": "patch", "write_stdin": "stdin", "update_plan": "plan",
        "TodoWrite": "todo", "WebFetch": "fetch", "WebSearch": "search",
    }.get(name, name)[:14]


# ---------------------------------------------------------------- 进程表


def live_processes() -> dict[str, list[str]]:
    """返回 {种类: [cwd, ...]}。只认挂着终端的交互式进程或 headless 子进程。"""
    out: dict[str, list[str]] = {}
    try:
        ps = subprocess.run(["ps", "-axo", "pid=,tty=,args="], capture_output=True,
                            text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return out
    pids: list[tuple[str, str]] = []
    for line in ps.splitlines():
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        pid, tty, args = parts
        argv0 = os.path.basename(args.split()[0])
        if argv0 not in ("claude", "codex") and argv0 not in OTHER_CLIS:
            continue
        if " app-server" in args or " exec-server" in args or " mcp" in args:
            continue  # 桌面 App 的守护进程，不是一个 agent
        if tty == "??" and " -p" not in args and " exec" not in args:
            continue
        pids.append((pid, argv0))
    if not pids:
        return out
    try:
        lsof = subprocess.run(["lsof", "-a", "-d", "cwd", "-Fpn", "-p",
                               ",".join(p for p, _ in pids)],
                              capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        lsof = ""
    kind_of = dict(pids)
    cur = None
    for line in lsof.splitlines():
        if line.startswith("p"):
            cur = line[1:]
        elif line.startswith("n") and cur in kind_of:
            out.setdefault(kind_of[cur], []).append(line[1:])
    return out


# ---------------------------------------------------------------- Claude Code


def claude_state(path: str) -> tuple[str, str, str]:
    """返回 (cwd, 状态, 工具名)。从文件尾往前找最后一条有意义的消息。"""
    cwd = ""
    state, tool = "think", ""
    decided = False
    for line in reversed(tail_lines(path)):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if not cwd and d.get("cwd"):
            cwd = d["cwd"]
        if decided:
            if cwd:
                break
            continue
        typ = d.get("type")
        if typ not in ("user", "assistant") or d.get("isMeta"):
            continue
        msg = d.get("message") or {}
        content = msg.get("content")
        if typ == "assistant":
            uses = [c for c in content or [] if isinstance(c, dict) and c.get("type") == "tool_use"]
            if uses:
                state, tool = "tool", pretty_tool(uses[-1].get("name", ""))
            elif msg.get("stop_reason") in ("end_turn", "stop_sequence", "max_tokens"):
                state = "wait"
            else:
                state = "think"
        else:
            text = content if isinstance(content, str) else ""
            if isinstance(content, list):
                text = " ".join(c.get("text", "") for c in content
                                if isinstance(c, dict) and c.get("type") == "text")
            state = "wait" if "[Request interrupted" in text else "think"
        decided = True
    return cwd, state, tool


def claude_agents(now: float, live_cwds: list[str]) -> list[dict]:
    agents = []
    for path in glob.glob(f"{HOME}/.claude/projects/*/*.jsonl"):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        age = now - mtime
        if age > LIVE_WINDOW:
            continue
        cwd, state, tool = claude_state(path)
        # 进行中的子 agent：最近 20 秒内写过的 subagents/*.jsonl
        sub_dir = path[:-6] + "/subagents"
        kids = 0
        for sp in glob.glob(sub_dir + "/*.jsonl"):
            try:
                if now - os.path.getmtime(sp) < 20:
                    kids += 1
            except OSError:
                pass
        agents.append(dict(kind="claude", path=path, cwd=cwd,
                           key=os.path.basename(os.path.dirname(path)), state=state, tool=tool,
                           age=age, kids=kids))
    return agents


# ---------------------------------------------------------------- Codex


def codex_state(path: str) -> tuple[str, str]:
    for line in reversed(tail_lines(path, 128 * 1024)):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        p = d.get("payload") or {}
        t = d.get("type")
        pt = p.get("type")
        if t == "event_msg" and pt in ("task_complete", "turn_aborted"):
            return "wait", ""
        if t == "response_item":
            if pt in ("function_call", "custom_tool_call", "local_shell_call"):
                return "tool", pretty_tool(p.get("name") or "shell")
            if pt == "function_call_output":
                return "think", ""
            if pt == "message" and p.get("role") == "assistant":
                return "think", ""
            if pt in ("reasoning", "message"):
                return "think", ""
        if t == "event_msg" and pt in ("user_message", "task_started"):
            return "think", ""
    return "wait", ""


def codex_agents(now: float, live_cwds: list[str]) -> list[dict]:
    agents = []
    for path in glob.glob(f"{HOME}/.codex/sessions/*/*/*/rollout-*.jsonl"):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        age = now - mtime
        if age > LIVE_WINDOW:
            continue
        cwd = ""
        try:
            cwd = (json.loads(first_line(path)).get("payload") or {}).get("cwd", "")
        except ValueError:
            pass
        state, tool = codex_state(path)
        agents.append(dict(kind="codex", path=path, cwd=cwd, key=cwd, state=state, tool=tool,
                           age=age, kids=0))
    return agents


# ---------------------------------------------------------------- 汇总


def claim_alive(agents: list[dict], live_cwds: list[str]) -> list[dict]:
    """一个进程只认领同目录下最新的一个会话；其余老会话按普通超时处理。

    匹配用的是 key：Claude 是会话文件所在的项目目录（= 启动目录编码，cd 之后也不变），
    Codex 是会话记录的 cwd。"""
    budget: dict[str, int] = {}
    for c in live_cwds:
        budget[c] = budget.get(c, 0) + 1
    kept = []
    for a in sorted(agents, key=lambda a: a["age"]):
        a["alive"] = budget.get(a["key"], 0) > 0
        if a["alive"]:
            budget[a["key"]] -= 1
        if a["age"] <= SHOW_WINDOW or a["alive"]:
            kept.append(a)
    return kept


def collect(now: float | None = None) -> list[dict]:
    now = now or time.time()
    procs = live_processes()
    agents = claim_alive(claude_agents(now, []),
                         [re.sub(r"[^A-Za-z0-9]", "-", c) for c in procs.get("claude", [])])
    agents += claim_alive(codex_agents(now, procs.get("codex", [])), procs.get("codex", []))

    for kind in sorted(OTHER_CLIS):
        for cwd in procs.get(kind, []):
            agents.append(dict(kind=kind, path=f"{kind}:{cwd}", cwd=cwd, state="think",
                               tool="", age=0, kids=0, alive=True))

    for a in agents:
        if a["state"] != "wait" and a["age"] > 90:
            a["state"] = "wait"      # 日志停在半路很久了，多半是卡在权限确认或被打断
        if a["age"] > SLEEP_AFTER:
            a["state"] = "sleep"

    rank = {"tool": 0, "think": 0, "wait": 1, "sleep": 2}
    agents.sort(key=lambda a: (rank[a["state"]], a["age"]))
    agents = agents[:MAX_AGENTS]

    names: dict[str, int] = {}
    out = []
    for a in agents:
        name = os.path.basename(a["cwd"].rstrip("/")) or "~"
        if a["cwd"].rstrip("/") == HOME:
            name = "~"
        names[name] = names.get(name, 0) + 1
        if names[name] > 1:
            name = f"{name} {names[name]}"
        out.append({
            "id": short_id(a["path"]),
            "k": a["kind"],
            "n": name[:20],
            "s": a["state"],
            "x": a["tool"],
            "g": int(a["age"]),
            "c": a["kids"],
        })
    return out


def build_frame() -> dict:
    return {"v": 1, "t": time.strftime("%H:%M"), "a": collect()}


# ---------------------------------------------------------------- 串口


class Serial:
    """裸 termios 串口。不碰 DTR/RTS —— ESP32-S3 原生 USB-JTAG 不需要。"""

    def __init__(self, path: str):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = attrs[3] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self._buf = b""

    def write(self, data: bytes, timeout: float = 2.0) -> bool:
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    return False
                select.select([], [self.fd], [], 0.05)
        return True

    def readline(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                return line.decode("utf-8", "replace")
            remain = max(0.0, deadline - time.monotonic())
            r, _, _ = select.select([self.fd], [], [], min(remain, 0.2))
            if not r and remain <= 0:
                return None
            if r:
                try:
                    self._buf += os.read(self.fd, 8192)
                except BlockingIOError:
                    pass

    def drain(self) -> None:
        self._buf = b""
        while True:
            try:
                if not os.read(self.fd, 4096):
                    return
            except OSError:
                return

    def close(self) -> None:
        try:
            os.close(self.fd)
        except OSError:
            pass


def find_port() -> str | None:
    ports = sorted(glob.glob(PORT_GLOB))
    return ports[0] if ports else None


def frame_line(frame: dict) -> bytes:
    return ("@@" + json.dumps(frame, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


# ---------------------------------------------------------------- 截图


def rle_decode(data: bytes, npix: int) -> bytes:
    out = bytearray(npix * 2)
    i = o = 0
    while i < len(data):
        tag = data[i]
        i += 1
        n = (tag & 0x7F) + 1
        if tag & 0x80:
            out[o:o + n * 2] = data[i:i + 2] * n
            i += 2
        else:
            out[o:o + n * 2] = data[i:i + n * 2]
            i += n * 2
        o += n * 2
    return bytes(out)


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def take_shot(port: str, out: str, frame: dict | None, settle: float) -> int:
    ser = Serial(port)
    try:
        ser.drain()
        if frame is not None:
            ser.write(frame_line(frame))
            time.sleep(settle)
            ser.drain()
        ser.write(b"@@SHOT\n")
        w = h = length = -1
        blob = bytearray()
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            line = ser.readline(1.0)
            if line is None:
                continue
            if line.startswith("!!SHOT "):
                _, sw, sh, sl = line.split()
                w, h, length = int(sw), int(sh), int(sl)
            elif line.startswith("!!DATA "):
                blob += bytes.fromhex(line[7:])
            elif line.startswith("!!SHOTEND"):
                break
            elif line.startswith("!!SHOTERR"):
                print("板子报告截图失败", file=sys.stderr)
                return 1
        if length < 0 or len(blob) < length:
            print(f"截图数据不完整: {len(blob)}/{length}", file=sys.stderr)
            return 1
    finally:
        ser.close()
    raw = rle_decode(bytes(blob[:length]), w * h)
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        rgb[i * 3] = ((v >> 11) & 0x1F) * 255 // 31
        rgb[i * 3 + 1] = ((v >> 5) & 0x3F) * 255 // 63
        rgb[i * 3 + 2] = (v & 0x1F) * 255 // 31
    write_png(out, w, h, bytes(rgb))
    print(f"已保存 {out} ({w}x{h})")
    return 0


# ---------------------------------------------------------------- 主循环


DEMO = {"v": 1, "t": "23:41", "a": [
    {"id": "a1", "k": "claude", "n": "agent-pond", "s": "tool", "x": "Bash", "g": 1, "c": 2},
    {"id": "b2", "k": "codex", "n": "magpie", "s": "think", "x": "", "g": 3, "c": 0},
    {"id": "c3", "k": "claude", "n": "douban", "s": "wait", "x": "", "g": 140, "c": 0},
    {"id": "d4", "k": "gemini", "n": "notes", "s": "tool", "x": "shell", "g": 2, "c": 0},
    {"id": "e5", "k": "codex", "n": "bisai", "s": "sleep", "x": "", "g": 1500, "c": 0},
]}


def main() -> int:
    ap = argparse.ArgumentParser(description="Agent 夜池：探测本机 agent 并推给 ESP32")
    ap.add_argument("--port", help="串口，默认自动找 /dev/cu.usbmodem*")
    ap.add_argument("--once", action="store_true", help="只打印一帧 JSON，不连板子")
    ap.add_argument("--demo", action="store_true", help="推一组假数据（看效果用）")
    ap.add_argument("--shot", metavar="PNG", help="截一张屏幕图存到本地")
    ap.add_argument("--settle", type=float, default=3.0, help="截图前等动画走几秒")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("-v", "--verbose", action="store_true", help="同时打印板子日志")
    args = ap.parse_args()

    if args.once:
        print(json.dumps(DEMO if args.demo else build_frame(), ensure_ascii=False, indent=1))
        return 0

    if args.shot:
        port = args.port or find_port()
        if not port:
            print("找不到串口", file=sys.stderr)
            return 1
        return take_shot(port, args.shot, DEMO if args.demo else build_frame(), args.settle)

    ser = None
    last_err = ""
    while True:
        try:
            if ser is None:
                port = args.port or find_port()
                if not port:
                    raise OSError("找不到 /dev/cu.usbmodem*")
                ser = Serial(port)
                print(f"[pond] 已连接 {port}", flush=True)
            frame = DEMO if args.demo else build_frame()
            if not ser.write(frame_line(frame)):
                raise OSError("写串口超时")
            while (line := ser.readline(0)) is not None:
                if args.verbose and line.strip() and not line.startswith("!!"):
                    print(f"[board] {line.strip()}", flush=True)
            summary = " ".join(f"{a['n']}:{a['s']}" for a in frame["a"]) or "(空)"
            if summary != last_err:
                print(f"[pond] {time.strftime('%H:%M:%S')} {summary}", flush=True)
                last_err = summary
        except OSError as e:
            if str(e) != last_err:
                print(f"[pond] {e}，稍后重试", flush=True)
                last_err = str(e)
            if ser:
                ser.close()
                ser = None
            time.sleep(2)
            continue
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
