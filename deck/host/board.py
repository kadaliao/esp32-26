"""和板子打交道的部分：串口、找设备、组帧、截图解码。

三个老项目的主机脚本里各抄了一份，这里合成一份，所有 app 共用。
"""
from __future__ import annotations

import glob
import json
import os
import select
import struct
import termios
import time
import zlib

PORT_GLOB = "/dev/cu.usbmodem*"


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
        """读一行。内部保留缓冲，一次 read 拿到的多行不会被丢掉。timeout=0 就是只看缓冲和已到的数据。"""
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                return line.decode("utf-8", "replace")
            remain = max(0.0, deadline - time.monotonic())
            r, _, _ = select.select([self.fd], [], [], min(remain, 0.2))
            if r:
                try:
                    chunk = os.read(self.fd, 8192)
                except BlockingIOError:
                    chunk = b""
                if not chunk and remain <= 0:
                    return None
                self._buf += chunk
            elif remain <= 0:
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


def frame_line(app: str, frame: dict) -> bytes:
    """@@<app>{json}\\n"""
    return ("@@" + app + json.dumps(frame, ensure_ascii=False, separators=(",", ":")) + "\n").encode()


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


def take_shot(ser: Serial, out: str, on_line=None) -> str:
    """让板子回传当前画面，存成 PNG。返回空串表示成功，否则是错误说明。
    截图期间收到的其他行交给 on_line（比如板子日志、手势）。"""
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
            return "板子报告截图失败"
        elif on_line:
            on_line(line)
    if length < 0 or len(blob) < length:
        return f"截图数据不完整: {len(blob)}/{length}"
    raw = rle_decode(bytes(blob[:length]), w * h)
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        v = raw[i * 2] | (raw[i * 2 + 1] << 8)
        rgb[i * 3] = ((v >> 11) & 0x1F) * 255 // 31
        rgb[i * 3 + 1] = ((v >> 5) & 0x3F) * 255 // 63
        rgb[i * 3 + 2] = (v & 0x1F) * 255 // 31
    write_png(out, w, h, bytes(rgb))
    return ""
