#!/usr/bin/env python3
"""读板子串口日志若干秒（帧率等 WARN 级日志）。用法：scripts/log.py [秒数]"""
import glob, os, select, sys, time
port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
end = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 12)
while time.time() < end:
    if select.select([fd], [], [], 0.5)[0]:
        try:
            sys.stdout.write(os.read(fd, 4096).decode("utf-8", "replace"))
            sys.stdout.flush()
        except BlockingIOError:
            pass
