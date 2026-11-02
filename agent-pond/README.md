# Agent 夜池（立创·实战派 ESP32-S3）

把这台 Mac 上正在跑的 coding agent 画成一池夜色里的萤火虫。

```
 23:41                              2 at work · 1 waiting
        ✦                    .
     (●) magpie           (●)  bisai
         thinking               asleep 25m
                 (✺) agent-pond
                     Bash +2       (◎) douban
                                       your turn
 ─────────────────────── 水面 ───────────────────────
            ～ 倒影随波纹晃动 ～
```

| 看到的 | 含义 |
|---|---|
| 颜色 | 珊瑚 = Claude Code，冰青 = Codex，蓝 = Gemini，紫 = 其他 CLI |
| 飞得快、拖尾、溅火星 | 正在干活；下面一行是当前工具名，换工具时荡开一圈涟漪 |
| `thinking` | 模型在想 / 在生成 |
| 停住、琥珀色光环在呼吸 | `your turn`：一轮结束，等你回话 |
| 变暗、微沉 | 10 分钟没动静，`asleep 25m` |
| 绕着转的小月亮 / `+2` | 正在跑的子 agent |
| 绽开一圈光 | 新 agent 出现 |
| 散成火星 | agent 退出 |
| 没名字的黄绿小光点 | 野萤火虫，纯装饰，池子空了它们会更亮 |

## 数据从哪来

`host/pond.py` 每秒扫一次，只读、不联网：

- **Claude Code**：`~/.claude/projects/*/*.jsonl` 的最后几条消息判断状态，`<会话>/subagents/` 数子 agent
- **Codex**（CLI 和桌面 App 共用）：`~/.codex/sessions/**/rollout-*.jsonl`
- **其他 CLI**：进程表里挂着终端的 `gemini / opencode / aider / grok / cursor-agent / crush / amp`

会话文件 30 分钟内动过才显示；有对应进程活着时放宽到 12 小时（一个进程只认领同目录最新的一个会话）。
最多 8 只，干活的排前面。

## 快速开始

```bash
source scripts/env.sh                  # 复用 esp32-playground 里的 ESP-IDF 6.0.2
idf.py build && idf.py -p "$ESPPORT" flash

python3 host/pond.py --once            # 不连板子，看探测结果
python3 host/pond.py --demo            # 推一组假数据看效果
python3 host/pond.py -v                # 前台推送，同时打印板子日志（帧率）
python3 host/pond.py --shot out.png    # 截屏（先停掉常驻服务，串口只能一个进程用）

scripts/install-agent.sh               # 常驻：开机自启，跑的是 Application Support 下的副本
```

改完 `host/pond.py` 要重跑 `install-agent.sh` 才会生效。

## 实现要点

- 不用 LVGL，自写软件渲染（`main/gfx.c`）。两层：
  - **光层**：半分辨率 RGB888，光晕和火星画在这里，每帧乘 222/256 衰减，所以运动的光会自然拖出彗尾；合成时双线性放大
  - **fb**：整屏 RGB565。背景渐变 + 光层，4×4 有序抖动消除色阶断层；星星、光核、涟漪、倒影、文字直接画在这里
- 水面倒影：把上方画面纵向压扁 2.5 倍镜像下来，加两层正弦波纹和水色
- 字体：`scripts/gen-fonts.py` 把系统的 Avenir Next 和 New York Italic 栅格化成 4bpp（`main/fonts.c`，已入库）
- 推屏与渲染并行：核 1 渲染，画完一帧换字节拷到 PSRAM；核 0 分条经 DMA 发给 ST7789。5 只萤火虫时约 25ms/帧，锁 30fps

板级参数、串口截图协议、颜色字节序等坑沿用上个项目，见 `../ai-subscription-display/HANDOFF.md`。

串口协议：主机 → `@@{json}` / `@@SHOT` / `@@PING` / `@@BRIGHT 0-100`。

```json
{"v":1,"t":"23:41","a":[{"id":"709e3488","k":"claude","n":"agent-pond","s":"tool","x":"Bash","g":2,"c":0}]}
```

`s`：`tool` / `think` / `wait` / `sleep`；`x` 工具名；`g` 距上次动静秒数；`c` 子 agent 数。
