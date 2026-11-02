#!/usr/bin/env python3
"""magpie live —— 把 magpie 网关的实时请求推给 ESP32。

magpie 本机网关（默认 http://127.0.0.1:3425）有两个只读接口，本机访问不用 key：
  GET /v1/magpie/concurrency                       此刻在飞的请求（agent、provider、模型、阶段、已用时）
  GET /v1/magpie/usage/requests?period=today       今天落地的请求（token、耗时、状态）+ 今日总数与花费

每 250ms 读一次，合成一帧推到串口：
  @@{"v":1,"t":"00:55","n":94,"c":5.43,
     "f":[{"id":"346","a":"dsh","an":"DeepSeek Harness","p":"douban-oneapi","pn":"Douban OneAPI",
           "m":"deepseek-flash","ph":"upstream","ms":14188}],          ← 在飞
     "d":[{"id":"346","a":"dsh",...,"o":500,"ms":21589,"ok":1}]}       ← 上一帧以来刚落地
  连不上网关时多一个 "x":1。

落地记录和在飞请求的 id 不是一套，这里按 agent+provider 把刚消失的在飞请求和新落地的记录配对，
配上了就沿用在飞的 id，板子上那只鸟就能原路飞回；没配上的（比如 Claude Code 直连、
只出现在日志里的请求）用记录自己的 id，板子会补一只快速往返的鸟。
"""
from __future__ import annotations

import argparse
import array
import hashlib
import subprocess
import glob
import json
import os
import select
import shutil
import signal
import struct
import sys
import termios
import time
import urllib.request
import zlib
from datetime import datetime

GATEWAY = os.environ.get("MAGPIE_GATEWAY", "http://127.0.0.1:3425")
PORT_GLOB = "/dev/cu.usbmodem*"
MAX_FLY = 12
MAX_DONE = 8
PAIR_WINDOW = 4.0
ICON_PX = 18
# magpie 装成 app 的时候没有 ~/.local/bin/magpie 这个软链，按常见位置挨个找；MAGPIE_BIN 可覆盖
MAGPIE_BIN_CANDIDATES = [
    os.path.expanduser("~/.local/bin/magpie"),
    "/Applications/magpie.app/Contents/MacOS/magpie",
    os.path.expanduser("~/Applications/magpie.app/Contents/MacOS/magpie"),
]
USER_ICONS = os.path.expanduser("~/.config/magpie/icons")
CACHE = os.path.expanduser("~/Library/Caches/magpie-live")   # 在飞请求消失后，最多等几秒去配它的落地记录


def get_json(path: str, timeout: float = 1.5) -> dict:
    with urllib.request.urlopen(GATEWAY + path, timeout=timeout) as r:
        return json.load(r)


def short(s: str, n: int) -> str:
    return s if len(s) <= n else s[: n - 1]


def parse_time(s: str) -> float:
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return 0.0


