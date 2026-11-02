#!/usr/bin/env python3
"""deck —— 桌面小面板的主机端：一个进程给板子上所有 app 供数据。

每个 app 一个线程，按自己的节奏取数据（系统 1 秒、magpie 250ms、agent 1 秒、额度 2 秒），
排进发件箱，主线程统一写串口：

    @@sys{...}  @@magpie{...}  @@pond{...}  @@quota{...}   给各个 app
    @@deck{"t":"14:32","d":"SUN 02 NOV","h":"..."}         给面板自己（首页顶栏）
    @@ICON ...                                             magpie 的图标

某个 app 卡住或报错只影响它自己的磁贴（变成 STALE），别的照常。

用法：
    python3 host/deck.py                  前台推送（Ctrl-C 停，退出前让板子显示「已停止」）
    python3 host/deck.py --demo           全部用假数据
    python3 host/deck.py --once           不连板子，每个 app 打印一帧
    python3 host/deck.py --only sys,magpie
    python3 host/deck.py --go sys         连上后切到某个 app（home / sys / magpie / pond / quota）
    python3 host/deck.py --shot a.png --go sys   推几秒数据后截屏
    python3 host/deck.py -v               同时打印板子日志和触摸手势
"""
from __future__ import annotations

import argparse
import importlib
import json
import os
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from board import Serial, find_port, frame_line, take_shot  # noqa: E402

# 新 app：写一个带 id / interval / poll() 的类（见 README「加一个新 app」），再加一行到这里。
# 顺序无所谓，板子上的顺序由固件 shell.c 的 APPS[] 决定。
APP_CLASSES = [
    ("apps.sysmon", "SysApp"),
    ("apps.magpie", "MagpieApp"),
    ("apps.pond", "PondApp"),
    ("apps.quota", "QuotaApp"),
]


def log(tag: str, msg: str) -> None:
    print(f"[{tag}] {time.strftime('%H:%M:%S')} {msg}", flush=True)


def load_apps() -> list:
    """某个 app 的模块坏了只跳过它，别的照常跑。"""
    out = []
    for mod, cls in APP_CLASSES:
        try:
            out.append(getattr(importlib.import_module(mod), cls))
        except Exception as e:
            log("deck", f"{mod} 加载失败，跳过：{type(e).__name__}: {e}")
    return out


class Outbox:
    """发件箱。断线期间只留每个 app 最新的那一批，重连后不补发过期数据。"""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.items: list[tuple[str, list[bytes]]] = []

    def put(self, app_id: str, lines: list[bytes]) -> None:
        with self.lock:
            self.items.append((app_id, lines))
            if len(self.items) > 400:  # 主线程卡住了：兜底别无限涨
                self._latest_only()

    def take(self) -> list[tuple[str, list[bytes]]]:
        with self.lock:
            items, self.items = self.items, []
        return items

    def latest_only(self) -> None:
        with self.lock:
            self._latest_only()

    def _latest_only(self) -> None:
        last: dict[str, tuple[str, list[bytes]]] = {}
        for it in self.items:
            last[it[0]] = it
        self.items = list(last.values())


class Feeder(threading.Thread):
    """一个 app 一个线程：取数据、排进发件箱、日志里报变化。"""

    SUMMARY_EVERY = 300.0  # 不加 -v 时，摘要每个 app 最多这么久记一行：常驻服务的日志别一天涨好几 MB

    def __init__(self, app, demo: bool, outbox: Outbox, stop: threading.Event, verbose: bool):
        super().__init__(daemon=True, name=app.id)
        self.app, self.demo, self.outbox, self.stop, self.verbose = app, demo, outbox, stop, verbose

    def run(self) -> None:
        app = self.app
        last_sum = last_err = None
        sum_at = -1e9
        while not self.stop.is_set():
            t0 = time.monotonic()
            try:
                frame = app.demo() if self.demo else app.poll()
                extra = app.extra_lines() if hasattr(app, "extra_lines") else []
                self.outbox.put(app.id, extra + [frame_line(app.id, frame)])
                if hasattr(app, "summary") and (s := app.summary(frame)) != last_sum:
                    if self.verbose or t0 - sum_at >= self.SUMMARY_EVERY:
                        log(app.id, s)
                        sum_at = t0
                    last_sum = s
                if self.verbose and hasattr(app, "events"):
                    for e in app.events(frame):
                        log(app.id, e)
                last_err = None
            except Exception as e:  # 一帧坏数据别让线程死掉
                msg = f"{type(e).__name__}: {e}"
                if msg != last_err:
                    log(app.id, f"取数据失败：{msg}")
                    last_err = msg
            self.stop.wait(max(0.05, app.interval - (time.monotonic() - t0)))


def host_label() -> str:
    """首页右上角显示的主机名（板子有中文后备字库，中文名也能显示）。"""
    try:
        name = subprocess.run(["scutil", "--get", "ComputerName"], capture_output=True, text=True,
                              timeout=3).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        name = ""
    name = "".join(ch for ch in name if ord(ch) >= 32).replace("'s ", " ").strip()
    return name.upper()[:18]


def deck_frame(host: str) -> dict:
    return {"t": time.strftime("%H:%M"), "d": time.strftime("%a %d %b").upper(), "h": host}


SERVICE = "com.liaoxingyi.deck"
SERVICE_PLIST = os.path.expanduser(f"~/Library/LaunchAgents/{SERVICE}.plist")


