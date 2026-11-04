# deck：我的桌面小面板（立创·实战派 ESP32-S3）

2 寸屏上的一个「桌面」：首页是一排磁贴，每个磁贴是一个小应用，点进去全屏看，左右划切换。
之前的三个项目都变成了这里的应用，再加一个新写的系统监控，以及两个用摄像头的：一面镜子、一张会看你的卡通脸。**一套固件、一个主机进程**，
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
| **MIRROR** | 板载摄像头的镜子，点一下换效果：光绘、字符雨、吉卜力…（见「MIRROR：摄像头镜子」） | 板子自己的摄像头；GHIBLI 效果在 Mac 上画 |
| **EYES** | 一张会看你的卡通脸：跟着你的脸转，没人时自己找事干、犯困、睡着，你回来会笑（见「EYES：一张会看你的卡通脸」） | 摄像头 + 板上跑的人脸检测，不用 Mac |

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
| 往左划 / 往右划 | 下一个 / 上一个，首页也在这个环里：首页 → SYSTEM → MAGPIE → AGENT POND → AI QUOTA → MIRROR → EYES → 首页（滑动动画）。应用超过 4 个时，首页上左右划是翻页 |
| 往上或往下划、长按 | 回首页 |
| 在应用里点一下 | 底部显示页码点，看看自己在第几个 |
| BOOT 键短按 / 长按 | 下一个 / 回首页（触摸屏坏了也能用） |

断电重启后回到上次停留的那个应用（停留 3 秒以上才记，存在 NVS 里）。

**自动调暗**：主机 2 分钟没消息（Mac 睡了、deck 没在跑）并且 1 分钟没人碰屏幕，背光慢慢降到 6%，帧率降到 10fps；
一有数据或者摸一下就亮回来。暗着的时候第一下触摸只负责叫醒，不会误点进某个应用。

## 快速开始

```bash
scripts/flash.sh                         # 编译 + 烧录，任何 shell 里直接跑
python3 host/deck.py                     # 前台推送（Ctrl-C 停，退出前板子会显示 "stopped on the mac"）

python3 host/deck.py --demo              # 全部用假数据
python3 host/deck.py --once              # 不连板子，每个应用打印一帧 JSON
python3 host/deck.py --only sys,magpie   # 只跑其中几个
python3 host/deck.py --go sys            # 连上后切到某个应用（home / sys / magpie / pond / quota / mirror / eyes）
python3 host/deck.py --go sys --shot a.png   # 推几秒数据后截屏（不用拍照）
python3 host/deck.py -v                  # 同时打印板子日志、帧率、触摸手势

scripts/install-agent.sh                 # 常驻：登录自启、崩了自动重启

# Claude 额度用的小 mod（装一次就行，见「Claude 额度从哪来」）
claude plugin marketplace add "$PWD/claude-mod"
claude plugin install deck-usage@esp32-deck
```

- 常驻服务跑的是 `~/Library/Application Support/deck` 下的副本，改完 `host/` 要重跑 `install-agent.sh` 才生效。日志在 `~/Library/Logs/deck.log`。
- `install-agent.sh` 会顺手停掉老项目的常驻服务（`ai-quota-panel`、`agent-pond`），它们会抢同一个串口。
- `flash.sh` 和前台跑的 `deck.py` 发现常驻服务在跑都会先暂停它（它占着串口），退出时自动恢复。
- 拔掉板子、串口被占：主机每 2 秒重试，插回来自动接上，magpie 的图标会重发。
- 依赖：`brew install librsvg imagemagick`（magpie 渲染图标用）。主机端只用 Python 标准库；
  MIRROR 的 GHIBLI 效果另外要 `brew install uv`（推理子进程的依赖由 uv 自己装）。

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
│   ├── lens.c             摄像头（GC0308）：上电 / 断电、取帧、亮度图和帧差（核 0）
│   ├── face.cpp           人脸检测（ESP-DL human_face_detect），核 0 的低优先级任务
│   ├── relay.c            GHIBLI：摄像头画面编 JPEG 送 Mac、收回画好的画面解码（核 0 编、核 1 解）
│   ├── util.h             颜色、插值、随机数、取 JSON 字段
│   └── apps/              sys.c  magpie.c  pond.c  quota.c  mirror.c  eyes.c  logos.c
├── host/
│   ├── deck.py            主程序：调度、串口、截图、退出时 BYE
│   ├── board.py           串口、找设备、组帧、截图解码
│   ├── apps/              sysmon.py  magpie.py  pond.py  quota.py  ghibli.py
│   └── ghibli/worker.py   吉卜力风格推理子进程（uv run --script，依赖写在文件头）
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
用摄像头、不靠主机数据的应用设 `.camera = true`：磁贴不等 Mac、不变 STALE，重启后也不会自动回到它。
画面本来就更新得慢的应用可以在进场时 `shell_want_fps(15)`，省下 CPU 和 PSRAM 带宽（切换动画、点击水波纹时壳会自动回到 30fps）。

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