class Tracker:
    def __init__(self) -> None:
        self.names: dict[str, str] = {}      # agent/provider id → 显示名
        self.icons: dict[str, str] = {}      # agent/provider id → magpie 的图标名
        self.used_icons: set[str] = set()
        self.seen_rows: dict[str, None] = {}  # 有序，裁剪时丢最旧的
        self.primed = False
        self.active: dict[str, dict] = {}    # 在飞 id → 请求
        self.vanished: dict[str, tuple[float, dict]] = {}  # 刚消失、等配对的
        self.calls = 0
        self.cost = 0.0
        self.last_rows = 0.0

    @staticmethod
    def prov_id(p: str) -> str:
        return "direct" if p in ("", "session-unknown") else p

    def name(self, key: str, fallback: str = "") -> str:
        if key == "direct":
            return "direct"
        return self.names.get(key) or fallback or key

    def icon_key(self, key: str, model: str = "") -> str:
        icon = self.icons.get(key, "")
        if key == "direct":  # 没经过网关的请求，用模型厂商的图标
            m = model.lower()
            icon = ("claude-color" if "claude" in m else "openai" if m.startswith(("gpt", "o1", "o3", "o4"))
                    else "gemini-color" if "gemini" in m else "")
        if not icon:
            return ""
        self.used_icons.add(icon)
        return Icons.key(icon)

    def req(self, rid: str, agent: str, prov: str, model: str) -> dict:
        prov = self.prov_id(prov)
        return {"ai": self.icon_key(agent), "pi": self.icon_key(prov, model),"id": short(rid, 15), "a": short(agent, 15), "an": short(self.name(agent), 19),
                "p": short(prov, 19), "pn": short(self.name(prov), 19), "m": short(model, 23)}

    def fetch_rows(self, now: float) -> list[dict]:
        done: list[dict] = []
        # 落地记录不用那么勤，半秒一次
        if now - self.last_rows >= 0.5 or self.vanished:
            self.last_rows = now
            data = get_json("/v1/magpie/usage/requests?period=today&limit=40")
            self.calls = int(data.get("calls") or 0)
            self.cost = float(data.get("cost") or 0)
            for group in ("agents", "providers"):
                for it in data.get(group) or []:
                    if it.get("id") and it.get("name"):
                        self.names[it["id"]] = it["name"]
                    if it.get("id") and it.get("icon") and it["icon"] != "generic":
                        self.icons[it["id"]] = it["icon"]
            rows = data.get("rows") or []
            fresh = []
            for row in rows:
                key = row.get("rid") or f"{row.get('t')}|{row.get('agent')}|{row.get('model')}"
                if key in self.seen_rows:
                    continue
                self.seen_rows[key] = None
                row["_key"] = key
                fresh.append(row)
            if not self.primed:          # 启动时的历史记录不重放
                self.primed = True
                fresh = []
            fresh.sort(key=lambda r: parse_time(r.get("t", "")))
            for row in fresh:
                agent, prov = row.get("agent", ""), self.prov_id(row.get("provider", ""))
                match = None
                for rid, (_, r) in sorted(self.vanished.items(), key=lambda kv: kv[1][0]):
                    if r["a"] == short(agent, 15) and r["p"] == short(prov, 19):
                        match = rid
                        break
                if match:
                    self.vanished.pop(match)
                    rid = match
                else:
                    rid = row["_key"][-12:]
                d = self.req(rid, agent, prov, row.get("served") or row.get("model", ""))
                status = int(row.get("status") or 0)
                d["ok"] = 1 if status == 0 or 200 <= status < 400 else 0
                d["o"] = int(row.get("out") or 0)
                d["ms"] = int(row.get("ms") or 0)
                done.append(d)
            if len(self.seen_rows) > 4000:
                self.seen_rows = dict.fromkeys(list(self.seen_rows)[-2000:])
        return done

    def poll(self) -> dict:
        now = time.time()
        if not self.primed:
            self.last_rows = 0.0
            self.fetch_rows(now)
        conc = get_json("/v1/magpie/concurrency")
        fly = []
        cur = {}
        for a in conc.get("active") or []:
            rid = str(a.get("id", ""))
            r = self.req(rid, a.get("agent", ""), a.get("provider", ""), a.get("model", ""))
            r["ph"] = short(str(a.get("phase", "")), 11)
            r["ms"] = int(a.get("ms") or 0)
            cur[rid] = r
            fly.append(r)
        for rid, r in self.active.items():
            if rid not in cur:
                self.vanished[rid] = (now, r)
        self.active = cur

        done = self.fetch_rows(now)
        for rid in [k for k, (t, _) in self.vanished.items() if now - t > PAIR_WINDOW]:
            self.vanished.pop(rid)

        return {"v": 1, "t": time.strftime("%H:%M"), "n": self.calls, "c": round(self.cost, 2),
                "f": fly[:MAX_FLY], "d": done[-MAX_DONE:]}


# ---------------------------------------------------------------- 图标
#
# magpie 把 agent / provider 的图标（assets/icons/*.svg|png）编进了自己的程序里，网关不对外提供。
# 这里直接从程序文件里读 Go embed 的文件表：每条 6 个 8 字节字（名字指针、名字长度、数据指针、
# 数据长度、16 字节哈希）。macOS 的指针是链式 fixup 编码，低 36 位才是相对 __TEXT 的偏移。
# 用户自己设的 provider 图标在 ~/.config/magpie/icons/<hash>.ico，接口里写作 "file:<hash>.ico"。


