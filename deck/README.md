# deck：我的桌面小面板（立创·实战派 ESP32-S3）

2 寸屏上的一个「桌面」：首页是一排磁贴，每个磁贴是一个小应用，点进去全屏看，左右划切换。
之前的三个项目都变成了这里的应用，再加一个新写的系统监控。**一套固件、一个主机进程**，
不用再为了换项目重新烧录。

```
 15:43  MON 02 NOV                    ● XINGYI-MBP16
┌ SYSTEM ──────────────┐ ┌ MAGPIE ──────────────┐
│ 34 % CPU   ▮▮▮▯▯▯▯▯▯ │ │ 0  ALL LANDED      ◎ │
│ GPU 12%     MEM 41%  │ │                      │
│ WindowServer     48% │ │ 7828 TODAY · $415.39 │
└──────────────────────┘ └──────────────────────┘
┌ AGENT POND ────────● ┐ ┌ AI QUOTA ──────────● ┐
│ ● deck      thinking │ │ ✳ Claude  ━━━━━  83% │
│ ● host          Bash │ │ ▣ Codex   ━━━    65% │
│ +5 more · 4 at work  │ │ ◆ DeepSeek   ¥75.18  │
└──────────────────────┘ └──────────────────────┘
```

## 应用

| 磁贴 | 全屏看到的 | 数据从哪来 |
|---|---|---|
| **SYSTEM** | 小屏版 htop：能效核 / 性能核分组的各核负载柱、CPU 走势、GPU、内存（按内存压力上色）和 swap、网速走势、SSD 剩余、电量和整机功耗、按 App 合并的进程表（coding agent 标颜色） | `host/apps/sysmon.py`，不用 sudo |
| **MAGPIE** | magpie 网关的实时请求：每个请求是一只喜鹊，从 agent 飞到 provider 再飞回来（原 magpie-live） | magpie 网关只读接口 |
| **AGENT POND** | 本机正在跑的 coding agent，画成一池萤火虫（原 agent-pond） | `~/.claude`、`~/.codex` 的会话文件 + 进程表 |
| **AI QUOTA** | Claude / Codex / DeepSeek / Grok… 的订阅额度、重置倒计时、余额（原 ai-subscription-display，从 LVGL 改画到共用渲染器上）；家数多了自动压成一家一行 | magpie 网关 + Claude Code 自己的本地缓存（见下文，不跑 `/usage`） |

磁贴里是实时的迷你预览，不用点进去也能看个大概。某个应用的数据超过 15 秒没更新，磁贴会压暗并标 `STALE`。

**角标**：应用觉得你该看一眼时，磁贴的角括号会呼吸、右上角亮一个琥珀色小点；在别的应用里时，屏幕顶部正中也会亮一个它颜色的小点。

| 应用 | 什么时候亮 |
|---|---|
| AGENT POND | 有 agent 一轮结束在等你回话 |
| MAGPIE | 15 秒内有请求失败 |
| SYSTEM | 内存压力 ≥ 75%，或者磁盘用到 97% 以上 |

## 手势

| 操作 | 效果 |
|---|---|
| 首页点磁贴 | 打开这个应用（淡入淡出） |
| 往左划 / 往右划 | 下一个 / 上一个，首页也在这个环里：首页 → SYSTEM → MAGPIE → AGENT POND → AI QUOTA → 首页（滑动动画） |
| 往上或往下划、长按 | 回首页 |
| 在应用里点一下 | 底部显示页码点，看看自己在第几个 |
| BOOT 键短按 / 长按 | 下一个 / 回首页（触摸屏坏了也能用） |

断电重启后回到上次停留的那个应用（停留 3 秒以上才记，存在 NVS 里）。

## 快速开始

```bash
scripts/flash.sh                         # 编译 + 烧录，任何 shell 里直接跑
python3 host/deck.py                     # 前台推送（Ctrl-C 停，退出前板子会显示 "stopped on the mac"）

python3 host/deck.py --demo              # 全部用假数据
python3 host/deck.py --once              # 不连板子，每个应用打印一帧 JSON
python3 host/deck.py --only sys,magpie   # 只跑其中几个
python3 host/deck.py --go sys            # 连上后切到某个应用（home / sys / magpie / pond / quota）
python3 host/deck.py --go sys --shot a.png   # 推几秒数据后截屏（不用拍照）
python3 host/deck.py -v                  # 同时打印板子日志、帧率、触摸手势

scripts/install-agent.sh                 # 常驻：登录自启、崩了自动重启

# Claude 额度用的小 mod（装一次就行，见「Claude 额度从哪来」）
claude plugin marketplace add "$PWD/claude-mod"
claude plugin install deck-usage@esp32-deck
```

