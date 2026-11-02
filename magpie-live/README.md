# magpie live（立创·实战派 ESP32-S3）

把 magpie 网关的实时请求画在 2 寸屏上：左边一列 agent，右边一列 provider，中间是 magpie。
每个请求是一只喜鹊：从 agent 飞出，穿过网关，在 provider 旁盘旋等回复，回完原路飞回，
成功洒金光、飘出 `+输出 token 数`，失败变红、飘出 `error`。

| 看到的 | 含义 |
|---|---|
| 节点徽章 | agent / provider 的图标，和 magpie 里显示的一样 |
| 节点外圈变亮 | 这个节点此刻有请求在飞 |
| 右上 `N in flight` | 在飞的请求数；下面一行是今天的请求数和花费 |
| `direct` | 没经过网关、只出现在 magpie 日志里的请求（比如 Claude Code 直连） |
| `gateway offline` | Mac 端连不上 magpie |

## 数据从哪来

不是 magpie 插件。magpie 没有界面插件机制（它的 `plugin` 只管 OpenCode provider），所以直接读网关的只读接口，本机访问不用 key：

- `GET http://127.0.0.1:3425/v1/magpie/concurrency`：此刻在飞的请求
- `GET http://127.0.0.1:3425/v1/magpie/usage/requests?period=today`：今天落地的请求、今日总数和花费、各 agent / provider 的名字和图标名

`host/live.py` 每 250ms 读一次，推给板子。在飞请求和落地记录的 id 不是同一套，脚本按 agent+provider 配对，鸟才能原路飞回。

图标：magpie 把图标编进了自己的程序里（`assets/icons/*.svg`），网关不对外提供。`live.py` 启动时按
`$MAGPIE_BIN` → `~/.local/bin/magpie` → `/Applications/magpie.app/Contents/MacOS/magpie` 的顺序找到程序，
从里面读出 Go embed 文件表，把图标导到 `~/Library/Caches/magpie-live/icons/`（magpie 升级后按 mtime 自动重新导出），
再用 `rsvg-convert` / `magick` 渲染成 18×18 发给板子。provider 的自定义图标读 `~/.config/magpie/icons/`。
一个图标都导不出来时启动日志会报「找不到 magpie 程序」，板子上只剩彩色光点。

## 快速开始

```bash
scripts/flash.sh                      # 编译 + 烧录，任何 shell 里直接跑
# 想手动用 idf.py：先 bash，再 source scripts/env.sh（fish 不能 source bash 脚本）

python3 host/live.py                  # 前台推送（Ctrl-C 停）
python3 host/live.py --once           # 不连板子，打印有变化的帧
python3 host/live.py --demo           # 假流量看效果
python3 host/live.py --shot out.png   # 推几秒后截屏
```

退出与出错：Ctrl-C、`kill`、关掉终端都会正常退出，退出前给板子发 `@@BYE`，板子收回所有喜鹊、显示 `stopped on the mac`。
拔掉板子、串口被别的进程占着：脚本每 2 秒重试，插回来自动接上。网关没开或返回的东西不对：板子显示 `gateway offline`，脚本照常跑。

依赖：`brew install librsvg imagemagick`（画图标用）。和 agent-pond 共用一块板子、一个串口，同一时间只能跑一个主机脚本，切项目要重新烧录。

改 logo：`uv run --with pillow python scripts/gen-logo.py`（从 magpie.app 的图标生成 `main/logo.c`）。

## 串口协议（主机 → 板子）

```
@@{"v":1,"t":"01:08","n":145,"c":7.33,
   "f":[{"id":"355","a":"dsh","an":"DeepSeek Harness","p":"douban-oneapi","pn":"Douban OneAPI",
         "m":"deepseek-flash","ai":"<图标key>","pi":"<图标key>","ph":"upstream","ms":14188}],
   "d":[{...同上, "o":910,"ms":19200,"ok":1}]}
@@ICON <8 位 key> 18 <每像素 6 个十六进制：RGB565 小端 + alpha>
@@BYE          主机退出
@@SHOT / @@PING / @@BRIGHT 0-100
```

`f` 在飞，`d` 上一帧以来落地；连不上网关时多一个 `"x":1`。图标第一次用到时发，之后每 20 秒重发一遍（防板子重启）。

渲染器、板级驱动、截图沿用 `../agent-pond`，坑见 `../ai-subscription-display/HANDOFF.md`。
