# StackChan 使用说明

StackChan 是石川真也（Shinya Ishikawa）开源的桌面机器人，M5Stack 以 K151 / K151-R 出货，
底座带一对 Feetech SCS 串行总线伺服作脖子。它跑在 M5Stack CoreS3 上，所以音频编解码直接复用
隔壁 `m5stack/core-s3/cores3_audio_codec.cc`，没有再抄一份。

**硬件**

| | |
|---|---|
| 主控 | M5Stack CoreS3（ESP32-S3，Octal PSRAM 关、Quad 开） |
| 显示 | ILI9342C，320×240，走 SPI；表情用 emote 动画 |
| 音频 | AW88298 功放 + ES7210 回采，和 CoreS3 一致 |
| 电源 | AXP2101 |
| 脖子 | 两个 Feetech SCS0009，串行总线，1 Mbaud |
| 底座 | PY32 I/O 扩展器，第 0 脚是伺服总电源（VM EN） |

> [!IMPORTANT]
> 伺服电源由底座的 PY32 第 0 脚控制，上电默认是断的。不先拉高这一脚，总线能发能收、
> 舵机也能报回位置，但**完全不会动** —— 这一条是这块板子最容易踩的坑。
> 拉高之后还要等约 200 ms，SCS0009 才醒。见 `EnableServoPower()`。

**编译**

```bash
python ./scripts/build.py m5stack/stackchan
python ./scripts/build.py m5stack/stackchan --language zh-CN --wake-word nihaoxiaozhi   # 安安 build
```

如需手动编译，请参考 `m5stack/stackchan/config.json` 修改 menuconfig 对应选项。

**烧录**

```bash
idf.py flash
```

> [!NOTE]
> 进入下载模式：长按复位按键（约 3 秒），直至内部指示灯亮绿色，松开按键。

**脖子工具**

| 工具 | 作用 |
|---|---|
| `self.neck.get_angles` | 读当前偏航 / 俯仰角，以及总线有没有应答 |
| `self.neck.set_angles` | 转到目标角并停住：待机时约 8 秒后回正中，对话中一直停到对话结束；偏航 −45..45，`duration_ms` 保留但不再使用 |
| `self.neck.set_torque` | 开关舵机扭矩 |
| `self.head.nod` | 点头，`count` 1–3（默认 1）；同意或说“是”时调用 |
| `self.head.shake` | 摇头，`count` 1–3（默认 1）；不同意或说“不”时调用 |

角度以中立姿态为零点，抬头为正。脖子不可用时 `self.head.*` 返回错误 “the neck is unavailable”；
扭矩关着时所有动头工具都拒绝执行；请求队列满时返回 “busy, try again”。
两次点头 / 摇头工具调用至少隔 1 秒，间隔更短的请求被忽略。

**行为规则**

头、灯环由 `BehaviorDirector`（`behavior_director.cc`）一个任务统一驱动，逻辑在 `behavior_core.cc`。
头的仲裁优先级从高到低：语音指令转头（`set_angles`）> 动作（点头 / 摇头）> 人脸追踪 > 保持不动。
高优先级结束后低优先级恢复；所有角度都过同一个限位函数（俯仰 5–85）。自动动作之间至少隔 4 秒，工具调用不受限。

| 状态 | 脖子 | 脸 | 灯环 | 唤醒 |
|---|---|---|---|---|
| 待机 | 慢速回正中后不动 | `neutral`（眨眼循环），偶尔小动作 | 关 | 开 |
| 听用户说 | 人脸追踪 | `listening` | 绿，常亮 | 开 |
| 她说话 | 追踪关，停在最后位置；可播动作 | 当前情绪动画 | 蓝，呼吸；强情绪用情绪色 | 开 |
| 连接中 / 配网 | 不动 | 官方默认 | 白，慢呼吸 | 按官方 |

说话时的情绪会触发轻动作：开心类点头、困惑类歪头、难过类慢抬头、生气 / 吃惊类快速摇头。
资源包里没有的情绪，脸退回 `neutral`。人脸追踪（P4）和待机小动作（P5）尚未实现，表里对应项目前表现为“不动”。

触摸、IMU、光照传感器这些另算，不在这块板子里。