- 常驻服务跑的是 `~/Library/Application Support/deck` 下的副本，改完 `host/` 要重跑 `install-agent.sh` 才生效。日志在 `~/Library/Logs/deck.log`。
- `install-agent.sh` 会顺手停掉老项目的常驻服务（`ai-quota-panel`、`agent-pond`），它们会抢同一个串口。
- `flash.sh` 发现常驻服务在跑会先暂停，烧完自动恢复。
- 拔掉板子、串口被占：主机每 2 秒重试，插回来自动接上，magpie 的图标会重发。
- 依赖：`brew install librsvg imagemagick`（magpie 渲染图标用）。主机端只用 Python 标准库。

## 结构

```
┌──────────────────── Mac：host/deck.py ────────────────────┐          ┌──────────── 板子 ────────────┐
│ 每个应用一个线程，按自己的节奏取数据：                    │          │ link.c   收行、按 app 名分发   │
│   apps/sysmon.py  1s     apps/magpie.py  250ms            │  USB     │ shell.c  首页、手势、切换动画  │
│   apps/pond.py    1s     apps/quota.py   2s               │ ───────▶ │ apps/*.c 各应用：解析 + 画面   │
│ → 发件箱 → 主线程写串口：@@sys{…} @@magpie{…} …           │ ◀─────── │ input.c  触摸(FT6336) + BOOT   │
│ ← 板子回：!!APP sys（切屏了）、!!IN tap 205 223（手势）   │          │ gfx.c    共用的软件渲染器      │
└───────────────────────────────────────────────────────────┘          └───────────────────────────────┘
```

某个应用卡住或报错只影响它自己（磁贴变 STALE），别的照常。断线期间发件箱只留每个应用最新的一帧，重连后不补发过期数据。

```
deck/
├── main/
│   ├── app.h              应用接口（一张函数表）
│   ├── shell.c            登记表 APPS[]、首页、手势、切换动画、NVS 记住上次的应用
│   ├── link.c             串口协议
│   ├── input.c            触摸 + BOOT 键 → 点击 / 长按 / 四向滑动
│   ├── gfx.c              渲染器：光层拖尾 + 合成 + 抖动、文字、矩形、遮罩
│   ├── util.h             颜色、插值、随机数、取 JSON 字段
│   └── apps/              sys.c  magpie.c  pond.c  quota.c  logos.c
├── host/
│   ├── deck.py            主程序：调度、串口、截图、退出时 BYE
│   ├── board.py           串口、找设备、组帧、截图解码
│   └── apps/              sysmon.py  magpie.py  pond.py  quota.py
├── claude-mod/            deck-usage：把 Claude Code 收到的额度读数写进缓存文件的小 mod
└── scripts/               flash.sh  install-agent.sh  gen-fonts.py  gen-logos.sh  gen-logo.py
```

## 加一个新应用

两边各写一个文件，各登记一行。约定只有一条：**主机发 `@@<id>{json}`，固件里 `id` 相同的应用收到 `data()`**。

**固件** `main/apps/hello.c`：

```c
#include "app.h"
#include "gfx.h"

static int s_n;

static void hello_init(void) {}
static void hello_data(const cJSON *root, int64_t now_ms) { s_n = (int)json_num(root, "n", 0); }

static void hello_enter(void)  // 进场：背景渐变是全局共用的，每个应用自己设
{
    for (int y = 0; y < GFX_H; y++) gfx_set_bg_row(y, 8, 10, 24);
}

static void hello_frame(float dt, int64_t now_ms)
{
    trail_fade(220);                                    // 光层衰减，动的光会拖出尾巴
    trail_glow(160, 120, 40, 0, 234, 255, 0.3f);
    gfx_compose(GFX_H);                                 // 背景 + 光层 → 整屏
    char s[16];
    snprintf(s, sizeof(s), "%d", s_n);
    text_align(&font_big, 160, 100, ALIGN_CENTER, s, 0, 255, 255, 255, 255);
}

static void hello_tile(const tile_t *t, int64_t now_ms)  // 首页磁贴（可选）
{
    char s[16];
    snprintf(s, sizeof(s), "n = %d", s_n);
    text_plain(&font_mono, t->x, t->y, s, 0, 220, 230, 255, 255);
}

const app_t app_hello = {
    .id = "hello", .name = "HELLO", .color = {255, 122, 217},
    .init = hello_init, .data = hello_data, .enter = hello_enter,
    .frame = hello_frame, .tile = hello_tile,
};
```

