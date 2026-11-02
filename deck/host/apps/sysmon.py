#!/usr/bin/env python3
"""sysmon —— 桌面小屏「htop」的系统指标数据源（deck host 守护进程的一个 app）。

只用标准库：ctypes 直接调 libSystem / IOKit / CoreFoundation，不要 sudo，不碰 powermetrics。
poll() 每秒一次，产出一帧短键 JSON（v t chip ne c cpu gpu mp mu mt sw swt net dio df dp bat up ld top），
速率类的量（CPU%、网速、磁盘读写）是相邻两次 poll 的差分，第一次 poll 这些都是 0。

各项来源：
  c / cpu   host_processor_info 的每核 tick。哪几个核是 E 核看 IODeviceTree:/cpus/cpuN 的
            cluster-type（M1 Max 上是 cpu0-1：taskpolicy -b 的忙循环只会压满这两个核），
            输出时 E 核排在前面
  gpu       IOAccelerator 的 PerformanceStatistics["Device Utilization %"]
  mp        100 - kern.memorystatus_level
  mu / mt   host_statistics64：App(internal - purgeable) + wired + compressed，即活动监视器的「已使用内存」
  sw / swt  vm.swapusage
  net       net.link.generic.ifdata.<idx>.general 的 64 位计数（路由表 NET_RT_IFLIST2 给非 root 的
            字节数被截成 32 位、按 KiB 取整）。只算物理网口（en* / awdl*）：utun、bridge、vmenet
            的流量在 en0 上还会再走一遍，算进来就重复了
  dio       IOBlockStorageDriver 的 Statistics，跳过磁盘映像（读 DMG 时底下的 SSD 也在读）
  df / dp   statvfs(/System/Volumes/Data)，GiB，和 df -h 一致
  bat       AppleSmartBattery；没有电池的 Mac 不出这个键
  top       按 app 合并的进程 CPU / 内存，见 _ProcTable

用法：python3 sysmon.py [--n N] [--demo]
"""

from __future__ import annotations

import ctypes
import errno
import json
import os
import random
import re
import socket
import struct
import subprocess
import sys
import time
from ctypes import (POINTER, Structure, byref, c_char_p, c_double, c_int, c_int64, c_long,
                    c_size_t, c_uint, c_uint8, c_uint32, c_uint64, c_ulong, c_void_p, sizeof)

GiB = 1 << 30
MiB = 1 << 20

TOP_N = 6           # top 列表行数
NAME_LEN = 22       # top 里 app 名的最大字符数（板子再按像素宽度截）
MAX_CORES = 24      # c 数组最多这么多项，核更多的芯片同类相邻核取平均
PS_EVERY = 2.0      # 其他用户的进程靠 ps 补数（一次 ~25 ms），隔这么久跑一次
BAT_EVERY = 10.0    # 电池、磁盘剩余空间变化慢，缓存
DF_EVERY = 30.0

# ---------------------------------------------------------------- ctypes 绑定

if sys.platform == "darwin":
    _c = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
    _io = ctypes.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
    _cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")

    def _sig(fn, restype, *argtypes):
        fn.restype, fn.argtypes = restype, argtypes

    _sig(_c.sysctlbyname, c_int, c_char_p, c_void_p, POINTER(c_size_t), c_void_p, c_size_t)
    _sig(_c.sysctl, c_int, POINTER(c_int), c_uint, c_void_p, POINTER(c_size_t), c_void_p, c_size_t)
    _sig(_c.mach_host_self, c_uint)
    _sig(_c.mach_timebase_info, c_int, c_void_p)
    _sig(_c.host_processor_info, c_int, c_uint, c_int, POINTER(c_uint),
         POINTER(POINTER(c_int)), POINTER(c_uint))
    _sig(_c.host_statistics64, c_int, c_uint, c_int, c_void_p, POINTER(c_uint))
    _sig(_c.vm_deallocate, c_int, c_uint, c_size_t, c_size_t)
    _sig(_c.proc_listallpids, c_int, c_void_p, c_int)
    _sig(_c.proc_pid_rusage, c_int, c_int, c_int, c_void_p)
    _sig(_c.proc_pidpath, c_int, c_int, c_char_p, c_uint32)
    _sig(_c.proc_name, c_int, c_int, c_char_p, c_uint32)

    _sig(_io.IOServiceMatching, c_void_p, c_char_p)
    _sig(_io.IOServiceGetMatchingServices, c_int, c_uint, c_void_p, POINTER(c_uint))
    _sig(_io.IOIteratorNext, c_uint, c_uint)
    _sig(_io.IOObjectRelease, c_int, c_uint)
    _sig(_io.IORegistryEntryFromPath, c_uint, c_uint, c_char_p)
    _sig(_io.IORegistryEntryGetChildIterator, c_int, c_uint, c_char_p, POINTER(c_uint))
    _sig(_io.IORegistryEntryGetParentEntry, c_int, c_uint, c_char_p, POINTER(c_uint))
    _sig(_io.IORegistryEntryGetRegistryEntryID, c_int, c_uint, POINTER(c_uint64))
    _sig(_io.IORegistryEntryCreateCFProperty, c_void_p, c_uint, c_void_p, c_void_p, c_uint)

    _sig(_cf.CFStringCreateWithCString, c_void_p, c_void_p, c_char_p, c_uint32)
    _sig(_cf.CFRelease, None, c_void_p)
    _sig(_cf.CFGetTypeID, c_ulong, c_void_p)
    _sig(_cf.CFNumberIsFloatType, c_uint8, c_void_p)
    _sig(_cf.CFNumberGetValue, c_uint8, c_void_p, c_long, c_void_p)
    _sig(_cf.CFBooleanGetValue, c_uint8, c_void_p)
    _sig(_cf.CFStringGetLength, c_long, c_void_p)
    _sig(_cf.CFStringGetMaximumSizeForEncoding, c_long, c_long, c_uint32)
    _sig(_cf.CFStringGetCString, c_uint8, c_void_p, c_char_p, c_long, c_uint32)
    _sig(_cf.CFDataGetLength, c_long, c_void_p)
    _sig(_cf.CFDataGetBytePtr, c_void_p, c_void_p)
    _sig(_cf.CFDictionaryGetCount, c_long, c_void_p)
    _sig(_cf.CFDictionaryGetKeysAndValues, None, c_void_p, c_void_p, c_void_p)
    _sig(_cf.CFArrayGetCount, c_long, c_void_p)
    _sig(_cf.CFArrayGetValueAtIndex, c_void_p, c_void_p, c_long)
    for _f in ("CFNumberGetTypeID", "CFBooleanGetTypeID", "CFStringGetTypeID",
               "CFDataGetTypeID", "CFDictionaryGetTypeID", "CFArrayGetTypeID"):
        _sig(getattr(_cf, _f), c_ulong)
    _T_NUM, _T_BOOL, _T_STR = _cf.CFNumberGetTypeID(), _cf.CFBooleanGetTypeID(), _cf.CFStringGetTypeID()
    _T_DATA, _T_DICT, _T_ARR = _cf.CFDataGetTypeID(), _cf.CFDictionaryGetTypeID(), _cf.CFArrayGetTypeID()

