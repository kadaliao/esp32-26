#!/usr/bin/env python3
"""把 magpie 的订阅配额 + Claude Code 的 /usage 汇总成一行 JSON，经 USB 串口推给 ESP32。

用法：
    python3 bridge.py              # 常驻推送（插上板子自检）
    python3 bridge.py --once       # 只打印一帧 JSON，不开串口
    python3 bridge.py --shot a.png # 让板子回传当前画面并存成 PNG
    python3 bridge.py --port /dev/cu.usbmodemXXXX

数据来源：
  - magpie 网关 http://127.0.0.1:3425/v1/magpie/quotas   (Codex / DeepSeek / Grok 的窗口和余额)
  - 本机 `claude -p /usage`                              (Claude 订阅的周 / Fable 窗口；非 Ultra 套餐另有 5h)
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import select
import struct
import subprocess
import sys
import termios
import threading
import time
import zlib
from datetime import datetime, timedelta

MAGPIE = os.environ.get("MAGPIE_URL", "http://127.0.0.1:3425")
PORT_GLOB = "/dev/cu.usbmodem*"

CLAUDE_TTL = 30.0
QUOTA_TTL = 10.0
ACCOUNT_TTL = 600.0

# Claude 套餐。magpie 只认得 max/pro，Ultra 会被报成 Max，所以这里以本地配置为准。
# Ultra 没有 5h 窗口（CLI 仍会打印一行 "Current session"，但没有意义），不显示。
CLAUDE_PLAN = os.environ.get("CLAUDE_PLAN", "Ultra")
CLAUDE_HAS_SESSION = CLAUDE_PLAN.lower() not in ("ultra",)

# 屏幕上从上到下的顺序
ORDER = ["claude", "codex", "deepseek", "grok"]
NAMES = {"claude": "Claude", "codex": "Codex", "deepseek": "DeepSeek", "grok": "Grok"}

MONTHS = {m: i + 1 for i, m in enumerate(
    ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"])}


# ---------------------------------------------------------------- 串口


class Serial:
    """裸 termios 串口。不碰 DTR/RTS —— ESP32-S3 原生 USB-JTAG 不需要。"""

    def __init__(self, path: str, baud: int = 115200):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0                      # iflag: 不做任何字符转换
        attrs[1] = 0                      # oflag: 原样输出
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0                      # lflag: 非行模式
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self._buf = b""

    def write(self, data: bytes, timeout: float = 2.0) -> bool:
        deadline = time.monotonic() + timeout
        view = memoryview(data)
        while view:
            try:
                n = os.write(self.fd, view)
                view = view[n:]
            except BlockingIOError:
                if time.monotonic() > deadline:
                    return False
                select.select([], [self.fd], [], 0.05)
        return True

    def readline(self, timeout: float) -> str | None:
        """读一行。内部保留缓冲，一次 read 拿到的多行不会被丢掉。"""
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                return line.decode("utf-8", "replace")
            remain = deadline - time.monotonic()
            if remain <= 0:
                return None
            r, _, _ = select.select([self.fd], [], [], min(remain, 0.2))
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 8192)
            except BlockingIOError:
                continue
            if chunk:
                self._buf += chunk

    def drain(self) -> None:
        self._buf = b""
        while True:
            try:
                if not os.read(self.fd, 4096):
                    return
            except (BlockingIOError, OSError):
                return

    def close(self) -> None:
        try:
            os.close(self.fd)
        except OSError:
            pass


def find_port() -> str | None:
    ports = sorted(glob.glob(PORT_GLOB))
    return ports[0] if ports else None


# ---------------------------------------------------------------- 数据源


def http_get_json(url: str, timeout: float = 5.0):
    import urllib.request

    req = urllib.request.Request(url, headers={"accept": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def fetch_quotas() -> list[dict]:
    try:
        data = http_get_json(f"{MAGPIE}/v1/magpie/quotas")
        return data.get("data", [])
    except Exception as exc:  # magpie 没开 / 端口改了
        print(f"[warn] magpie 配额读取失败: {exc}", file=sys.stderr)
        return []


def fetch_account_plans() -> dict[str, str]:
    """从 `magpie accounts --json` 取套餐名（claude 的 max/pro 只能从这拿到）。"""
    try:
        out = subprocess.run(["magpie", "accounts", "--json"], capture_output=True,
                             text=True, timeout=15)
        if out.returncode != 0:
            return {}
        plans = {}
        for acc in json.loads(out.stdout or "[]"):
            agent = acc.get("agent")
            plan = (acc.get("plan") or "").strip()
            if agent and plan:
                plans[agent] = plan.capitalize()
        return plans
    except Exception:
        return {}


RE_SESSION = re.compile(r"Current session:\s*(\d+)%\s*used[^\n]*", re.I)
RE_WEEK_ALL = re.compile(r"Current week \(all models\):\s*(\d+)%\s*used[^\n]*", re.I)
RE_WEEK_MODEL = re.compile(r"Current week \(([^)]+)\):\s*(\d+)%\s*used[^\n]*", re.I)
RE_WHEN = re.compile(
    r"([A-Za-z]{3})\w*\s+(\d{1,2})\s+at\s+(\d{1,2})(?::(\d{2}))?\s*(am|pm)?\s*(?:\(([^)]+)\))?",
    re.I)


def resolve_claude_bin() -> str | None:
    env = os.environ.get("CLAUDE_BIN")
    if env and os.path.exists(env):
        return env
    found = None
    for cand in ("claude", os.path.expanduser("~/.local/bin/claude")):
        from shutil import which
        path = which(cand) if "/" not in cand else cand
        if path and os.path.exists(path):
            found = path
            break
    if found:
        return found
    hits = sorted(glob.glob(os.path.expanduser(
        "~/.local/share/fnm/node-versions/*/installation/lib/node_modules/"
        "@anthropic-ai/claude-code/bin/claude.exe")))
    return hits[-1] if hits else None


def parse_when(text: str, now: datetime) -> int:
    """把 'Nov 1 at 9:20pm (Asia/Singapore)' 解析成“还有多少秒”。失败返回 -1。"""
    m = RE_WHEN.search(text)
    if not m:
        return -1
    mon, day, hour, minute, ampm, tzname = m.groups()
    month = MONTHS.get(mon[:3].lower())
    if not month:
        return -1
    hour = int(hour) % 12
    if (ampm or "").lower() == "pm":
        hour += 12
    minute = int(minute or 0)

    tz = None
    if tzname:
        try:
            from zoneinfo import ZoneInfo
            tz = ZoneInfo(tzname.strip())
        except Exception:
            tz = None

    target = None
    for year in (now.year + 1, now.year, now.year - 1):
        try:
            dt = datetime(year, month, int(day), hour, minute,
                          tzinfo=tz or now.tzinfo)
        except ValueError:
            continue
        delta = (dt - now).total_seconds()
        # 取“刚刚过去或还没到”的那个最近的候选
        if -86400 <= delta <= 400 * 86400:
            if target is None or abs(delta) < abs(target[1]):
                target = (dt, delta)
    if not target:
        return -1
    return max(0, int(target[1]))


class ClaudeCache:
    """`claude -p /usage` 可能要几十秒甚至卡住（launchd 下实测会挂住），
    绝不能在 2 秒一次的推送循环里同步等它。刷新放后台线程，用完缓存就走。"""

    def __init__(self, ttl: float = CLAUDE_TTL):
        self.ttl = ttl
        self.data: dict | None = None
        self.at = 0.0
        self.busy = False
        self.lock = threading.Lock()

    def maybe_refresh(self, now: datetime) -> None:
        with self.lock:
            if self.busy or (time.monotonic() - self.at) < self.ttl:
                return
            self.busy = True
        threading.Thread(target=self._work, args=(now,), daemon=True).start()

    def _work(self, now: datetime) -> None:
        try:
            res = claude_usage(now)
        except Exception:
            res = None
        with self.lock:
            if res is not None:
                self.data = res
            # 无论成败都记时间，避免失败后每 2 秒狂重试
            self.at = time.monotonic()
            self.busy = False


def claude_usage(now: datetime) -> dict | None:
    binary = resolve_claude_bin()
    if not binary:
        return None
    try:
        out = subprocess.run([binary, "-p", "/usage"], capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, timeout=25)
        text = out.stdout or ""
    except Exception as exc:
        print(f"[warn] claude /usage 失败: {exc}", file=sys.stderr)
        return None
    if "Current week" not in text and "Current session" not in text:
        return None

    result: dict = {"_at": now}
    m = RE_SESSION.search(text)
    if m:
        result["session"] = {"used": int(m.group(1)),
                             "reset_in": parse_when(m.group(0), now)}
    m = RE_WEEK_ALL.search(text)
    if m:
        result["week_all"] = {"used": int(m.group(1)),
                              "reset_in": parse_when(m.group(0), now)}
    for mm in RE_WEEK_MODEL.finditer(text):
        label = mm.group(1).strip()
        if label.lower() == "all models":
            continue
        result[label.lower()] = {"used": int(mm.group(2)),
                                 "reset_in": parse_when(mm.group(0), now)}
        break
    return result


# ---------------------------------------------------------------- 组帧


def reset_label(secs: int, now: datetime) -> str:
    """重置的本地时刻：24 小时内 "at 22:59"，更远 "Tue 09:00"。"""
    if secs is None or secs < 0:
        return ""
    # 秒数是向下取整的，+30s 再按分钟显示，免得 22:59 变成 22:58
    when = now.astimezone() + timedelta(seconds=secs + 30)
    if secs < 86400:
        return when.strftime("at %H:%M")
    return when.strftime("%a %H:%M")


def mkwin(key: str, used: int, secs: int, now: datetime) -> dict:
    return {"k": key, "u": used, "x": secs, "r": reset_label(secs, now)}


def secs_until(iso: str | None, now: datetime) -> int:
    if not iso:
        return -1
    try:
        dt = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return -1
    return max(0, int((dt - now).total_seconds()))


def window_key(name: str) -> str:
    low = (name or "").lower()
    if "5" in low and "hour" in low:
        return "5h"
    if "7" in low and "day" in low:
        return "7d"
    if "day" in low:
        return "1d"
    if "week" in low:
        return "7d"
    return (name or "?")[:6]


def pretty_balance(text: str | None) -> str:
    if not text:
        return ""
    if "credit" in text.lower():
        num = text.split()[0].replace(",", "")
        try:
            val = float(num)
            return f"{int(round(val)):,} credits"
        except ValueError:
            return text
    return text


def build_payload(quotas: list[dict], cc: dict | None, plans: dict[str, str],
                  now: datetime) -> dict:
    cards: list[dict] = []

    if cc:
        # cc 是最多 30 秒前的缓存，reset_in 要扣掉这段时间，否则倒计时和重置时刻都会偏
        age = max(0, int((now - cc["_at"]).total_seconds())) if cc.get("_at") else 0

        def left(w: dict) -> int:
            return max(0, w["reset_in"] - age) if w["reset_in"] >= 0 else -1

        wins = []
        if CLAUDE_HAS_SESSION and cc.get("session"):
            wins.append(mkwin("5h", cc["session"]["used"], left(cc["session"]), now))
        if cc.get("week_all"):
            wins.append(mkwin("7d", cc["week_all"]["used"], left(cc["week_all"]), now))
        extra = [k for k in cc if k not in ("session", "week_all", "_at")]
        meta = ""
        if extra:
            label = extra[0]
            meta = f"{label.capitalize()} {cc[label]['used']}%"
        cards.append({"id": "claude", "n": NAMES["claude"],
                      "s": CLAUDE_PLAN or plans.get("claude", ""), "w": wins, "m": meta})

    by_id: dict[str, dict] = {}
    for q in quotas:
        pid = q.get("provider") or ""
        if pid == "claude":
            continue  # 用 CLI 解析的数据，magpie 这边读不到 Claude 窗口
        wins = []
        for w in q.get("windows") or []:
            wins.append(mkwin(window_key(w.get("name")), int(w.get("used") or 0),
                              secs_until(w.get("resetsAt"), now), now))
        meta = pretty_balance(q.get("balance"))
        plan = (q.get("plan") or "").capitalize()
        if pid == "grok":
            plan = "SuperGrok" if "supergrok" in (q.get("name") or "").lower() else plan
        by_id[pid] = {"id": pid, "n": NAMES.get(pid, q.get("name") or pid),
                      "s": plan, "w": wins, "m": meta}

    for pid in ORDER:
        if pid == "claude":
            continue
        if pid in by_id:
            cards.append(by_id.pop(pid))
    for pid in sorted(by_id):
        cards.append(by_id[pid])

    return {"v": 1, "t": now.strftime("%H:%M:%S"), "p": cards}


# ---------------------------------------------------------------- 截图


def rle_decode(data: bytes, npix: int) -> bytes:
    out = bytearray(npix * 2)
    i = 0
    o = 0
    while i < len(data):
        tag = data[i]
        i += 1
        if tag & 0x80:
            n = (tag & 0x7F) + 1
            val = data[i:i + 2]
            i += 2
            out[o:o + n * 2] = val * n
            o += n * 2
        else:
            n = (tag & 0x7F) + 1
            out[o:o + n * 2] = data[i:i + n * 2]
            i += n * 2
            o += n * 2
    return bytes(out)


def write_png(path: str, w: int, h: int, rgb: bytes) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)


def take_shot(port: str, out: str, frame: dict | None = None) -> int:
    ser = Serial(port)
    try:
        ser.drain()
        if frame is not None:
            # 先推一帧，保证截到的是“有数据”的画面而不是 NO LINK
            line = "@@" + json.dumps(frame, ensure_ascii=False, separators=(",", ":")) + "\n"
            ser.write(line.encode("utf-8"))
            time.sleep(1.5)
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

    npix = w * h
    raw = rle_decode(bytes(blob[:length]), npix)
    rgb = bytearray(npix * 3)
    for i in range(npix):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        rgb[i * 3] = (r * 255) // 31
        rgb[i * 3 + 1] = (g * 255) // 63
        rgb[i * 3 + 2] = (b * 255) // 31
    write_png(out, w, h, bytes(rgb))
    print(f"已保存 {out} ({w}x{h}, {length} 字节 RLE)")
    return 0


# ---------------------------------------------------------------- 主循环


def main() -> int:
    ap = argparse.ArgumentParser(description="AI 用量面板串口桥接")
    ap.add_argument("--port", help="串口设备，默认自动找 /dev/cu.usbmodem*")
    ap.add_argument("--once", action="store_true", help="只打印一帧 JSON")
    ap.add_argument("--shot", metavar="PNG", help="把板子当前画面存成 PNG")
    ap.add_argument("--interval", type=float, default=2.0, help="推送间隔（秒）")
    args = ap.parse_args()

    if args.shot:
        port = args.port or find_port()
        if not port:
            print("没找到串口设备（板子插上了吗？）", file=sys.stderr)
            return 1
        now = datetime.now().astimezone()
        payload = build_payload(fetch_quotas(), claude_usage(now),
                                fetch_account_plans(), now)
        return take_shot(port, args.shot, payload)

    quotas: list[dict] = []
    plans: dict[str, str] = {}
    cc_cache = ClaudeCache()
    t_quota = t_plan = 0.0

    if args.once:
        now = datetime.now().astimezone()
        payload = build_payload(fetch_quotas(), claude_usage(now),
                                fetch_account_plans(), now)
        print(json.dumps(payload, ensure_ascii=False, indent=2))
        return 0

    port = args.port or find_port()
    if not port:
        print("没找到串口设备（板子插上了吗？）", file=sys.stderr)
        return 1
    print(f"[info] 串口 {port}，magpie {MAGPIE}")

    ser: Serial | None = None
    warned = False
    while True:
        started = time.monotonic()
        now = datetime.now().astimezone()

        if ser is None:
            try:
                ser = Serial(port)
                print(f"[info] 已打开 {port}")
                warned = False
            except OSError as exc:
                if not warned:
                    print(f"[warn] 打开串口失败: {exc}（每 5 秒重试）", file=sys.stderr)
                    warned = True
                time.sleep(5)
                port = find_port() or port
                continue

        if started - t_quota >= QUOTA_TTL:
            quotas = fetch_quotas()
            t_quota = started
        # claude 走后台线程刷新，卡住也不会堵住 2 秒一次的推送
        cc_cache.maybe_refresh(now)
        if started - t_plan >= ACCOUNT_TTL:
            plans = fetch_account_plans()
            t_plan = started

        payload = build_payload(quotas, cc_cache.data, plans, now)
        line = "@@" + json.dumps(payload, ensure_ascii=False, separators=(",", ":")) + "\n"
        try:
            ser.write(line.encode("utf-8"))
            ser.drain()
        except OSError as exc:
            print(f"[warn] 串口断开: {exc}", file=sys.stderr)
            ser.close()
            ser = None
            port = find_port() or port

        elapsed = time.monotonic() - started
        time.sleep(max(0.2, args.interval - elapsed))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
