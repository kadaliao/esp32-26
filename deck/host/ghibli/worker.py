# /// script
# requires-python = ">=3.10"
# dependencies = ["onnxruntime>=1.18", "numpy", "pillow"]
# ///
"""吉卜力风格的推理进程（AnimeGANv3 的 Hayao 模型，宫崎骏风格）。

由 host/apps/ghibli.py 用 `uv run --script` 拉起，依赖装在 uv 自己的缓存环境里，不碰 deck.py 的标准库环境。
stdin / stdout 走二进制帧：4 字节大端长度 + JPEG。进来的是摄像头画面，出去的是画好的 320×240。
日志写 stderr。
"""
from __future__ import annotations

import hashlib
import io
import os
import subprocess
import sys
import time
import urllib.request

import numpy as np
import onnxruntime as ort
from PIL import Image

MODEL_URL = "https://github.com/TachibanaYoshino/AnimeGANv3/releases/download/v1.1.0/AnimeGANv3_Hayao_36.onnx"
MODEL_SHA256 = "95ba7b219073fd5b12f569bc38056ffd3019cf4caf15b1feb9f73d1286c9f69d"
MODEL_PATH = os.path.expanduser("~/Library/Caches/deck/models/AnimeGANv3_Hayao_36.onnx")
IN_W, IN_H = 320, 256      # 模型要求边长是 8 的倍数且不小于 256：240 高拉到 256
OUT_W, OUT_H = 320, 240    # 板子屏幕
JPEG_Q = 85


def log(msg: str) -> None:
    print(f"[ghibli-worker] {msg}", file=sys.stderr, flush=True)


def ensure_model() -> str:
    if os.path.exists(MODEL_PATH):
        return MODEL_PATH
    os.makedirs(os.path.dirname(MODEL_PATH), exist_ok=True)
    log(f"下载模型 {MODEL_URL}")
    data = urllib.request.urlopen(MODEL_URL, timeout=60).read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != MODEL_SHA256:
        raise RuntimeError(f"模型校验失败：sha256 {digest}")
    tmp = MODEL_PATH + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, MODEL_PATH)
    return MODEL_PATH


def perf_cores() -> int:
    """只用性能核：实测 M1 Max 上 8 个性能核比默认（连能效核一起）快三分之一。"""
    try:
        return int(subprocess.run(["sysctl", "-n", "hw.perflevel0.physicalcpu"], capture_output=True,
                                  text=True, timeout=2).stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0


def load_session() -> tuple[ort.InferenceSession, str]:
    so = ort.SessionOptions()
    if (n := perf_cores()) > 0:
        so.intra_op_num_threads = n
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    path = ensure_model()
    # 输入的高、宽在模型里是动态的：固定下来，ORT 能多做一些图优化
    probe = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
    dims = probe.get_inputs()[0].shape  # [1, H, W, 3]
    for dim, val in ((dims[1], IN_H), (dims[2], IN_W)):
        if isinstance(dim, str):
            so.add_free_dimension_override_by_name(dim, val)
    sess = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
    return sess, sess.get_inputs()[0].name


def read_exact(f, n: int) -> bytes | None:
    buf = b""
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def main() -> int:
    t0 = time.monotonic()
    sess, name = load_session()
    log(f"模型就绪，{(time.monotonic() - t0) * 1000:.0f} ms")
    inp, out = sys.stdin.buffer, sys.stdout.buffer
    n, spent = 0, 0.0
    while True:
        head = read_exact(inp, 4)
        if head is None:
            return 0
        data = read_exact(inp, int.from_bytes(head, "big"))
        if data is None:
            return 0
        t = time.monotonic()
        img = Image.open(io.BytesIO(data)).convert("RGB").resize((IN_W, IN_H), Image.BILINEAR)
        x = np.asarray(img, dtype=np.float32)[None] / 127.5 - 1.0
        y = sess.run(None, {name: x})[0][0]
        pic = Image.fromarray(((y + 1.0) * 127.5).clip(0, 255).astype(np.uint8)).resize((OUT_W, OUT_H),
                                                                                        Image.BILINEAR)
        buf = io.BytesIO()
        pic.save(buf, "JPEG", quality=JPEG_Q)
        res = buf.getvalue()
        out.write(len(res).to_bytes(4, "big") + res)
        out.flush()
        n += 1
        spent += time.monotonic() - t
        if n % 100 == 0:
            log(f"{n} 帧，平均 {spent / 100 * 1000:.0f} ms/帧")
            spent = 0.0


if __name__ == "__main__":
    sys.exit(main())