不是定时取数据、而是「来一帧处理一帧」的 app（比如 `apps/ghibli.py`）：不写 `poll`，改写 `bin_tags = ("cam",)`、
`on_bin(tag, data)`（板子发来的二进制帧）和 `start(outbox, stop, log)`（自己开线程，结果直接 `outbox.put(id, [bytes])`）。

首页一页 4 个磁贴，第 5 个起自动排到下一页。文字接口吃 UTF-8，中文直接写就行。

## MIRROR：摄像头镜子

板载的 GC0308（30 万像素）出 320×240，正好铺满屏。点一下换效果：

| 效果 | 看到的 |
|---|---|
| LIGHT PAINT | 画面压暗，哪里在动哪里就留下彩色光尾，挥手能画出一道彩虹 |
| MATRIX | 字符雨：屏幕切成 40×24 个字符格，每列一道往下掉的雨（字是镜像的）。画面亮的地方字也亮，人从字里浮出来 |
| GHIBLI | 吉卜力风格：画面送到 Mac 上用宫崎骏风格的模型重画一遍再送回来（见下一节） |
| GHOST | 只有动的地方发光，站着不动就隐身 |
| NEON | 只剩轮廓线，按行渐变的霓虹色 |
| PLAIN | 普通镜子 |

- **摄像头只在 MIRROR 在前台时上电**，切走就断电（PCA9557 的 bit2）。除了 GHIBLI，画面都不出板子，主机端不需要跑任何东西。
  断电重启后也不会自动回到 MIRROR，免得一开机摄像头就亮。
- 主机想切效果可以发 `@@mirror{"fx":"matrix"}`（`paint` / `matrix` / `ghibli` / `ghost` / `neon` / `plain`）。
- 在镜头前挥手也算「有人」，和碰屏幕一样推迟自动调暗。
- 帧率：摄像头 26.7fps，渲染 30fps；光绘、幽灵模式下大幅度挥手、满屏光尾时会掉到 24–27fps（光层合成是逐像素的，满屏时最贵）。

实现：`lens.c` 在核 0 开一个任务取帧，一趟走完把传感器的大端 RGB565 换成小端、算出 160×120 的亮度图和帧差，
几块缓冲轮换交给读者（渲染线程、送去 Mac 的 relay 任务各占一块），互不等待。帧差、亮度图正好和光层同分辨率，光绘就是把帧差直接写进光层；
摄像头底图用 `gfx_compose_img` 合成（压暗和叠光一趟做完，图从 PSRAM 只读一次）。
引脚和小智 lichuang-dev 一致，SCCB 复用触摸芯片那条 I2C1，驱动是 `espressif/esp32-camera`（sdkconfig 里只开了 GC0308 的探测）。

### GHIBLI：在 Mac 上画，回显到板子

```
板子 relay_enc（核 0）          USB 串口              Mac：deck.py → apps/ghibli.py
摄像头最新一帧 → JPEG ── !!BIN cam <len> + JPEG ──▶  交给推理子进程（uv run --script ghibli/worker.py）
                                                     AnimeGANv3 Hayao（宫崎骏风格），onnxruntime CPU
屏幕 ◀ RGB565 ◀ JPEG ◀─ @@BIN fx <len> + JPEG ────  320×240 JPEG
       relay_dec（核 1，优先级比渲染低）
```

