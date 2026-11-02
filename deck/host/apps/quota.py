"""AI 用量：magpie 的订阅配额 + Claude 订阅额度，汇总成一帧。

（原 ai-subscription-display/host/bridge.py 的数据部分，串口和主循环交给 deck.py）

数据来源：
  - magpie 网关 http://127.0.0.1:3425/v1/magpie/quotas   (Codex / DeepSeek / Grok 的窗口和余额)
  - Claude：只读本地缓存，不跑 `claude -p /usage`、不发任何请求，见 ClaudeLimits
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from datetime import datetime, timedelta

MAGPIE = os.environ.get("MAGPIE_URL", "http://127.0.0.1:3425")

QUOTA_TTL = 10.0
ACCOUNT_TTL = 600.0

# Claude 套餐。magpie 只认得 max/pro，Ultra 会被报成 Max，所以这里以本地配置为准。
# Ultra 没有 5h 窗口（读数里仍有一个 five_hour，但没有意义），不显示。
CLAUDE_PLAN = os.environ.get("CLAUDE_PLAN", "Ultra")
CLAUDE_HAS_SESSION = CLAUDE_PLAN.lower() not in ("ultra",)

# 屏幕上从上到下的顺序
ORDER = ["claude", "codex", "deepseek", "grok"]
NAMES = {"claude": "Claude", "codex": "Codex", "deepseek": "DeepSeek", "grok": "Grok"}

# Claude 额度的两个本地缓存
MOD_CACHE = os.path.expanduser("~/Library/Caches/deck/claude-limits.json")
CLAUDE_JSON = os.path.expanduser("~/.claude.json")
OLD_AFTER = 3600  # 最新的读数比这还旧，就在卡片上注明是多久前的


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


class ClaudeLimits:
    """Claude 订阅额度。只读本地缓存，不跑 /usage、不发请求；两个来源，每个窗口取较新的那份：

    1. deck-usage mod（deck/claude-mod）写的 ~/Library/Caches/deck/claude-limits.json。
       Claude Code 每次 API 响应都带着 5h / 7d 窗口，窗口挪动一个整点 mod 就写一次，
       所以只要这台 Mac 上有 Claude Code 会话在干活，读数几乎是实时的。
    2. ~/.claude.json 里的 cachedUsageUtilization：Claude Code 跑 /usage 时存下的完整读数，
       只有它有按模型的周窗口（Fable）。你自己在会话里敲 /usage 时才会更新。

    没有会话在跑时额度本来也不会因为这台 Mac 变；重置时刻过了就当窗口已经清零。
    """

    def __init__(self) -> None:
        self._seen: dict[str, tuple[float, object]] = {}  # 路径 → (mtime, 解析结果)

    def _load(self, path: str):
        """文件没变就用上次解析的；正写到一半解析失败也用上次的。"""
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            return None
        old = self._seen.get(path)
        if old and old[0] == mtime:
            return old[1]
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError):
            return old[1] if old else None
        self._seen[path] = (mtime, data)
        return data

    def read(self) -> dict | None:
        """{"wins": {"5h"|"7d": (已用%, 重置时刻 ISO, 读数时刻)}, "scoped": [(模型名, 已用%, 重置时刻, 读数时刻)]}"""
        wins: dict[str, tuple[float, str | None, float]] = {}
        mod = self._load(MOD_CACHE) or {}
        at = (mod.get("at") or 0) / 1000
        for lim in mod.get("limits") or []:
            key = {"five_hour": "5h", "seven_day": "7d"}.get(lim.get("kind"))
            if key and lim.get("used") is not None:
                wins[key] = (float(lim["used"]), lim.get("resets_at"), at)

        cached = (self._load(CLAUDE_JSON) or {}).get("cachedUsageUtilization") or {}
        util = cached.get("utilization") or {}
        at = (cached.get("fetchedAtMs") or 0) / 1000
        for kind, key in (("five_hour", "5h"), ("seven_day", "7d")):
            w = util.get(kind)
            if isinstance(w, dict) and w.get("utilization") is not None and (key not in wins or wins[key][2] < at):
                wins[key] = (float(w["utilization"]), w.get("resets_at"), at)
        scoped = []
        for lim in util.get("limits") or []:
            name = (((lim.get("scope") or {}).get("model") or {}).get("display_name")) or ""
            if lim.get("kind") == "weekly_scoped" and name:
                scoped.append((name, float(lim.get("percent") or 0), lim.get("resets_at"), at))
        if not wins and not scoped:
            return None
        return {"wins": wins, "scoped": scoped}


def after_reset(used: float, iso: str | None, now: datetime) -> tuple[int, int]:
    """(已用%, 距重置秒数)。重置时刻已经过了：窗口清零，下次什么时候重置不知道。"""
    if not iso:
        return round(used), -1
    try:
        dt = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return round(used), -1
    left = int((dt - now).total_seconds())
    return (0, -1) if left <= 0 else (round(used), left)


def ago(secs: float) -> str:
    return f"{int(secs // 86400)}d ago" if secs >= 86400 else f"{int(secs // 3600)}h ago"


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
        wins = []
        for key in ("5h", "7d"):
            if key == "5h" and not CLAUDE_HAS_SESSION:
                continue
            if key in cc["wins"]:
                used, iso, _ = cc["wins"][key]
                wins.append(mkwin(key, *after_reset(used, iso, now), now))
        meta = ""
        if cc["scoped"]:
            name, used, iso, _ = cc["scoped"][0]
            meta = f"{name} {after_reset(used, iso, now)[0]}%"
        newest = max([w[2] for w in cc["wins"].values()] + [x[3] for x in cc["scoped"]])
        age = now.timestamp() - newest
        if age > OLD_AFTER:  # 不是实时的了，注明一下
            meta = f"{meta} \u00b7 {ago(age)}" if meta else ago(age)
        cards.append({"id": "claude", "n": NAMES["claude"],
                      "s": CLAUDE_PLAN or plans.get("claude", ""), "w": wins, "m": meta})

    by_id: dict[str, dict] = {}
    for q in quotas:
        pid = q.get("provider") or ""
        if pid == "claude":
            continue  # 用本地缓存的读数，magpie 这边读不到 Claude 窗口
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

DEMO = {"v": 1, "t": "17:19:28", "p": [
    {"id": "claude", "n": "Claude", "s": "Ultra", "w": [{"k": "7d", "u": 34, "x": 20371, "r": "Tue 09:00"}],
     "m": "Fable 10%"},
    {"id": "codex", "n": "Codex", "s": "Pro", "w": [{"k": "5h", "u": 12, "x": 9000, "r": "at 19:49"},
                                                    {"k": "7d", "u": 27, "x": 249503, "r": "Wed 14:37"}],
     "m": "62,168 credits"},
    {"id": "deepseek", "n": "DeepSeek", "s": "", "w": [], "m": "¥76.24"},
    {"id": "grok", "n": "Grok", "s": "SuperGrok", "w": [{"k": "7d", "u": 100, "x": 154434, "r": "Tue 12:13"}],
     "m": ""}]}


class QuotaApp:
    id = "quota"
    interval = 2.0

    def __init__(self) -> None:
        self.quotas: list[dict] = []
        self.plans: dict[str, str] = {}
        self.claude = ClaudeLimits()
        self.t_quota = self.t_plan = -1e9

    def poll(self) -> dict:
        started = time.monotonic()
        now = datetime.now().astimezone()
        if started - self.t_quota >= QUOTA_TTL:
            self.quotas = fetch_quotas()
            self.t_quota = started
        if started - self.t_plan >= ACCOUNT_TTL:
            self.plans = fetch_account_plans()
            self.t_plan = started
        return build_payload(self.quotas, self.claude.read(), self.plans, now)

    def demo(self) -> dict:
        return {**DEMO, "t": time.strftime("%H:%M:%S")}

    def summary(self, frame: dict) -> str:
        parts = []
        for c in frame["p"]:
            left = min((100 - w["u"] for w in c["w"]), default=None)
            parts.append(f"{c['n']}:{left}%" if left is not None else f"{c['n']}:{c['m'] or '--'}")
        return " ".join(parts) or "(空)"
