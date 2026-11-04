"""GHIBLI：板子把摄像头画面（JPEG）送来，这里画成吉卜力风格再送回去。

板子上 MIRROR 切到 GHIBLI 效果时才会送画面过来；没画面时什么都不做。
真正跑模型的是 host/ghibli/worker.py：用 `uv run --script` 拉起的子进程，依赖装在 uv 的缓存里，
deck.py 本身还是只用标准库。第一次用要装 onnxruntime、下模型（约 4MB），之后几秒就能起来。
30 秒没画面就把子进程停掉，不占 CPU 和内存。

板子 → 这里：!!BIN cam <len> + JPEG（deck.py 收到后调 on_bin）
这里 → 板子：@@BIN fx <len> + JPEG（320×240）
"""
from __future__ import annotations

import collections
import os
import shutil
import subprocess
import threading
import time

WORKER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ghibli", "worker.py")
UV_CANDIDATES = ("/opt/homebrew/bin/uv", "/usr/local/bin/uv", os.path.expanduser("~/.local/bin/uv"),
                 os.path.expanduser("~/.cargo/bin/uv"))


def find_uv() -> str | None:
    """常驻服务（launchd）的 PATH 很短，找不到时按常见安装位置找。"""
    if p := shutil.which("uv"):
        return p
    return next((p for p in UV_CANDIDATES if os.path.exists(p)), None)


class GhibliApp:
    id = "ghibli"
    bin_tags = ("cam",)
    IDLE_S = 30.0      # 这么久没画面就停掉推理进程

    def __init__(self) -> None:
        self.frames: collections.deque[bytes] = collections.deque(maxlen=2)  # 板子最多 2 帧在路上
        self.cv = threading.Condition()
        self.last_frame = 0.0
        self.proc: subprocess.Popen | None = None
        self.log_err: threading.Thread | None = None

    # ---- deck.py 调的
    def start(self, outbox, stop: threading.Event, log) -> None:
        self.outbox, self.stop, self.log = outbox, stop, log
        threading.Thread(target=self._run, daemon=True, name="ghibli").start()

    def on_bin(self, tag: str, data: bytes) -> None:
        with self.cv:
            self.frames.append(data)
            self.last_frame = time.monotonic()
            self.cv.notify()

    # ---- 推理线程
    def _run(self) -> None:
        while not self.stop.is_set():
            with self.cv:
                if not self.frames:
                    self.cv.wait(1.0)
                data = self.frames.popleft() if self.frames else None
            if data is None:
                if self.proc and time.monotonic() - self.last_frame > self.IDLE_S:
                    self.log(self.id, "一阵子没画面了，停掉推理进程")
                    self._kill()
                continue
            try:
                out = self._paint(data)
            except Exception as e:  # 推理进程挂了：下一帧重新拉起
                self.log(self.id, f"推理失败：{type(e).__name__}: {e}")
                self._kill()
                self.stop.wait(2.0)
                continue
            self.outbox.put(self.id, [f"@@BIN fx {len(out)}\n".encode() + out])
        self._kill()

    def _paint(self, data: bytes) -> bytes:
        proc = self._ensure()
        proc.stdin.write(len(data).to_bytes(4, "big") + data)
        proc.stdin.flush()
        head = self._read(proc, 4)
        return self._read(proc, int.from_bytes(head, "big"))

    def _read(self, proc: subprocess.Popen, n: int) -> bytes:
        buf = b""
        while len(buf) < n:
            chunk = proc.stdout.read(n - len(buf))
            if not chunk:
                raise RuntimeError(f"推理进程退出了（{proc.poll()}）")
            buf += chunk
        return buf

    def _ensure(self) -> subprocess.Popen:
        if self.proc and self.proc.poll() is None:
            return self.proc
        uv = find_uv()
        if not uv:
            raise RuntimeError("找不到 uv（brew install uv）")
        self.log(self.id, "拉起推理进程（第一次会装 onnxruntime、下模型）")
        self.proc = subprocess.Popen([uv, "run", "--quiet", "--script", WORKER], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        proc = self.proc

        def pump() -> None:  # 推理进程的日志转到 deck 的日志里
            for line in proc.stderr:
                text = line.decode("utf-8", "replace").strip()
                if text and "Warning" not in text:
                    self.log(self.id, text)

        threading.Thread(target=pump, daemon=True, name="ghibli-log").start()
        return proc

    def _kill(self) -> None:
        if self.proc:
            try:
                self.proc.stdin.close()
                self.proc.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                self.proc.kill()
            self.proc = None