- 模型是 [AnimeGANv3](https://github.com/TachibanaYoshino/AnimeGANv3) 发布页的 `AnimeGANv3_Hayao_36.onnx`（4.2MB），
  第一次用时下载到 `~/Library/Caches/deck/models/` 并校验 sha256。这个项目没写许可证、README 说非商用，自己玩没问题。
- `worker.py` 的依赖（onnxruntime、numpy、pillow）写在文件头的 PEP 723 元数据里，`uv run --script` 自动装进 uv 的缓存环境，
  **deck.py 本身还是只用标准库**。要先 `brew install uv`；第一次切到 GHIBLI 要等十几秒（装依赖、下模型），之后一两秒就起来。
  30 秒没有画面过来就把子进程停掉。
- 速度（M1 Max）：约 7fps，从拍到显示约 350ms。三段大致相当：板子编 JPEG 约 75ms、Mac 推理 85–110ms（8 个性能核满载）、
  板子解码约 35ms；最多同时 2 帧在路上，让 USB 传输和 Mac 推理重叠。这段时间屏幕只刷 15fps（`shell_want_fps`），
  省下的 PSRAM 带宽和核 0 时间给编码用——编码从 120ms 降到 75ms。
- **它会让 Mac 的性能核一直满载**：5 分钟没人碰屏幕、或者屏幕自动调暗了就暂停（点一下继续）。
- **这个效果的画面会离开板子**，送到这台 Mac 上的 deck.py（不出本机）。Mac 没响应时显示压暗的原画面和提示。

## EYES：一张会看你的卡通脸

整块屏幕是脸：暖色的底，白眼球描墨线，大瞳孔（上深下浅的琥珀色渐变）带两个高光，眉毛、腮红、会变形的嘴。

| 情况 | 表情 |
|---|---|
| 看到你的脸 | 盯着你转，瞳孔放大；偶尔冲你眨一只眼。凑太近会瞪大眼、嘴变成 O |
| 没看到脸 | 看画面里在动的东西，什么都没有就自己找事干：东张西望、挑一边眉毛好奇、嘟嘴吹口哨 |
| 没人 25 秒 | 犯困：眼皮半垂、眨得又慢又少，隔一阵打个哈欠 |
| 没人 45 秒 | 睡着：眼睛闭成 ‿‿，冒 z。画面里有大动静会把它吵醒 |
| 你离开超过 5 秒又回来、或者把它吵醒 | ^^ 笑眼，咧嘴笑，脸红，蹦一下 |
| 点一下屏幕 | >_<，脸红，蹦一下 |

- 动起来不呆板靠这几样：视线和整张脸的位置都是弹簧（转过去会稍微过冲再回来）；脸跟着视线平移一点、带呼吸起伏；
  盯着你时瞳孔有细微的抖动；眨眼时眼睛略微变宽（挤压拉伸）；表情全是连续量，换表情是渐变过去的，嘴型交叉淡入。
- 摄像头和人脸检测都只在 EYES 在前台时开，和 MIRROR 一样画面不出板子。看到脸时算「有人」，不会自动调暗。
- 主机可以发 `@@eyes{"poke":1}`（戳一下）、`{"greet":1}`（打招呼）、`{"nap":1}`（马上睡）、`{"debug":1}`（左上角画出人脸框，底下一行状态和检测耗时）。
- 人脸检测用乐鑫 ESP-DL 的 `human_face_detect`（MSR + MNP 两级模型，约 190KB，放在 flash 里）。
  `lens.c` 处理每帧时，检测器空闲就顺手写一张 160×120 的彩色图给它（正好是模型第一级的输入，不用再缩放）；
  检测在核 0 的低优先级任务里跑，最多每秒 6 次左右，实际一次 160–180ms（核 0 上推屏、取帧优先），对眼睛够用。
  模型第一次用时才加载，离开 EYES 就释放（它占一块内部 RAM，摄像头和 GHIBLI 的编解码器都要用）。
- 画法：眼白是圆角矩形的距离场，和上下眼皮（抛物线）取交集，外面一圈墨线。上眼皮合到底，剩下的墨线就是闭眼的 ‿；
  下眼皮顶到头就是笑眼的 ⌒（剩下的月牙涂成墨色）。为了跑满 30fps：只有圆角和边缘才逐像素算距离场，
  每行整块在眼白里、又在虹膜范围外的那段直接写白；S3 的浮点单元没有开方指令，用平方根倒数的位运算 + 牛顿迭代代替 `sqrtf`。
  每帧约 28ms（两只眼睛 13ms）。

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
| `@@BIN <tag> <len>` + len 个原始字节 | 二进制帧，固件里用 `link_on_bin(tag, cb)` 收（GHIBLI 画好的画面是 `fx`）。最大 160KB |

板子 → 主机：`!!APP <id>`（切屏了）、`!!IN <tap/long/left/right/up/down/btn/btnlong> x y`（手势）、
`!!SHOT w h len` + `!!DATA <hex>`… + `!!SHOTEND`（截图，RLE）、`!!BIN <tag> <len>` + 原始字节（`link_write_bin`，摄像头画面是 `cam`），
以及 WARN 级日志（帧率每 10 秒一行）。写串口有一把锁，二进制帧的头和数据中间不会插进日志；日志碰上锁被占着就直接丢掉，不阻塞。
主机端 `board.Serial.readline` 读到 `!!BIN` 头会把数据收齐放进 `ser.bins`，deck.py 再交给声明了这个 tag 的 app。

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
  - 光层每行记下有光的那一段，衰减和合成只处理那段，范围外直接铺背景；倒影也改成查表，
  - 走势线一列一道竖线，不再用抗锯齿粗线，
  - 结果：首页从 18fps 到 30fps，夜池（8 只萤火虫）从 26 到 30。现在每页都在 29.8fps 左右，每帧 20–28ms（上限 33ms）。
- 推屏与渲染并行（核 1 渲染，核 0 分条 DMA 推屏），板级参数、截图协议、颜色字节序等坑见 `../ai-subscription-display/README.md`。
- 内部 RAM 很挤（这块板子的大头都在 PSRAM）：摄像头 DMA 一块限 16KB（`CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX`），
  GHIBLI 的 JPEG 编码器 / 解码器（约 39KB / 9KB）只在用的时候开、而且等摄像头先起来再开，人脸模型离开 EYES 就释放，
  relay 的两个任务栈放在 PSRAM。来回切 MIRROR / GHIBLI / EYES，内部 RAM 每轮都回到同样的数。
- USB-Serial-JTAG 的坑：驱动的发送环形缓冲只有 4KB，一次写超过它的长度会永远写不进去（返回 0），所以 `link.c` 每次最多写 1KB。
  实测两个方向都约 840KB/s。
- 整屏缓冲其实在 PSRAM 里：内部 RAM 最大的连续块只有 90 多 KB，放不下 150KB（开机日志会打 `fell back to PSRAM`）。
  所以数据缓存开到 64KB、行长 64B（`sdkconfig.defaults`），每帧拷进推屏缓冲那一步从 9–11ms 降到约 6.7ms，各页都受益。
- 字体：`scripts/gen-fonts.py` 栅格化系统字体（Avenir Next、New York Italic、SF Mono、Avenir Next Condensed），4bpp，带 `· ¥ ° …` 几个扩展字符。
- 中文：另做一套后备字库 `main/cjk12.bin`（冠黑体 W3 12px，GB2312 的 6763 个汉字 + 常用中文标点，582KB，`EMBED_FILES` 原样放在 flash 里）。
  主字体里没有的字符二分查找这套字库，按基线对齐画；JSON 里的字符串截断时不会截在半个字上。

## 已知限制

- 中文只收了 GB2312 的字，生僻字、繁体、emoji 显示成 `?`。
- 触摸方向照搬小智 lichuang-dev 的映射（`input.c` 的 `map_xy`），实测点击位置和左右划方向是对的；要是哪天觉得反了，改那一个函数就行。
- 必须有 Mac 在跑 `deck.py`，数据源都在 Mac 本地（MIRROR、EYES 除外，但 MIRROR 的 GHIBLI 要 Mac）。Mac 睡眠后各应用转成「信号丢失」，首页右上角变成 `NO SIGNAL`。
- 摄像头 30 万像素，近处看得清，远处和暗处噪点多；字符雨只有 40×24 格，人要离近一点才认得出轮廓。
- 人脸检测要正脸、光线够；侧脸、背光、离得远时会时有时无（EYES 会把视线保持 1.2 秒，短暂丢失看不出来）。
