# esp32-26

立创·实战派 ESP32-S3（2.0" 320×240 屏）上的几个小项目，基于 ESP-IDF + LVGL，主机端用 Python 脚本经 USB 串口推送数据。

| 项目 | 说明 |
|---|---|
| [deck](deck/) | **桌面小面板**：首页磁贴 + 手势切换，下面几个项目都是它里面的应用，外加小屏版 htop。一套固件、一个主机进程，日常用这个 |
| [ai-subscription-display](ai-subscription-display/) | AI 用量面板：显示 Claude Code / Codex / DeepSeek / Grok 的订阅额度、重置倒计时和余额 |
| [agent-pond](agent-pond/) | Agent 夜池：把本机正在跑的 coding agent 画成一池萤火虫 |
| [magpie-live](magpie-live/) | magpie live：把 magpie 网关的实时请求画成往返飞行的喜鹊 |

各项目的构建与烧录方法见各自目录下的 README。下面三个老项目仍可单独烧录，它们的主机脚本对 deck 固件也照样有效。