_UTF8 = 0x08000100              # kCFStringEncodingUTF8
_CF_SINT64, _CF_DOUBLE = 4, 13  # kCFNumberSInt64Type / kCFNumberDoubleType


# ---------------------------------------------------------------- sysctl


def _sysctl(name: str) -> bytes:
    key, size = name.encode(), c_size_t(0)
    if _c.sysctlbyname(key, None, byref(size), None, 0) != 0:
        raise OSError(ctypes.get_errno(), f"sysctl {name}")
    buf = ctypes.create_string_buffer(size.value)
    if _c.sysctlbyname(key, buf, byref(size), None, 0) != 0:
        raise OSError(ctypes.get_errno(), f"sysctl {name}")
    return buf.raw[:size.value]


def _sysctl_int(name: str, default: int | None = None) -> int:
    try:
        return int.from_bytes(_sysctl(name), "little", signed=True)
    except OSError:
        if default is None:
            raise
        return default


def _sysctl_str(name: str) -> str:
    return _sysctl(name).split(b"\0", 1)[0].decode()


# ---------------------------------------------------------------- IOKit / CoreFoundation

_cfkeys: dict[str, int] = {}


def _cfstr(s: str) -> int:
    """缓存的 CFString（进程生命周期内不释放，就那么几个键）。"""
    ref = _cfkeys.get(s)
    if ref is None:
        ref = _cfkeys[s] = _cf.CFStringCreateWithCString(None, s.encode(), _UTF8)
    return ref


def _cf_py(ref):
    """CF 对象 → Python 值（只认 IORegistry 里会出现的几种类型）。不负责释放 ref。"""
    if not ref:
        return None
    tid = _cf.CFGetTypeID(ref)
    if tid == _T_NUM:
        if _cf.CFNumberIsFloatType(ref):
            d = c_double()
            _cf.CFNumberGetValue(ref, _CF_DOUBLE, byref(d))
            return d.value
        i = c_int64()  # 按有符号取：电池的 InstantAmperage 放电时是负数（ioreg 显示成 1844674407370955xxxx）
        _cf.CFNumberGetValue(ref, _CF_SINT64, byref(i))
        return i.value
    if tid == _T_BOOL:
        return bool(_cf.CFBooleanGetValue(ref))
    if tid == _T_STR:
        n = _cf.CFStringGetMaximumSizeForEncoding(_cf.CFStringGetLength(ref), _UTF8) + 1
        buf = ctypes.create_string_buffer(n)
        _cf.CFStringGetCString(ref, buf, n, _UTF8)
        return buf.value.decode("utf-8", "replace")
    if tid == _T_DATA:
        return ctypes.string_at(_cf.CFDataGetBytePtr(ref), _cf.CFDataGetLength(ref))
    if tid == _T_DICT:
        n = _cf.CFDictionaryGetCount(ref)
        keys, vals = (c_void_p * n)(), (c_void_p * n)()
        _cf.CFDictionaryGetKeysAndValues(ref, keys, vals)
        return {_cf_py(k): _cf_py(v) for k, v in zip(keys, vals)}
    if tid == _T_ARR:
        return [_cf_py(_cf.CFArrayGetValueAtIndex(ref, i)) for i in range(_cf.CFArrayGetCount(ref))]
    return None