def find_magpie() -> str | None:
    """找到 magpie 可执行文件。app 包装的话在 Contents/MacOS 里。"""
    for cand in [os.environ.get("MAGPIE_BIN", ""), *MAGPIE_BIN_CANDIDATES]:
        if cand and os.path.isfile(cand):
            return cand
    return shutil.which("magpie")


def extract_icons() -> str:
    out = os.path.join(CACHE, "icons")
    stamp = os.path.join(out, ".stamp")
    binary = find_magpie()
    if not binary:
        print("[live] 找不到 magpie 程序（~/Applications 或 /Applications 里的 magpie.app，"
              "或用 MAGPIE_BIN 指定），内置图标用不了", flush=True)
        return out
    try:
        mtime = str(os.path.getmtime(binary))
    except OSError:
        return out
    try:
        if os.path.exists(stamp):
            with open(stamp, encoding="utf-8") as fh:
                if fh.read() == mtime:
                    return out
        os.makedirs(out, exist_ok=True)
        with open(binary, "rb") as fh:
            b = fh.read()
    except OSError:  # 权限、文件正好被替换、缓存目录写不了：别让整条链路挂掉
        return out
    segs = {}
    for line in subprocess.run(["otool", "-l", binary], capture_output=True, text=True).stdout.split("Load command"):
        f = dict(x.split(None, 1) for x in line.strip().splitlines() if len(x.split(None, 1)) == 2)
        if f.get("cmd") == "LC_SEGMENT_64" and f.get("segname", "").startswith("__DATA"):
            segs[f["segname"]] = (int(f["fileoff"]), int(f["filesize"]))
    mask = (1 << 36) - 1
    tag = b"assets/icons/"
    n = 0
    for fo, size in segs.values():
        q = array.array("Q")
        q.frombytes(b[fo:fo + size // 8 * 8])
        k = 0
        while k + 4 < len(q):
            nl = q[k + 1]
            if 13 < nl < 120:
                no = q[k] & mask
                if b[no:no + 13] == tag:
                    name = b[no:no + nl].decode("latin1")
                    do, dl = q[k + 2] & mask, q[k + 3]
                    if dl < 1_000_000:
                        open(os.path.join(out, os.path.basename(name)), "wb").write(b[do:do + dl])
                        n += 1
                    k += 6
                    continue
            k += 1
    if n:
        open(stamp, "w").write(mtime)
    return out


class Icons:
    """把图标名渲染成 18x18 的 RGB565+alpha，给板子用。"""

    def __init__(self) -> None:
        self.dir = extract_icons()
        self.cache: dict[str, bytes | None] = {}

    @staticmethod
    def key(icon: str) -> str:
        return hashlib.sha1(icon.encode()).hexdigest()[:8]

    def source(self, icon: str) -> str | None:
        if icon.startswith("file:"):
            p = os.path.join(USER_ICONS, os.path.basename(icon[5:]))
            return p if os.path.exists(p) else None
        for ext in (".svg", ".png"):
            p = os.path.join(self.dir, icon + ext)
            if os.path.exists(p):
                return p
        return None

    def raw(self, icon: str) -> bytes | None:
        if icon in self.cache:
            return self.cache[icon]
        data = None
        src = self.source(icon)
        if src:
            try:
                n = str(ICON_PX)
                if src.endswith(".svg"):
                    png = subprocess.run(["rsvg-convert", "-w", n, "-h", n, "-a", src],
                                         capture_output=True, check=True).stdout
                    rgba = subprocess.run(["magick", "png:-", "-background", "none", "-gravity", "center",
                                           "-extent", f"{n}x{n}", "rgba:-"], input=png,
                                          capture_output=True, check=True).stdout
                else:
                    frame = src
                    if src.endswith(".ico"):  # .ico 可能有好几帧，取最大的那帧
                        sizes = subprocess.run(["magick", "identify", "-format", "%w\\n", src],
                                               capture_output=True, text=True).stdout.split()
                        if sizes:
                            frame = f"{src}[{max(range(len(sizes)), key=lambda i: int(sizes[i]))}]"
                    rgba = subprocess.run(["magick", frame, "-resize", f"{n}x{n}", "-background", "none",
                                           "-gravity", "center", "-extent", f"{n}x{n}", "rgba:-"],
                                          capture_output=True, check=True).stdout
                if len(rgba) == ICON_PX * ICON_PX * 4:
                    out = bytearray()
                    for i in range(0, len(rgba), 4):
                        r, g, bl, a = rgba[i:i + 4]
                        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)
                        out += bytes((v & 0xFF, v >> 8, a))
                    data = bytes(out)
            except (OSError, subprocess.CalledProcessError):
                data = None
        self.cache[icon] = data
        return data

    def line(self, icon: str) -> bytes | None:
        data = self.raw(icon)
        if not data:
            return None
        return f"@@ICON {self.key(icon)} {ICON_PX} {data.hex()}\n".encode()


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
                if not os.path.exists(self.path):
                    raise OSError("板子断开了")
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
                    chunk = os.read(self.fd, 8192)
                except BlockingIOError:
                    continue
                if not chunk:  # 可读却读到空：设备被拔掉了
                    raise OSError("板子断开了")
                self._buf += chunk
            if remain <= 0:
                return None

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
    # 板子字体只有 ASCII，非 ASCII 字符换成 ?
    text = json.dumps(frame, ensure_ascii=False, separators=(",", ":"))
    text = "".join(ch if ord(ch) < 128 else "?" for ch in text)
    return ("@@" + text + "\n").encode()


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


def take_shot(ser: Serial, out: str) -> int:
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


# ---------------------------------------------------------------- 演示数据


class Demo:
    """不依赖网关的假流量：几个 agent 往几个 provider 发请求。"""
    AGENTS = [("claude", "Claude Code", "claudecode-color"), ("codex", "Codex", "codex-color"),
              ("dsh", "DeepSeek Harness", "deepseek-color"), ("opencode", "OpenCode", "opencode")]
    PROVS = [("anthropic", "Anthropic", "anthropic"), ("openai", "OpenAI", "openai"),
             ("deepseek", "DeepSeek", "deepseek-color"), ("openrouter", "OpenRouter", "openrouter")]
    used_icons = {x[2] for x in AGENTS + PROVS}

    def __init__(self) -> None:
        import random
        self.rng = random.Random(7)
        self.fly: dict[str, tuple[float, float, int, int]] = {}
        self.seq = 100
        self.calls = 312
        self.cost = 4.18

    def poll(self) -> dict:
        now = time.time()
        rng = self.rng
        if len(self.fly) < 7 and rng.random() < 0.25:
            self.seq += 1
            self.fly[str(self.seq)] = (now, rng.uniform(2, 9), rng.randrange(4), rng.randrange(4))
        f, d = [], []
        for rid, (t0, dur, ai, pi) in list(self.fly.items()):
            a, p = self.AGENTS[ai], self.PROVS[pi]
            r = {"id": rid, "a": a[0], "an": a[1], "p": p[0], "pn": p[1], "m": "model",
                 "ai": Icons.key(a[2]), "pi": Icons.key(p[2])}
            if now - t0 >= dur:
                del self.fly[rid]
                self.calls += 1
                self.cost += 0.02
                d.append({**r, "ok": 0 if rng.random() < 0.1 else 1, "o": rng.randrange(80, 3000), "ms": int(dur * 1000)})
            else:
                f.append({**r, "ph": "upstream", "ms": int((now - t0) * 1000)})
        return {"v": 1, "t": time.strftime("%H:%M"), "n": self.calls, "c": round(self.cost, 2), "f": f, "d": d}


# ---------------------------------------------------------------- 主循环


class Stop(Exception):
    """收到退出信号。"""


def on_signal(signum, _frame):
    raise Stop(signal.Signals(signum).name)


def say_bye(ser: "Serial | None") -> None:
    """告诉板子 Mac 端要走了，让它显示「已停止」而不是「信号丢失」。尽力而为，不等。"""
    if ser is None:
        return
    try:
        ser.write(b"@@BYE\n", timeout=0.3)
    except OSError:
        pass
    ser.close()



def main() -> int:
    ap = argparse.ArgumentParser(description="magpie live：把 magpie 网关的实时请求推给 ESP32")
    ap.add_argument("--port", help="串口，默认自动找 /dev/cu.usbmodem*")
    ap.add_argument("--once", action="store_true", help="不连板子，每 250ms 打印一帧（Ctrl-C 退出）")
    ap.add_argument("--demo", action="store_true", help="推假流量看效果，不读网关")
    ap.add_argument("--shot", metavar="PNG", help="先推几秒数据，再截一张屏幕图")
    ap.add_argument("--settle", type=float, default=6.0, help="截图前推几秒")
    ap.add_argument("--interval", type=float, default=0.25)
    ap.add_argument("-v", "--verbose", action="store_true", help="同时打印板子日志")
    args = ap.parse_args()

    src = Demo() if args.demo else Tracker()

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)

    gw_err = [""]

    def frame() -> dict:
        try:
            fr = src.poll()
            gw_err[0] = ""
            return fr
        except Stop:
            raise
        except Exception as e:  # 连不上、超时、返回的东西不对，都按「网关离线」处理
            msg = f"{type(e).__name__}: {e}"
            if msg != gw_err[0]:
                print(f"[live] 读网关失败：{msg}", flush=True)
                gw_err[0] = msg
            return {"v": 1, "t": time.strftime("%H:%M"), "x": 1, "f": [], "d": []}

    if args.once:
        try:
            while True:
                fr = frame()
                if fr["f"] or fr["d"] or fr.get("x"):
                    print(json.dumps(fr, ensure_ascii=False), flush=True)
                time.sleep(args.interval)
        except Stop:
            return 0

    icons = Icons()
    icons_sent: set[str] = set()
    icons_at = time.monotonic()
    ser: Serial | None = None
    last = ""
    shot_at = time.monotonic() + args.settle

    def drop(reason: str) -> None:
        nonlocal ser, last
        if reason != last:
            print(f"[live] {reason}，稍后重试", flush=True)
            last = reason
        if ser:
            ser.close()
            ser = None

    try:
        while True:
            try:
                if ser is None:
                    port = args.port or find_port()
                    if not port:
                        raise OSError("找不到 /dev/cu.usbmodem*")
                    ser = Serial(port)
                    icons_sent.clear()
                    last = ""
                    print(f"[live] 已连接 {port}", flush=True)
                fr = frame()
                # 新出现的图标立刻发；所有图标每 20 秒重发一遍（板子可能重启过）
                used = getattr(src, "used_icons", set())
                if time.monotonic() - icons_at > 20:
                    icons_sent.clear()
                    icons_at = time.monotonic()
                for icon in sorted(used - icons_sent):
                    icons_sent.add(icon)
                    if (ln := icons.line(icon)) and not ser.write(ln):
                        raise OSError("写串口超时")
                if not ser.write(frame_line(fr)):
                    raise OSError("写串口超时")
                while (line := ser.readline(0)) is not None:
                    if args.verbose and line.strip() and not line.startswith("!!"):
                        print(f"[board] {line.strip()}", flush=True)
                for d in fr["d"]:
                    print(f"[live] {time.strftime('%H:%M:%S')} {'✓' if d['ok'] else '✗'} {d['an']} → {d['pn']}"
                          f"  {d['m']}  {d['o']} tok  {d['ms'] / 1000:.1f}s", flush=True)
                state = "网关离线" if fr.get("x") else f"在飞 {len(fr['f'])}"
                if state != last:
                    print(f"[live] {time.strftime('%H:%M:%S')} {state}", flush=True)
                    last = state
                if args.shot and time.monotonic() >= shot_at:
                    shot_ser, ser = ser, None  # 截图后不发 BYE，画面保持原样
                    try:
                        return take_shot(shot_ser, args.shot)
                    finally:
                        shot_ser.close()
            except OSError as e:  # 拔线、串口被占、写超时：关掉，等板子回来
                drop(str(e) or type(e).__name__)
                time.sleep(2)
                continue
            except Stop:
                raise
            except Exception as e:  # 别的意外：记下来继续跑，不让常驻进程因为一帧坏数据挂掉
                drop(f"意外错误 {type(e).__name__}: {e}")
                time.sleep(2)
                continue
            time.sleep(args.interval)
    except Stop as e:
        print(f"\n[live] 收到 {e}，退出", flush=True)
        return 0
    finally:
        say_bye(ser)

if __name__ == "__main__":
    sys.exit(main())