def pause_service() -> bool:
    """前台跑的时候，常驻服务会抢同一个串口：先停掉它，返回要不要在退出时拉起来。
    常驻服务自己（plist 里设了 DECK_SERVICE=1）不走这里。"""
    if os.environ.get("DECK_SERVICE"):
        return False
    target = f"gui/{os.getuid()}/{SERVICE}"
    if subprocess.run(["launchctl", "print", target], capture_output=True).returncode != 0:
        return False
    subprocess.run(["launchctl", "bootout", target], capture_output=True)
    log("deck", "已暂停常驻服务，退出时恢复")
    time.sleep(0.5)  # 等它松开串口
    return True


def resume_service() -> None:
    r = subprocess.run(["launchctl", "bootstrap", f"gui/{os.getuid()}", SERVICE_PLIST], capture_output=True)
    log("deck", "常驻服务已恢复" if r.returncode == 0 else f"常驻服务没能恢复：{r.stderr.decode().strip()}")


class Stop(Exception):
    """收到退出信号。"""


def on_signal(signum, _frame):
    raise Stop(signal.Signals(signum).name)


def run(args, apps: list) -> int:
    outbox = Outbox()
    stop = threading.Event()
    for a in apps:
        Feeder(a, args.demo, outbox, stop, args.verbose).start()

    host = host_label()
    ser: Serial | None = None
    last_reason = ""
    deck_at = 0.0
    shot_at: float | None = None

    def on_board(line: str) -> None:
        if line.startswith("!!HELLO"):
            parts = line.split()
            log("deck", f"板子固件 {' '.join(parts[1:3])}，当前画面 {parts[3] if len(parts) > 3 else '?'}")
        elif line.startswith("!!APP "):
            log("deck", f"板子切到 {line[6:].strip()}")
        elif line.startswith("!!IN "):
            if args.verbose:
                log("touch", line[5:].strip())
        elif line.startswith("!!"):
            pass
        elif args.verbose and line.strip():
            log("board", line.strip())

    try:
        while True:
            try:
                if ser is None:
                    port = args.port or find_port()
                    if not port:
                        raise OSError("找不到 /dev/cu.usbmodem*（板子插上了吗？）")
                    ser = Serial(port)
                    ser.drain()
                    log("deck", f"已连接 {port}")
                    last_reason = ""
                    for a in apps:
                        if hasattr(a, "on_connect"):
                            a.on_connect()
                    ser.write(b"@@HELLO\n")
                    deck_at = 0.0
                    if args.go:
                        ser.write(f"@@GO {args.go}\n".encode())
                    if args.shot:
                        shot_at = time.monotonic() + args.settle

                now = time.monotonic()
                if now - deck_at >= 5:
                    deck_at = now
                    if not ser.write(frame_line("deck", deck_frame(host))):
                        raise OSError("写串口超时")
                for _, lines in outbox.take():
                    for ln in lines:
                        if not ser.write(ln):
                            raise OSError("写串口超时")
                while (line := ser.readline(0)) is not None:
                    on_board(line)

                if shot_at is not None and now >= shot_at:
                    if args.go:  # 等的这几秒里可能有人点过屏幕：截之前再切一次，等淡入淡出走完
                        ser.write(f"@@GO {args.go}\n".encode())
                        time.sleep(0.6)
                    err = take_shot(ser, args.shot, on_line=on_board)
                    ser.close()
                    ser = None  # 截完不发 BYE，画面保持原样
                    if err:
                        print(err, file=sys.stderr)
                        return 1
                    print(f"已保存 {args.shot}")
                    return 0
            except OSError as e:  # 拔线、串口被占、写超时：关掉，等板子回来
                reason = str(e) or type(e).__name__
                if reason != last_reason:
                    log("deck", f"{reason}，2 秒后重试")
                    last_reason = reason
                if ser:
                    ser.close()
                    ser = None
                outbox.latest_only()
                time.sleep(2)
                continue
            time.sleep(0.03)
    except Stop as e:
        print(f"\n[deck] 收到 {e}，退出", flush=True)
        return 0
    finally:
        stop.set()
        if ser is not None:  # 告诉板子 Mac 端要走了，显示「已停止」而不是「信号丢失」
            try:
                ser.write(b"@@BYE\n", timeout=0.3)
            except OSError:
                pass
            ser.close()


def main() -> int:
    ap = argparse.ArgumentParser(description="deck：给桌面小面板上的所有 app 供数据")
    ap.add_argument("--port", help="串口，默认自动找 /dev/cu.usbmodem*")
    ap.add_argument("--demo", action="store_true", help="全部用假数据")
    ap.add_argument("--once", action="store_true", help="不连板子，每个 app 打印一帧 JSON")
    ap.add_argument("--only", help="只跑这几个 app，逗号分隔：sys,magpie,pond,quota")
    ap.add_argument("--go", help="连上后切到某个 app（home 是首页）")
    ap.add_argument("--shot", metavar="PNG", help="推几秒数据后截一张屏幕图")
    ap.add_argument("--settle", type=float, default=5.0, help="截图前推几秒")
    ap.add_argument("-v", "--verbose", action="store_true", help="同时打印板子日志和触摸手势")
    args = ap.parse_args()

    classes = load_apps()
    if args.only:
        want = set(args.only.split(","))
        classes = [c for c in classes if c.id in want]
        if not classes:
            print(f"没有这些 app：{args.only}", file=sys.stderr)
            return 1
    apps = [c() for c in classes]

    if args.once:
        for a in apps:
            # 有的数据（CPU、网速）要两次采样才算得出速率
            frame = a.demo() if args.demo else a.poll()
            if not args.demo and a.interval <= 1.0:
                time.sleep(a.interval)
                frame = a.poll()
            print(f"--- {a.id}")
            print(json.dumps(frame, ensure_ascii=False))
        return 0

    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, on_signal)
    paused = pause_service()
    try:
        return run(args, apps)
    finally:
        if paused:
            resume_service()


if __name__ == "__main__":
    sys.exit(main())