def _io_prop(entry: int, key: str):
    ref = _io.IORegistryEntryCreateCFProperty(entry, _cfstr(key), None, 0)
    if not ref:
        return None
    try:
        return _cf_py(ref)
    finally:
        _cf.CFRelease(ref)


def _io_iter(it: c_uint):
    """遍历 io_iterator_t，每个对象和迭代器本身用完即释放。"""
    try:
        while obj := _io.IOIteratorNext(it):
            try:
                yield obj
            finally:
                _io.IOObjectRelease(obj)
    finally:
        _io.IOObjectRelease(it)


def _io_services(cls: str):
    it = c_uint()
    if _io.IOServiceGetMatchingServices(0, _io.IOServiceMatching(cls.encode()), byref(it)) != 0:
        return iter(())
    return _io_iter(it)


# ---------------------------------------------------------------- CPU


def _cluster_types() -> dict[int, str]:
    """IODeviceTree:/cpus 下每个 cpuN 的 {logical-cpu-id: "E"/"P"}。"""
    root = _io.IORegistryEntryFromPath(0, b"IODeviceTree:/cpus")
    if not root:
        return {}
    out: dict[int, str] = {}
    try:
        it = c_uint()
        if _io.IORegistryEntryGetChildIterator(root, b"IODeviceTree", byref(it)) != 0:
            return {}
        for k, cpu in enumerate(_io_iter(it)):
            ct, lid = _io_prop(cpu, "cluster-type"), _io_prop(cpu, "logical-cpu-id")
            if isinstance(ct, bytes) and ct:
                out[lid if isinstance(lid, int) else k] = ct[:1].decode()
    finally:
        _io.IOObjectRelease(root)
    return out


def _squeeze(vals: list[int], ne: int, limit: int = MAX_CORES) -> tuple[list[int], int]:
    """核数超过 limit 时，E / P 各自把相邻 k 个核取平均，保证「前 ne 个是 E 核」的含义不变。"""
    k = 1
    while True:
        e = [round(sum(vals[i:i + k]) / len(vals[i:i + k])) for i in range(0, ne, k)]
        p = [round(sum(vals[i:i + k]) / len(vals[i:i + k])) for i in range(ne, len(vals), k)]
        if len(e) + len(p) <= limit:
            return e + p, len(e)
        k += 1


class _CPU:
    """每核 tick（user, system, idle, nice）差分 → 忙碌 %。"""

    def __init__(self, host: int):
        self.host = host
        self.task = c_uint.in_dll(_c, "mach_task_self_").value
        self.prev: list[int] | None = None
        self.n = len(self._ticks()) // 4
        # 核数以 sysctl 为准（perflevel1 不存在 = 没有 E 核），哪几个编号是 E 核以设备树为准；
        # 设备树读不到或对不上，就假定 E 核是编号最小的那几个（M1/M2/M3 都是这样）
        ne = _sysctl_int("hw.perflevel1.logicalcpu", 0) if _sysctl_int("hw.nperflevels", 1) >= 2 else 0
        ne = max(0, min(ne, self.n))
        try:
            types = _cluster_types()
        except Exception:
            types = {}
        e_ids = sorted(i for i, t in types.items() if t == "E")
        if len(types) != self.n or len(e_ids) != ne or any(i >= self.n for i in types):
            e_ids = list(range(ne))
        self.order = e_ids + [i for i in range(self.n) if i not in e_ids]
        self.ne_raw = ne
        self.ne = _squeeze([0] * self.n, ne)[1]

    def _ticks(self) -> list[int]:
        n, arr, cnt = c_uint(), POINTER(c_int)(), c_uint()
        kr = _c.host_processor_info(self.host, 2, byref(n), byref(arr), byref(cnt))  # PROCESSOR_CPU_LOAD_INFO
        if kr != 0:
            raise OSError(kr, "host_processor_info")
        try:
            return arr[:n.value * 4]
        finally:
            _c.vm_deallocate(self.task, ctypes.cast(arr, c_void_p).value, cnt.value * sizeof(c_int))

    def sample(self) -> tuple[list[int], int]:
        """(每核忙碌 %：E 核在前、最多 24 项, 全部核平均忙碌 %)。第一次调用全 0。"""
        cur = self._ticks()
        prev, self.prev = self.prev, cur
        if prev is None or len(prev) != len(cur):
            return _squeeze([0] * self.n, self.ne_raw)[0], 0
        per, busy_all, total_all = [], 0, 0
        for i in range(self.n):
            # tick 是 natural_t（32 位无符号，会回绕），读出来又是 int，按 32 位取差
            d = [(cur[i * 4 + j] - prev[i * 4 + j]) & 0xFFFFFFFF for j in range(4)]
            busy, total = d[0] + d[1] + d[3], d[0] + d[1] + d[2] + d[3]
            per.append(round(100 * busy / total) if total else 0)
            busy_all += busy
            total_all += total
        cores, _ = _squeeze([per[i] for i in self.order], self.ne_raw)
        return cores, round(100 * busy_all / total_all) if total_all else 0


# ---------------------------------------------------------------- 内存 / 交换