然后：`app.h` 里加 `extern const app_t app_hello;`，`shell.c` 的 `APPS[]` 里加 `&app_hello`，
`main/CMakeLists.txt` 的 `SRCS` 里加 `"apps/hello.c"`。可选的回调还有 `bye`（主机退出）、
`leave`、`input`（手势，返回 true 表示自己处理了）、`badge`（角标）。

**主机** `host/apps/hello.py`：

```python
import time

class HelloApp:
    id = "hello"          # 和固件里的 id 一致
    interval = 1.0        # 几秒取一次

    def poll(self) -> dict:          # 抛异常 = 这一轮失败，deck 记日志、下一轮重试
        return {"t": time.strftime("%H:%M"), "n": int(time.time()) % 100}

    # 下面都是可选的
    def demo(self) -> dict: ...                  # --demo 时用
    def summary(self, frame) -> str: ...         # 变化时打一行日志
    def events(self, frame) -> list[str]: ...    # 每帧要单独记的事件
    def extra_lines(self) -> list[bytes]: ...    # 数据帧之前要先发的行（比如图标）
    def on_connect(self) -> None: ...            # 板子（重新）连上时
```

然后在 `deck.py` 的 `APP_CLASSES` 里加 `("apps.hello", "HelloApp")`。每帧带上 `"t"`（时间）就行，首页顶栏的时钟也会用它。

首页目前是 2×2，只画前 4 个应用的磁贴；第 5 个起还能左右划到，首页分页还没做。

## 串口协议

主机 → 板子，按行：

| 行 | 作用 |
|---|---|
| `@@<app>{json}` | 给某个应用的一帧数据 |
| `@@deck{"t":"15:43","d":"MON 02 NOV","h":"..."}` | 给面板自己：首页顶栏的时间、日期、主机名（每 5 秒）；可带 `"go":"sys"` 切屏 |
| `@@{json}` | 老格式（单项目时代的主机脚本），按字段认：`a` → pond，`f`/`d` → magpie，`p` → quota |
| `@@ICON <key> 18 <hex>` | 共享图标缓存（每像素 RGB565 小端 + alpha） |
| `@@GO <app>` / `@@GO home` | 切屏（淡入淡出） |
| `@@BYE` | 主机要退出了：每个应用都转成「已停止」 |
| `@@HELLO` | 板子回 `!!HELLO deck 1 <当前画面> touch=<0/1>` |
| `@@SHOT` / `@@PING` / `@@BRIGHT 0-100` | 截图 / `!!PONG` / 背光 |

板子 → 主机：`!!APP <id>`（切屏了）、`!!IN <tap/long/left/right/up/down/btn/btnlong> x y`（手势）、
`!!SHOT w h len` + `!!DATA <hex>`… + `!!SHOTEND`（截图，RLE），以及 WARN 级日志（帧率每 10 秒一行）。

各应用的帧格式：`sys` 见 `host/apps/sysmon.py` 开头；`magpie`、`pond`、`quota` 和原项目一样，
见 `../magpie-live`、`../agent-pond`、`../ai-subscription-display` 的 README。

## Claude 额度从哪来

以前每 30 秒跑一次 `claude -p /usage`：每次起一个 Claude Code 进程，还会在 `~/.claude/projects` 里留一个会话
（agent 夜池里那个叫 `~` 的萤火虫就是它）。现在一次都不跑，只读两份本地缓存，每个窗口取较新的那份：

| 来源 | 内容 | 什么时候更新 |
|---|---|---|
| `~/Library/Caches/deck/claude-limits.json`，`claude-mod/` 里的 deck-usage mod 写 | 5h、7d 窗口：已用 % + 重置时刻 | Claude Code 每次 API 响应都带着这两个窗口；窗口挪动一个整点，引擎推 `session.measure`，mod 就写一次。这台 Mac 上有会话在干活时几乎是实时的 |
| `~/.claude.json` 的 `cachedUsageUtilization` | 同上，外加按模型的周窗口（Fable） | Claude Code 跑 `/usage` 时存的，也就是你自己在会话里敲 `/usage` 时 |

