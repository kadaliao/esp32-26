# AI 用量面板（立创·实战派 ESP32-S3）

把一块立创·实战派 ESP32-S3 做成桌面小屏，实时显示 **Claude Code / Codex / DeepSeek / Grok**
的订阅额度、重置倒计时和余额。数据来自 [magpie](https://github.com/yetone/magpie) 的本地网关
外加 `claude -p /usage`，经 **USB 串口**推给板子 —— 不配 WiFi、不联网、不上传任何东西。

```
┌──────────────────────── Mac ────────────────────────┐      USB-C      ┌──── 实战派 ESP32-S3 ────┐
│  magpie 网关 127.0.0.1:3425                          │                 │  2.0" IPS 320×240       │
│    GET /v1/magpie/quotas  → Codex/DeepSeek/Grok      │                 │  ST7789 + LVGL 9        │
│  claude -p /usage         → Claude 5h/周/Fable       │                 │                         │
│            ↓ host/bridge.py                          │  115200 8N1     │  link.c 解析 JSON       │
│  "@@{json}\n" 每 2 秒 ───────────────────────────────┼────────────────▶│  ui.c  刷新卡片         │
│  "!!SHOT …" 截图回传 ◀───────────────────────────────┼─────────────────┤                         │
└──────────────────────────────────────────────────────┘                 └─────────────────────────┘
```

## 屏幕内容

每张卡片一行标题（名称 / 套餐 / 大字号剩余百分比）+ 若干条额度进度条（剩余比例 + 重置倒计时）、
余额。进度条按剩余量变色：≥50% 绿、≥20% 黄、其余红。

| 来源 | 显示 |
|---|---|
| Claude Code | 5h 会话窗口、7 天全模型窗口，`Fable xx%` 放在副标题 |
| Codex | 7 天窗口、credits 余额 |
| DeepSeek | 账户余额（¥） |
| Grok | 7 天窗口 |

顶栏是主机时间 + 链路状态（`LINK 2s` / `STALE 30s` / `NO LINK 3m`）。

## 为什么 Claude 不走 magpie

magpie 目前读不到 Claude 订阅的额度窗口（`magpie quota claude` 报 `not configured`，
`~/.config/magpie/quotas.json` 里没有 claude 条目）。所以 bridge 直接跑 Claude Code 自己的
`claude -p /usage`（这是 Claude Code 本地回答的命令，不消耗额度），解析其中三行：

```
Current session: 4% used · resets Nov 1 at 9:20pm (Asia/Singapore)
Current week (all models): 100% used · resets Nov 1 at 11pm (Asia/Singapore)
Current week (Fable): 10% used · resets Nov 1 at 11pm (Asia/Singapore)
```

## 快速开始

```bash
# 1. 编译（复用 esp32-playground 里已装好的 ESP-IDF 6.0.2）
source scripts/env.sh
idf.py build
idf.py -p "$ESPPORT" flash monitor      # Ctrl+] 退出

# 2. 不开板子也能看数据长什么样
python3 host/bridge.py --once

# 3. 推送（前台，先看日志）
python3 host/bridge.py

# 4. 常驻（开机自启 + 崩了自动重启）
scripts/install-agent.sh
```

`install-agent.sh` 会把 `host/bridge.py` **复制**到
`~/Library/Application Support/ai-quota-panel/bridge.py` 并在那里运行，
而不是直接跑仓库里的文件：仓库在外部卷 `/Volumes/Data-External` 上，
launchd 冷启动时从那里 import 会卡死在 `open()`（实测），而且外部盘没挂载时服务也不该挂着。
**改完 `host/bridge.py` 后要重跑一次 `scripts/install-agent.sh` 同步副本。**

`scripts/env.sh` 会把 `python3` 临时指向 3.14 —— 系统 python 已经升到 3.15，
而 ESP-IDF 的虚拟环境是按 3.14 建的，不换会在 `export.sh` 报 venv 不存在。

## 串口协议

主机 → 板子，按行：

| 行 | 作用 |
|---|---|
| `@@{json}` | 一帧完整快照 |
| `@@SHOT` | 回传当前画面 |
| `@@PING` | 板子回 `!!PONG` |
| `@@BRIGHT <0-100>` | 调背光 |

快照格式（`u` = 已用百分比，`x` = 距重置秒数，`-1` 未知）：

```json
{"v":1,"t":"17:19:28","p":[
  {"id":"claude","n":"Claude","s":"Max","w":[{"k":"5h","u":4,"x":14371},
                                            {"k":"7d","u":100,"x":20371}],"m":"Fable 10%"},
  {"id":"codex","n":"Codex","s":"Pro","w":[{"k":"7d","u":27,"x":249503}],"m":"62,168 credits"},
  {"id":"deepseek","n":"DeepSeek","s":"","w":[],"m":"¥76.24"},
  {"id":"grok","n":"Grok","s":"SuperGrok","w":[{"k":"7d","u":100,"x":154434}],"m":""}]}
```

板子 → 主机：`!!PONG` / `!!OK` / `!!SHOT w h len` + 若干 `!!DATA <hex>` + `!!SHOTEND`。
日志（WARN 以上）也走同一个口，bridge 会忽略非 `!!` 开头的内容。

板子的日志是**非阻塞写**（`main.c` 里换了 `esp_log_set_vprintf`），
所以主机没在监听时固件也不会卡在写串口上。

## 截图调试

不用拍照，直接让板子把最后渲染的一帧回传：

```bash
python3 host/bridge.py --shot panel.png
```

实现在 `main/shot.c`：flush 回调顺手把每个区域拷进 PSRAM 里的整屏影子缓冲，
收到 `@@SHOT` 就 RLE 压缩（`0x80|(run-1)` + RGB565 / `lit-1` + 原始像素）再 hex 分行发出，
主机侧 `host/bridge.py` 解码后直接写 PNG（不依赖 Pillow）。

## 板级参数

全部照抄小智 `lichuang-dev` 板型（在这块板上验证过，见
`~/workspace/esp32-playground/xiaozhi-esp32/main/boards/lichuang-dev/`）：

| 项 | 值 |
|---|---|
| 屏幕 | ST7789，320×240，SPI3，MOSI=40 / SCLK=41 / DC=39 / CS=NC，spi_mode 2，80 MHz |
| 面板 | swap_xy、mirror(x)、invert_color，复位脚 NC |
| 背光 | GPIO42，LEDC 5 kHz，**低电平点亮**（反相） |
| IO 扩展 | PCA9557 @0x19，I2C1（SDA=1，SCL=2）；reg0x03=0xF8、reg0x01=0x03，复位后 bit0=0 给屏幕上电 |
| 芯片 | ESP32-S3，16 MB Flash / 8 MB Octal PSRAM，原生 USB-Serial/JTAG |

## 回滚到小智固件

原始固件备份在 `~/workspace/esp32-playground/backups/`：

```bash
source ~/workspace/esp32-playground/scripts/env.sh
esptool --port "$ESPPORT" write-flash 0x0 \
  ~/workspace/esp32-playground/backups/2026-07-18-before-xiaozhi-esp32s3-16mb.bin
```

## 踩过的坑（都已修）

| 现象 | 原因 |
|---|---|
| 开机就 `Guru Meditation (LoadProhibited)`，EXCVADDR `0x10` | `CONFIG_LV_USE_FONT_COMPRESSED` 没开。LVGL 自带和 `lv_font_conv` 生成的字体都是 4bpp 压缩字形，没有解压能力时 `get_glyph_bitmap()` 返回 NULL，`draw_buf->data` 就解引用空指针。**任何文字都会崩**。 |
| 同上，第一条日志就崩 | `esp_log_set_vprintf` 换成自写函数后，`board_init()` 的第一条 `ESP_LOGI` 在 `usb_serial_jtag_driver_install()` 之前就调 `write_bytes`。现在先 `link_start()`，并加了驱动就绪守卫。 |
| 串口截图收到 `!!SHOT` 头但没有数据 | `shot.c` 里前缀只写了 `"!!D "`，少写 4 个字符；另外 Python 侧 `readline` 每次 `read(4096)` 后只取第一行、把同块剩余行丢掉，改成了带内部缓冲的行读取器。 |
| 最后一张卡被底部裁掉 | 卡片高度分配时用含 gap 的 `y` 去算最后一张，导致它比自然高度小 8px。改成用累计高度差补误差。 |
| launchd 服务启动后毫无输出、不占串口 | 从外部卷上 import 卡在 `open()`。改为跑本地副本。 |

## 已知限制

- **必须有主机在跑 bridge**。Mac 休眠/退出登录后屏幕会一直显示最后一次数据并转成 `NO LINK`，
  不会自己恢复 —— 数据源都在 Mac 本地。
- bridge 每 30 秒跑一次 `claude -p /usage`（约 3 秒）。如果 Claude Code 未登录会拿不到 Claude 卡片。
- 界面文字是英文。内置 Montserrat 不含 `¥`，所以 `main/fonts/` 里的三套字体是用
  `lv_font_conv` 从 LVGL 自带 Montserrat-Medium.ttf 重新生成的子集（0x20–0x7E + `¥` + `·` + `—`）。
- 编辑器里 clangd 跑在宿主机 clang 上，看不懂 Xtensa 工具链，`.clangd` 里摘掉了不兼容参数。
  **以 `idf.py build` 的结果为准**。