class _VMStat64(Structure):
    _fields_ = [(n, c_uint32) for n in ("free", "active", "inactive", "wire")] + \
               [(n, c_uint64) for n in ("zero_fill", "reactivations", "pageins", "pageouts", "faults",
                                        "cow_faults", "lookups", "hits", "purges")] + \
               [(n, c_uint32) for n in ("purgeable", "speculative")] + \
               [(n, c_uint64) for n in ("decompressions", "compressions", "swapins", "swapouts")] + \
               [(n, c_uint32) for n in ("compressor", "throttled", "external", "internal")] + \
               [(n, c_uint64) for n in ("total_uncompressed_in_compressor", "swapped")]


def _memory_used(host: int, page: int) -> int:
    """活动监视器的「已使用内存」= App 内存(internal - purgeable) + 联动(wired) + 被压缩，字节。"""
    vs = _VMStat64()
    cnt = c_uint(sizeof(vs) // 4)
    kr = _c.host_statistics64(host, 4, byref(vs), byref(cnt))  # HOST_VM_INFO64
    if kr != 0:
        raise OSError(kr, "host_statistics64")
    return (vs.internal - vs.purgeable + vs.wire + vs.compressor) * page


def _swap() -> tuple[float, float]:
    total, _avail, used = struct.unpack_from("<QQQ", _sysctl("vm.swapusage"))  # struct xsw_usage
    return round(used / GiB, 1), round(total / GiB, 1)


# ---------------------------------------------------------------- 网络 / 磁盘 / GPU / 电池

_IFF_UP, _IFF_LOOPBACK, _IFT_ETHER = 0x1, 0x8, 0x06
# 以太网类型但不是「出这台机器的物理流量」：网桥成员 / 虚拟机网卡 / 低延迟 WLAN / 内部网卡 / 热点
_IF_SKIP = ("bridge", "vmenet", "llw", "anpi", "ap", "feth")


class _Net:
    """物理网口累计字节（IFMIB：struct ifmibdata，里面的 if_data64 按 4 字节对齐，从偏移 52 开始）。"""

    def __init__(self):
        self.buf = ctypes.create_string_buffer(512)
        self.mib = (c_int * 6)(4, 18, 0, 2, 0, 1)  # CTL_NET, PF_LINK, NETLINK_GENERIC, IFMIB_IFDATA, idx, IFDATA_GENERAL

    def counters(self) -> dict[str, tuple[int, int]]:
        """{网口名: (发送, 接收) 累计字节}。"""
        out = {}
        for idx, name in socket.if_nameindex():
            if name.startswith(_IF_SKIP):
                continue
            self.mib[4] = idx
            size = c_size_t(len(self.buf))
            if _c.sysctl(self.mib, 6, self.buf, byref(size), None, 0) != 0 or size.value < 132:
                continue
            raw = ctypes.string_at(self.buf, size.value)
            flags = struct.unpack_from("<i", raw, 20)[0]  # ifmd_flags
            if raw[52] != _IFT_ETHER or not flags & _IFF_UP or flags & _IFF_LOOPBACK:
                continue  # utun / gif / stf / lo / bridge 都不是 IFT_ETHER
            ib, ob = struct.unpack_from("<QQ", raw, 52 + 64)  # if_data64.ifi_ibytes / ifi_obytes
            out[name] = (ob, ib)
        return out


def _is_disk_image(driver: int) -> bool:
    parent = c_uint()
    if _io.IORegistryEntryGetParentEntry(driver, b"IOService", byref(parent)) != 0:
        return False
    try:
        pc = _io_prop(parent, "Protocol Characteristics")
    finally:
        _io.IOObjectRelease(parent)
    return isinstance(pc, dict) and pc.get("Physical Interconnect") == "Virtual Interface"


def _disk_counters() -> dict[int, tuple[int, int]]:
    """{registry entry id: (读, 写) 累计字节}，磁盘映像除外。"""
    out = {}
    for drv in _io_services("IOBlockStorageDriver"):
        if _is_disk_image(drv):
            continue
        st, eid = _io_prop(drv, "Statistics"), c_uint64()
        if isinstance(st, dict) and _io.IORegistryEntryGetRegistryEntryID(drv, byref(eid)) == 0:
            out[eid.value] = (st.get("Bytes (Read)", 0), st.get("Bytes (Write)", 0))
    return out


def _gpu() -> int:
    best = -1
    for acc in _io_services("IOAccelerator"):
        st = _io_prop(acc, "PerformanceStatistics")
        if isinstance(st, dict) and isinstance(st.get("Device Utilization %"), int):
            best = max(best, st["Device Utilization %"])
    return min(best, 100)


def _battery() -> list | None:
    """[电量 %, 接着电源 1/0, 瓦数（充电为正、放电为负）]；没有电池返回 None。"""
    for bat in _io_services("AppleSmartBattery"):
        cur, cap = _io_prop(bat, "CurrentCapacity"), _io_prop(bat, "MaxCapacity")
        if not isinstance(cur, int):
            continue
        # Apple Silicon 上 MaxCapacity 就是 100；Intel 上两者都是 mAh
        pct = round(100 * cur / cap) if isinstance(cap, int) and cap > 0 else cur
        ac = 1 if _io_prop(bat, "ExternalConnected") else 0
        mv = _io_prop(bat, "Voltage") or 0
        ma = _io_prop(bat, "InstantAmperage")
        if ma is None:
            ma = _io_prop(bat, "Amperage") or 0
        if ma >= 1 << 63:
            ma -= 1 << 64
        return [max(0, min(100, pct)), ac, round(mv * ma / 1e6, 1)]
    return None


def _sys_power() -> float | None:
    """整机此刻的功耗（瓦），电池控制器报的 SystemPowerIn，不用 sudo；台式机没有就返回 None。"""
    for bat in _io_services("AppleSmartBattery"):
        tel = _io_prop(bat, "PowerTelemetryData")
        if isinstance(tel, dict) and isinstance(tel.get("SystemPowerIn"), int):
            return round(tel["SystemPowerIn"] / 1000, 1)
    return None


def _disk_free() -> tuple[int, int]:
    """(数据卷剩余 GiB, 已用 %)。已用 % 按整个容器算（系统卷、VM 卷也占地方），和 df 的 Capacity 一致。"""
    for path in ("/System/Volumes/Data", "/"):
        try:
            st = os.statvfs(path)
            break
        except OSError:
            continue
    else:
        raise OSError("statvfs")
    free, total = st.f_bavail * st.f_frsize, st.f_blocks * st.f_frsize
    return round(free / GiB), round(100 * (total - free) / total) if total else -1


# ---------------------------------------------------------------- 进程 → app 分组

AGENTS = {"claude", "codex", "gemini", "opencode", "aider", "grok", "cursor-agent", "crush", "amp"}
# node 直接跑 npm 包里的 js 时，文件名看不出是谁，看包名
_AGENT_PKGS = {"claude-code": "claude", "codex": "codex", "gemini-cli": "gemini", "amp": "amp",
               "opencode-ai": "opencode", "grok-cli": "grok", "cursor-agent": "cursor-agent"}
_INTERP = re.compile(r"node|bun|deno|ruby|[Pp]ython[\d.]*")
_VERSIONED = re.compile(r"\d+(\.\d+)+")   # Claude Code 原生安装：~/.local/share/claude/versions/2.1.316
_NPM_PKG = re.compile(r"/node_modules/(?:@[^/]+/)?([^/]+)/")
_PY_FW = re.compile(r"/Python\.framework/Versions/(\d+\.\d+)/")


def _agent_of(word: str) -> str:
    stem = os.path.basename(word)
    for ext in (".js", ".mjs", ".cjs"):
        stem = stem.removesuffix(ext)
    if stem in AGENTS:
        return stem
    return "codex" if stem.startswith("codex-") else ""  # codex-code-mode-host 之类的辅助进程


def _outer_app(path: str) -> str:
    """路径里最外层的 X.app → "X"。.framework 里面的 .app 不算（比如 Python.framework 里的 Python.app）。"""
    for part in path.split("/"):
        if part.endswith(".framework"):
            return ""
        if part.endswith(".app") and len(part) > 4:
            return part[:-4]
    return ""


def _classify(path: str, argv) -> tuple[str, str]:
    """可执行路径 → (分组名, agent 种类)。argv 是惰性的，只有解释器 / 版本号命名的可执行文件才去读参数。

    agent 优先于 .app 分组（比如 Codex.app 里带的 codex 也算 codex）；不在 .app 里的进程用可执行文件名。
    """
    base = os.path.basename(path)
    kind = _agent_of(base) or ("claude" if "/claude/versions/" in path else "")
    if not kind and (_INTERP.fullmatch(base) or _VERSIONED.fullmatch(base)):
        args = argv()
        script = next((a for a in args[1:] if not a.startswith("-")), "")
        kind = (_agent_of(args[0]) if args else "") or _agent_of(script)
        if not kind and (m := _NPM_PKG.search(script)):
            kind = _AGENT_PKGS.get(m.group(1), "")
    if kind:
        return kind, kind
    if app := _outer_app(path):
        if not app.isascii():  # 板子字库没有中文：「网易云音乐.app」退回它的可执行文件名
            m = re.search(re.escape(app) + r"\.app/Contents/MacOS/([^/]+)", path)
            exe = m.group(1) if m else base
            app = exe if exe.isascii() else app
        return app, ""
    if base == "Python" and (m := _PY_FW.search(path)):
        return "python" + m.group(1), ""  # Homebrew / python.org 的框架版 Python 实际跑的是 Python.app
    return base.removeprefix("com.apple."), ""


def _cputime(s: str) -> float:
    """ps 的 time 列（"2997:31.88"，可能带 "天-" 前缀）→ 秒。"""
    days, _, s = s.rpartition("-")
    secs = 0.0
    for part in s.split(":"):
        secs = secs * 60 + float(part)
    return (int(days) if days else 0) * 86400 + secs


class _RUsageV0(Structure):  # struct rusage_info_v0
    _fields_ = [("uuid", c_uint8 * 16)] + \
               [(n, c_uint64) for n in ("user_time", "system_time", "pkg_idle_wkups", "interrupt_wkups",
                                        "pageins", "wired_size", "resident_size", "phys_footprint",
                                        "proc_start_abstime", "proc_exit_abstime")]


class _TimeBase(Structure):
    _fields_ = [("numer", c_uint32), ("denom", c_uint32)]


class _ProcTable:
    """进程 → 按 app 合并的 [{"n","c","m","k"}]，CPU% 降序取前 TOP_N。

    自己用户的进程走 proc_pid_rusage：CPU 时间是 mach absolute time 单位（M1 上 ×125/3 才是 ns），
    内存取 phys_footprint（活动监视器「内存」列的口径；直接加 RSS 会把几十个 helper 共享的页重复算）。
    其他用户的进程（root、_windowserver、_spotlight……）libproc 一律 EPERM，用 setuid 的 ps 补
    CPU 时间和 RSS，每 PS_EVERY 秒跑一次，中间沿用上次的速率。kernel_task（pid 0）连 ps 都看不到。
    pid → (路径, 分组) 缓存；启动时刻变了（pid 复用）就重新分组，有 CPU 活动的进程顺手查一下路径
    有没有变（exec），其余进程不重复调 proc_pidpath。
    """

    def __init__(self):
        tb = _TimeBase()
        _c.mach_timebase_info(byref(tb))
        self.ns_per_unit = tb.numer / tb.denom
        self.pidbuf = (c_int * 4096)()
        self.ru = _RUsageV0()
        self.pathbuf = ctypes.create_string_buffer(4096)
        self.argbuf = None
        self.t: float | None = None
        self.cpu: dict[int, tuple[int, int]] = {}         # pid -> (启动时刻, 累计 CPU)
        self.info: dict[int, tuple[str, str, str]] = {}   # pid -> (路径, 分组名, agent 种类)
        self.denied: set[int] = set()                     # libproc 读不了的 pid（别的用户）
        self.ps: dict[int, tuple[float, int]] = {}        # pid -> (累计 CPU 秒, RSS 字节)
        self.ps_rate: dict[int, float] = {}               # pid -> CPU%
        self.ps_t: float | None = None
        self.ps_runs = 0

    # -- 采样

    def sample(self) -> list[dict]:
        now = time.monotonic()
        dt = now - self.t if self.t is not None else 0.0
        self.t = now
        scale = 100 * self.ns_per_unit / (dt * 1e9) if dt > 0 else 0.0
        pids = self._pids()
        groups: dict[str, list] = {}  # 分组名 -> [CPU%, 内存字节, agent 种类]
        others: list[int] = []
        cpu: dict[int, tuple[int, int]] = {}
        ru, ru_ref, rusage = self.ru, byref(self.ru), _c.proc_pid_rusage
        for pid in pids:
            if pid <= 0:
                continue
            if pid in self.denied:
                others.append(pid)
                continue
            if rusage(pid, 0, ru_ref) != 0:  # RUSAGE_INFO_V0
                if ctypes.get_errno() == errno.EPERM:
                    self.denied.add(pid)
                    others.append(pid)
                continue  # ESRCH：刚退出
            start, total = ru.proc_start_abstime, ru.user_time + ru.system_time
            prev = self.cpu.get(pid)
            cpu[pid] = (start, total)
            if prev is not None and prev[0] == start:
                d = total - prev[1]
                info = self._info(pid, recheck=d > 0)
            else:
                d = 0
                info = self._info(pid, fresh=True)
            if info:
                self._add(groups, info, d * scale, ru.phys_footprint)
        self.cpu = cpu

        if others:
            self._run_ps(now)
            for pid in others:
                info = self._info(pid)
                if info:
                    self._add(groups, info, self.ps_rate.get(pid, 0.0), self.ps.get(pid, (0, 0))[1])

        alive = set(pids)
        for d in (self.info, self.ps, self.ps_rate):
            for pid in [p for p in d if p not in alive]:
                del d[pid]
        self.denied &= alive

        rows = sorted(groups.items(), key=lambda kv: (-kv[1][0], -kv[1][1]))[:TOP_N]
        return [{"n": name[:NAME_LEN], "c": round(c), "m": round(m / MiB), "k": k}
                for name, (c, m, k) in rows]

    @staticmethod
    def _add(groups: dict, info: tuple, cpu: float, mem: int):
        g = groups.get(info[1])
        if g is None:
            groups[info[1]] = [cpu, mem, info[2]]
        else:
            g[0] += cpu
            g[1] += mem

    def _pids(self) -> list[int]:
        while True:
            n = _c.proc_listallpids(self.pidbuf, sizeof(self.pidbuf))
            if n < 0:
                raise OSError(ctypes.get_errno(), "proc_listallpids")
            if n < len(self.pidbuf):
                return self.pidbuf[:n]
            self.pidbuf = (c_int * (len(self.pidbuf) * 2))()

    def _run_ps(self, now: float):
        """ps -axo pid,time,rss，算出其他用户进程的 CPU%。前两次连着跑（好尽快有速率），之后每 PS_EVERY 秒。"""
        if self.ps_runs >= 2 and now - self.ps_t < PS_EVERY - 0.05:
            return
        try:
            # 绝对路径：launchd 下 PATH 很短
            out = subprocess.run(["/bin/ps", "-axo", "pid=,time=,rss="], capture_output=True,
                                 text=True, timeout=5).stdout
        except (OSError, subprocess.SubprocessError):
            return
        cur: dict[int, tuple[float, int]] = {}
        for line in out.splitlines():
            f = line.split()
            try:
                if len(f) == 3 and (pid := int(f[0])) in self.denied:
                    cur[pid] = (_cputime(f[1]), int(f[2]) * 1024)
            except ValueError:
                pass
        dt = now - self.ps_t if self.ps_t is not None else 0.0
        rate: dict[int, float] = {}
        if dt > 0:
            for pid, (t, _) in cur.items():
                old = self.ps.get(pid)
                if old is not None and t >= old[0]:
                    rate[pid] = 100 * (t - old[0]) / dt
        self.ps, self.ps_rate, self.ps_t = cur, rate, now
        self.ps_runs += 1

    # -- 分组

    def _info(self, pid: int, fresh: bool = False, recheck: bool = False):
        """pid 的 (路径, 分组名, 种类)。fresh=新进程/pid 复用，必重新分组；recheck=路径变了才重新分组。"""
        old = self.info.get(pid)
        if old is not None and not fresh and not recheck:
            return old
        path = self._path(pid)
        if path is None:
            return old
        if old is not None and not fresh and old[0] == path:
            return old
        name, kind = _classify(path, lambda: self._argv(pid))
        self.info[pid] = info = (path, name, kind)
        return info

    def _path(self, pid: int) -> str | None:
        if _c.proc_pidpath(pid, self.pathbuf, len(self.pathbuf)) > 0:
            return self.pathbuf.value.decode("utf-8", "replace")
        name = ctypes.create_string_buffer(64)  # 拿不到路径就退回 p_comm
        if _c.proc_name(pid, name, len(name)) > 0 and name.value:
            return name.value.decode("utf-8", "replace")
        return None

    def _argv(self, pid: int) -> list[str]:
        """KERN_PROCARGS2 里的 argv（最多前 4 个）。

        这块内存后半截是环境变量，里面常有 API key：只解析 argc 个参数，绝不整段打印或缓存。
        缓冲区必须给够 kern.argmax，给小了内核不报错、只返回一串 0。
        """
        if self.argbuf is None:
            self.argbuf = ctypes.create_string_buffer(_sysctl_int("kern.argmax", 1 << 20))
        mib = (c_int * 3)(1, 49, pid)  # CTL_KERN, KERN_PROCARGS2
        size = c_size_t(len(self.argbuf))
        if _c.sysctl(mib, 3, self.argbuf, byref(size), None, 0) != 0 or size.value < 8:
            return []
        # 布局：argc | exec 路径 \0 | 若干 \0 对齐 | argv[0] \0 argv[1] \0 ... | 环境变量
        raw = ctypes.string_at(self.argbuf, min(size.value, 64 * 1024))
        argc = struct.unpack_from("<i", raw)[0]
        i = raw.find(b"\0", 4)
        if i < 0:
            return []
        while i < len(raw) and raw[i] == 0:
            i += 1
        args = []
        for _ in range(max(0, min(argc, 4))):
            j = raw.find(b"\0", i)
            if j < 0:
                break
            args.append(raw[i:j].decode("utf-8", "replace"))
            i = j + 1
        return args


# ---------------------------------------------------------------- app


class SysApp:
    """deck 的系统状态 app：poll() 一帧真数据，demo() 一帧假数据，summary() 一行日志。"""

    id = "sys"
    interval = 1.0

    def __init__(self):
        self._ready = False   # 原生部分第一次 poll 才初始化，--demo 不碰系统
        self._prev: dict[str, tuple[float, tuple[int, int]]] = {}
        self._cache: dict[str, tuple[float, object]] = {}
        self.errors: dict[str, str] = {}  # 本轮出错的指标 → 异常文本（main 里打印，守护进程可以记日志）

    def _init(self):
        self._host = _c.mach_host_self()
        self._cpu = _CPU(self._host)
        self._procs = _ProcTable()
        self._net = _Net()
        self._page = _sysctl_int("hw.pagesize")
        self._mem_total = _sysctl_int("hw.memsize")
        self._boot = struct.unpack_from("<q", _sysctl("kern.boottime"))[0]  # struct timeval.tv_sec
        try:
            self._chip = _sysctl_str("machdep.cpu.brand_string").removeprefix("Apple ")
        except OSError:
            self._chip = ""
        self._ready = True

    # -- 小工具

    def _try(self, key: str, fn, default):
        try:
            return fn()
        except Exception as e:  # 单项失败用哨兵值顶上，其余照常出帧
            self.errors[key] = f"{type(e).__name__}: {e}"
            return default

    def _slow(self, key: str, period: float, fn):
        now = time.monotonic()
        hit = self._cache.get(key)
        if hit is None or now - hit[0] >= period:
            hit = self._cache[key] = (now, self._try(key, fn, None))
        return hit[1]

    def _rate(self, key: str, cur: dict) -> list[int]:
        """{设备: (a, b) 累计字节} → [a/s, b/s]。

        按设备分别差分：只算前后两次都在的设备（新插上的网卡 / 硬盘不会带着历史计数冲出一个尖峰，
        拔掉的也不会让总数变小），单个设备计数变小（重置）记 0。
        """
        now = time.monotonic()
        prev, self._prev[key] = self._prev.get(key), (now, cur)
        if prev is None or now <= prev[0]:
            return [0, 0]
        a = b = 0
        for dev, (x, y) in cur.items():
            if (old := prev[1].get(dev)) is not None:
                a += max(0, x - old[0])
                b += max(0, y - old[1])
        dt = now - prev[0]
        return [round(a / dt), round(b / dt)]

    # -- 接口

    def poll(self) -> dict:
        if not self._ready:
            self._init()  # 失败就抛，这一轮算失败，下一轮重来
        self.errors = {}
        f: dict = {"v": 1, "t": time.strftime("%H:%M"), "chip": self._chip, "ne": self._cpu.ne}
        f["c"], f["cpu"] = self._try("cpu", self._cpu.sample, ([], -1))
        f["gpu"] = self._try("gpu", _gpu, -1)
        f["mp"] = self._try("mp", lambda: max(0, min(100, 100 - _sysctl_int("kern.memorystatus_level"))), -1)
        f["mu"] = self._try("mu", lambda: round(_memory_used(self._host, self._page) / GiB, 1), -1)
        mt = round(self._mem_total / GiB, 1)
        f["mt"] = int(mt) if mt.is_integer() else mt
        f["sw"], f["swt"] = self._try("sw", _swap, (-1, -1))
        f["net"] = self._try("net", lambda: self._rate("net", self._net.counters()), [0, 0])
        f["dio"] = self._try("dio", lambda: self._rate("dio", _disk_counters()), [0, 0])
        f["df"], f["dp"] = self._slow("df", DF_EVERY, _disk_free) or (-1, -1)
        bat = self._slow("bat", BAT_EVERY, _battery)
        if bat:
            f["bat"] = bat
            pw = self._slow("pw", 2.0, _sys_power)
            if pw is not None:
                f["pw"] = pw
        f["up"] = max(0, int(time.time()) - self._boot)
        f["ld"] = self._try("ld", lambda: round(os.getloadavg()[0], 1), -1)
        f["top"] = self._try("top", self._procs.sample, [])
        return f

    def demo(self) -> dict:
        r = random.Random()
        e = [r.randint(55, 95) for _ in range(2)]
        p = sorted((r.randint(0, 60) for _ in range(8)), reverse=True)
        cores = e + p
        top = [{"n": "WeChat", "c": r.randint(150, 230), "m": 1024, "k": ""},
               {"n": "Google Chrome", "c": r.randint(40, 90), "m": 3150, "k": ""},
               {"n": "WindowServer", "c": r.randint(20, 40), "m": 380, "k": ""},
               {"n": "claude", "c": r.randint(5, 30), "m": 410, "k": "claude"},
               {"n": "codex", "c": r.randint(1, 15), "m": 220, "k": "codex"},
               {"n": "python3.15", "c": r.randint(0, 5), "m": 35, "k": ""}]
        top.sort(key=lambda x: -x["c"])
        return {"v": 1, "t": time.strftime("%H:%M"), "chip": "M1 Max", "ne": 2, "c": cores,
                "cpu": round(sum(cores) / len(cores)), "gpu": r.randint(5, 60), "mp": 34,
                "mu": round(r.uniform(20, 23), 1), "mt": 32, "sw": 3.9, "swt": 5.0,
                "net": [r.randint(500, 50_000), r.randint(10_000, 20_000_000)],
                "dio": [r.randint(0, 2_000_000), r.randint(0, 800_000)],
                "df": 72, "dp": 92, "bat": [100, 1, 0.0], "up": 1_309_000,
                "ld": round(r.uniform(2, 9), 1), "top": top}

    def summary(self, frame: dict) -> str:
        s = f"cpu {frame.get('cpu', -1)}% gpu {frame.get('gpu', -1)}% mem {frame.get('mp', -1)}%"
        if top := frame.get("top"):
            s += f" top {top[0]['n']} {top[0]['c']}%"
        return s


# ---------------------------------------------------------------- 调试入口


def _main(argv: list[str] | None = None) -> None:
    import argparse
    ap = argparse.ArgumentParser(description="打印 SysApp 的帧：JSON 走 stdout，耗时 / 摘要走 stderr")
    ap.add_argument("--demo", action="store_true", help="打印 demo() 的假数据")
    ap.add_argument("--n", type=int, default=0, help="跑 N 次后退出（默认一直跑）")
    ap.add_argument("--interval", type=float, default=SysApp.interval)
    a = ap.parse_args(argv)
    app = SysApp()
    i = 0
    try:
        while True:
            t0 = time.perf_counter()
            frame = app.demo() if a.demo else app.poll()
            ms = (time.perf_counter() - t0) * 1000
            js = json.dumps(frame, ensure_ascii=False, separators=(",", ":"))
            print(js, flush=True)
            print(f"# {ms:.1f} ms, {len(js.encode())} B | {app.summary(frame)}", file=sys.stderr, flush=True)
            for k, err in app.errors.items():
                print(f"# {k} 失败: {err}", file=sys.stderr, flush=True)
            i += 1
            if a.n and i >= a.n:
                break
            time.sleep(max(0.0, a.interval - (time.perf_counter() - t0)))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    _main()