- 没有会话在跑时，额度也不会因为这台 Mac 变；重置时刻过了就当窗口已清零。
- 读数超过 1 小时，卡片上会注明 `2h ago` 之类，别当成实时的（比如 Mac mini 或 claude.ai 上用掉的，要等这台 Mac 的会话下次收到响应才会反映出来）。
- mod 没有界面，不改状态栏；从 `claude-mod/` 目录直接读，改完在会话里 `/reload-plugins` 生效。卸载：`claude plugin uninstall deck-usage@esp32-deck`。

## 系统页的数据

`sysmon.py` 每次采样约 5ms（每 2 秒多跑一次 `ps`，约 30ms），全部不用 sudo：

- **各核**：`host_processor_info`。哪些是能效核读设备树里每个 CPU 的 `cluster-type`，所以换到 Mac mini 也对（M1 Max 上 cpu0–1 是能效核，`taskpolicy -b` 压测验证过）。超过 24 核时同类相邻的核取平均。
- **GPU**：ioreg 里 IOAccelerator 的 `Device Utilization %`。
- **内存**：「已用」和活动监视器一致（App + 联动 + 压缩）；内存条颜色按内存压力（`kern.memorystatus_level`），这才是系统真正觉得紧不紧。
- **进程**：按最外层的 `X.app` 合并（微信的十几个 Helper 合成一行 `WeChat`），不在 .app 里的用可执行文件名；Claude Code / Codex 等 coding agent 合成一行并标颜色。CPU 是两次采样之间的差值，和 htop 一样是「此刻」的。别的用户（root、_windowserver…）的进程 libproc 读不了，退回 `ps` 的累计 CPU 时间自己算差值，否则会漏掉三分之一的负载。
- **网络**：只算物理网卡（`en*`、`awdl*`），VPN 隧道的流量也会走 en0，算了就重复了。
- **功耗**：电池控制器报的整机功耗 `SystemPowerIn`，只有笔记本有。

拿不到的：`kernel_task` 的 CPU（`ps` 也看不见，所以进程合计会比总 CPU 少一截）、别的用户进程的「内存」（只有常驻内存）、温度。

## 和老项目的关系

- 三个老项目的目录原样保留，单独烧录也还能用。
- **老项目的主机脚本对 deck 固件照样有效**：它们发的是不带应用名的 `@@{json}`，固件按字段认出是谁的。
- deck 的主机端把三份重复的串口 / 截图代码合成了 `host/board.py`，三个数据源搬进了 `host/apps/`，探测逻辑没改。

## 实现要点

- 渲染器沿用 agent-pond / magpie-live 的软件渲染（光层拖尾 + 双线性放大 + 4×4 有序抖动），所有应用共用；切应用时清掉光层，背景渐变由各应用进场时自己设。
- 切换动画：切之前把最后一帧拷进 PSRAM，新应用照常画满整屏，再按进度把旧帧叠上去（滑动 = 每行两次 memmove，淡入淡出 = 逐像素混色）。
- 为了让首页和系统页跑满 30fps 做过的优化：
  - 半透明填充改成 RGB565 直接混色（摊开成 `0x07E0F81F` 一次乘完三个通道），
  - 光层按行记录有没有光，暗的行整行跳过，
  - 合成时用查表代替逐像素钳位和打包，
  - 结果：首页从 18fps 到 30fps。实测各页：首页 / 系统 / 额度约 30fps，magpie 约 28，agent 夜池 8 只萤火虫时约 26。
- 推屏与渲染并行（核 1 渲染，核 0 分条 DMA 推屏），板级参数、截图协议、颜色字节序等坑见 `../ai-subscription-display/README.md`。
- 字体：`scripts/gen-fonts.py` 栅格化系统字体（Avenir Next、New York Italic、SF Mono、Avenir Next Condensed），4bpp，带 `· ¥ ° …` 几个扩展字符，文字接口吃 UTF-8。

## 已知限制

- 字库没有中文：中文名字会显示成 `?`。系统页对中文 App 名会退回它的可执行文件名（比如 `NeteaseMusic`）。
- 触摸方向照搬小智 lichuang-dev 的映射（`input.c` 的 `map_xy`），实测点击位置和左右划方向是对的；要是哪天觉得反了，改那一个函数就行。
- 必须有 Mac 在跑 `deck.py`，数据源都在 Mac 本地。Mac 睡眠后各应用转成「信号丢失」，首页右上角变成 `NO SIGNAL`。
