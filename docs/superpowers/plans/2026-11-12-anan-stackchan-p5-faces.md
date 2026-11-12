# 安安 Stack-chan P5：自己的脸和待机小动作 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 屏幕上显示用户自己的 21 个表情（全屏、无边框、会动），待机时偶尔播一个屏幕小动作，以后更新固件不用重新配网。

**Architecture:** 一个 Python 脚本把用户的 PNG 转成固件能播的 `.eaf` 动画（8 位调色板、RLE、320×240），同时生成 `emote.json` 和一个 C++ 头文件（待机小动作列表）。生成物提交进仓库，固件编译不依赖 Pillow。CMake 把官方素材包和本板的文件合到一个临时目录交给官方打包脚本。固件侧新增一个纯逻辑的待机计时器，由行为协调器驱动，通过显示对象播放动画。

**Tech Stack:** Python 3 + Pillow（仅脚本和它的测试），`unittest`；C++20 主机测试（CTest，`assert`）；ESP-IDF 6.1；GitHub Actions。

**Spec:** `docs/superpowers/specs/2026-11-12-anan-stackchan-firmware-design.md` 第 6 节。

**Spike 结果（2026-11-12，已在真机验证）：** 分支 `anan/p5-spike`（提交 248bc53）。全屏 320×240、8 位调色板、RLE、每 32 行一块的 `.eaf`，以 5 fps 播放，显示正常、颜色正确、眨眼自然、语音不卡。编码器对官方 10 个 `.eaf` 文件逐帧重编码，字节完全一致。参考实现在 `/home/claude/anan-spike/eaf.py` 和 `make_faces.py`（只读参考，不要原样照搬结构）。

## Global Constraints

- 只改这些路径：`main/boards/m5stack/stackchan/**`、`scripts/anan_faces/**`、`.github/**`、`docs/**`，以及 `main/CMakeLists.txt` 里 StackChan 专属的两段。不改其他板子，不改 `application.*`、`main/audio/**`。
- `.eaf` 格式固定：容器头 `0x89 "EAF"`、帧数、校验和（表和帧数据的字节和，32 位）、长度；帧表每项 8 字节（大小、偏移）；每帧以 `0x5A5A` 开头，`_S` 帧头，位深 8，块高 32，调色板 256×4 字节（B、G、R、A，A 固定 255），每块首字节 0 表示 RLE（计数、值 成对，计数 1–255）。文件最后一帧必须是 `_C` 尾帧，播放器按“表项数 − 1”算帧数。
- 画面 320×240；原图四边各裁 8% 后缩放（LANCZOS）；量化不抖动；同一个动画的所有帧共用一个调色板。
- 所有动画 fps 5。
- 素材文件名长度（含扩展名）≤ 31 字符（`CONFIG_MMAP_FILE_NAME_LENGTH=32`）。
- assets 分区 8 MB；官方构建会校验大小，超出必须失败而不是截断。
- 纯逻辑 C++ 文件不包含任何 ESP-IDF 头文件；新 C++ 代码在命名空间 `anan`。
- 非主任务回调不直接改状态，投递到协调器队列。
- 只对改动的 C/C++ 文件跑 `.clang-format`；本机 clang-format 18 不认仓库配置里的 `AlwaysBreakAfterReturnType: ExceptShortType`，用去掉这一行的临时副本格式化新文件。

## Review Focus

1. 某个情绪的 PNG 缺失 → 脚本报错并指出文件名，不生成半套素材。（Task 1）
2. 原图尺寸不是 4:3 或四角裁完不是背景色（边框没裁干净）→ 脚本报错。（Task 1）
3. 纯黑像素（0,0,0）→ 调色板 A=255，不被播放器当成透明。（Task 1）
4. 待机小动作播放中被唤醒 → 立即停止，不在对话中途切回 `neutral` 覆盖对话表情。（Task 3）
5. 待机时服务器或别的路径设置了表情 → 小动作计时重新开始，不和它抢屏幕。（Task 3）

---

### Task 1: 素材脚本和生成物

**Files:**
- Create: `scripts/anan_faces/eaf.py`、`scripts/anan_faces/build_faces.py`、`scripts/anan_faces/README.md`
- Create: `scripts/anan_faces/tests/test_eaf.py`、`scripts/anan_faces/tests/test_build_faces.py`
- Create: `main/boards/m5stack/stackchan/assets-src/anan/<21 个情绪名>.png`（裁边后 640×480 的无损 PNG，由脚本的 `prepare` 子命令从原图生成；原图在 `/home/claude/anan-spike/src/`）
- Create（生成物）: `main/boards/m5stack/stackchan/emoji/*.eaf`、`main/boards/m5stack/stackchan/assets/320_240/emote.json`、`main/boards/m5stack/stackchan/anan_idle_actions.inc`

**Interfaces:**
- Produces:
  - `eaf.py`: `read(path) -> (tag: bytes, frames: list[bytes])`、`parse_frame(frame) -> dict`、`build_frame(w, h, palette: bytes, pixels: bytes, block_height=32) -> bytes`、`write(path, frames: list[bytes]) -> int`（相同帧只存一份，表项指向同一偏移；自动追加 `_C` 尾帧）。
  - `build_faces.py prepare <原图目录> <assets-src 目录>`：裁 8%、缩到 640×480、存 PNG。
  - `build_faces.py build <assets-src 目录> <板子目录>`：生成全部 `.eaf`、`emote.json`、`anan_idle_actions.inc`。输出确定：同样输入连跑两次，字节一致。
  - `emote.json` 条目：21 个情绪名各一条 + `idle` + 每个待机小动作一条。
  - `anan_idle_actions.inc` 每行一条：`ANAN_IDLE_ACTION("idle_sleepy", 3200)`（名字，时长毫秒 = 帧数 × 200）。

21 个情绪名：`neutral happy laughing funny sad angry crying loving embarrassed surprised shocked thinking winking cool relaxed delicious kissy confident sleepy silly confused`。

动画定义（帧序列按 5 fps 写）：

| 条目 | 文件 | loop | 帧序列 |
|---|---|---|---|
| `neutral`、`idle` | `anan_neutral.eaf` | true | neutral×18, relaxed×1, neutral×10, relaxed×1 |
| 其余 20 个情绪 `<name>` | `anan_<name>.eaf` | true | 原图×8, 下移 1 像素×2, 下移 2 像素×8, 下移 1 像素×2 |
| `idle_sleepy` | `anan_idle_sleepy.eaf` | false | neutral×2, sleepy×12, neutral×2 |
| `idle_wink` | `anan_idle_wink.eaf` | false | neutral×2, winking×5, neutral×2 |
| `idle_delicious` | `anan_idle_delicious.eaf` | false | neutral×2, delicious×8, neutral×2 |

“下移 n 像素”：整幅图向下平移 n 行，顶部空出的行用原图第 0 行填充，底部多出的行丢弃。

- [ ] **Step 1: 写失败的测试。** `test_eaf.py`：
  - `test_roundtrip_small_frame`：8×40 的像素和调色板，`build_frame` → `parse_frame` 得回同样的 w、h、pixels、palette；块数为 2。
  - `test_rle_run_longer_than_255_is_split`：一行 600 个相同值 → 解码后仍是 600 个。
  - `test_write_dedups_identical_frames_and_appends_trailer`：写入 [A, B, A] → `read` 得 4 帧，第 0、2 帧字节相同，最后一帧以 `5a5a5f43` 开头；文件大小小于存三份的大小。
  - `test_container_checksum_and_length`：校验和等于第 16 字节之后所有字节的和（32 位），长度字段等于文件长度 − 16。
  - `test_black_pixel_palette_alpha_is_255`（在 `test_build_faces.py`）：纯黑图量化后调色板里黑色项第 4 字节为 255。
  - `test_build_faces.py`（用临时目录里生成的 21 张纯色 640×480 假图）：`test_build_creates_all_files`（24 个 `.eaf`、`emote.json` 有 21+1+3=25 条且每条的 `src` 文件存在、名字长度 ≤ 31）；`test_build_is_deterministic`；`test_missing_face_fails_with_name`；`test_neutral_sequence_is_30_frames_with_2_unique`；`test_emotion_sequence_is_20_frames_with_3_unique`（用带横条纹的假图，否则平移后相同）；`test_idle_inc_lines`（3 行，时长 3200、1800、2400）；`test_prepare_rejects_non_4_3` 和 `test_prepare_rejects_border_left_in_corners`（四角颜色与中心背景差太大）。
- [ ] **Step 2:** 运行 `python3 -m unittest discover -s scripts/anan_faces/tests -v`。预期：导入失败。
- [ ] **Step 3:** 实现 `eaf.py` 和 `build_faces.py`。`_C` 尾帧用这 18 字节加注释串：`5a5a 5f43 00 000000000001 000000` + 4 字节小端长度 + ASCII 文本 `anan faces 1`（与官方文件的尾帧结构一致：官方为同样的头 + 长度 0x1f + `online convert 0.1.4_2025-09-01`）。
- [ ] **Step 4:** 运行测试。预期：全部通过。
- [ ] **Step 5:** 运行 `prepare` 和 `build` 生成真实素材。检查：24 个 `.eaf`；总大小打印出来并写进报告（预期 3–4.5 MB）；用 `eaf.read` + `parse_frame` 把 `anan_happy.eaf` 第 0 帧还原成 PNG 存到 `/tmp/anan-check/` 并目视确认是开心脸、无边框。
- [ ] **Step 6:** 写 `scripts/anan_faces/README.md`（中文）：怎么换图、怎么加一个待机小动作（放图、在脚本的动作表加一行、重跑 `build`、提交生成物）。
- [ ] **Step 7:** 提交（脚本和测试一个提交，素材源图和生成物一个提交）。

### Task 2: 构建接入

**Files:**
- Modify: `main/CMakeLists.txt`（StackChan 分支设置 `EMOTE_EXTERNAL_PATH`；新增 StackChan 素材暂存块）
- Create: `main/boards/m5stack/stackchan/assets/320_240/layout.json`
- Modify: `.github/workflows/anan-stackchan.yml`（host-tests job 加脚本测试；firmware job 多上传两个文件）
- Create: `docs/anan/flashing.md`
- Modify: `main/boards/m5stack/stackchan/README.md`

**Interfaces:**
- Consumes: Task 1 的 `emoji/*.eaf`、`assets/320_240/emote.json`。
- Produces: CI artifact `anan-stackchan-firmware` 里除 `merged-binary.bin` 外还有 `xiaozhi.bin` 和 `configured_assets.bin`。

- [ ] **Step 1:** 按 `anan/p5-spike` 提交 248bc53 的做法改 `main/CMakeLists.txt`（`git show 248bc53 -- main/CMakeLists.txt` 可看），去掉 “SPIKE” 字样，注释写清楚目的。暂存目录内容：官方 `emoji_large/` 全部、官方 `320_240/config.json`、本板 `layout.json` 和 `emote.json`、本板 `emoji/*.eaf`。
- [ ] **Step 2:** `layout.json`：以官方 `320_240/layout.json` 为底（CI 里在 `managed_components/espressif2022__esp_emote_assets/320_240/layout.json`；参考副本在 `/tmp/emote/esp_emote_assets/320_240/layout.json`），只改 `eye_anim`：`align` 为 `GFX_ALIGN_CENTER`，`x` 0，`y` 0，`anim.mirror` 为 `"false"`。其余元素不动。
- [ ] **Step 3:** workflow：`host-tests` 加一步 `pip install pillow` 和 `python3 -m unittest discover -s scripts/anan_faces/tests -v`；`firmware` 的上传路径加 `build/xiaozhi.bin`、`build/configured_assets.bin`。
- [ ] **Step 4:** `docs/anan/flashing.md`（中文）两种烧法，命令用 `python3 -m esptool --chip esp32s3 -p <串口> -b 460800`：
  - 首次或换分区后：`write_flash 0x0 merged-binary.bin`（会清掉 Wi-Fi 配置）。
  - 日常更新（保留 Wi-Fi 和绑定）：`write_flash 0x20000 xiaozhi.bin 0x800000 configured_assets.bin`。
  - 写明：不要用网页烧录工具（2026-11-12 实测它把 8 MB 之后的数据写坏）；怎么找串口名；怎么确认文件大小。
- [ ] **Step 5: 验证。** 本地跑脚本测试；YAML 能解析。CI 由控制方推送后确认：日志里 `External Path` 指向暂存目录、`Loaded emoji config: 25 entries`、合并固件生成、分区大小校验通过。
- [ ] **Step 6:** 提交。

### Task 3: 待机小动作（固件）

**Files:**
- Create: `main/boards/m5stack/stackchan/idle_actions.h`、`idle_actions.cc`（纯逻辑）
- Test: `main/boards/m5stack/stackchan/host_tests/test_idle_actions.cc`
- Modify: `host_tests/CMakeLists.txt`、`director_logic.{h,cc}`、`behavior_director.{h,cc}`、`anan_display.h`、`stackchan.cc`、板子 `README.md`

**Interfaces:**
- Consumes: `Outputs.idle_actions_enabled`（已有，待机且头部无人占用时为真）；`anan_idle_actions.inc`（Task 1）；`AnanDisplay::SetEmotion`。
- Produces:

```cpp
namespace anan {
struct IdleAction { const char* name; uint32_t duration_ms; };
inline constexpr int64_t kIdleGapMinMs = 20000;
inline constexpr int64_t kIdleGapMaxMs = 45000;

class IdleActions {
public:
    // actions 可以为空；rng 返回 [0, n) 的整数，测试里注入确定的序列。
    IdleActions(std::span<const IdleAction> actions, std::function<uint32_t(uint32_t n)> rng);
    struct Step { const char* play = nullptr; bool restore = false; };
    // 每个协调器周期调用一次。enabled = idle_actions_enabled。
    // play 非空：现在开始播这个动画。restore 为真：动作播完，切回 "neutral"。
    Step Tick(bool enabled, int64_t now_ms);
    // 别的来源刚设置了表情（服务器、状态切换）：取消正在播的动作，计时重新开始。不要求 restore。
    void OnExternalEmotion(int64_t now_ms);
    bool playing() const;
};
}
```

规则：
- `enabled` 由假变真时开始计时，间隔取 `kIdleGapMinMs + rng(kIdleGapMaxMs - kIdleGapMinMs + 1)`。
- 到时间：从列表里选一个（`rng(n)`），如果和上一次相同且 n > 1，取下一个（循环）。返回 `play`。
- 动作开始后经过它的 `duration_ms`：返回 `restore = true`，重新计时。
- 播放中 `enabled` 变假（被唤醒、手动转头等）：停止，不返回 `restore`（对话的表情由对话自己设置），`playing()` 变假。
- `enabled` 为假时不计时。列表为空时永远不播。

接线：
- `AnanDisplay` 区分两种设置表情的来源：外部调用的 `SetEmotion`（转发给协调器，并通知“外部表情”）和协调器内部播放待机动作用的新方法 `PlayIdleAnimation(const char* name)`（直接调父类 `SetEmotion`，不转发、不通知）。`restore` 时调 `PlayIdleAnimation("neutral")`。
- 协调器任务每周期用 `Outputs.idle_actions_enabled` 调 `Tick`。显示调用通过 `Application::Schedule` 切到主任务执行（检查仓库里其他地方从非主任务调用 `Display` 的做法，照做）。
- 动作表：`static constexpr anan::IdleAction kIdleActions[] = {` + `#define ANAN_IDLE_ACTION(n, d) {n, d},` + `#include "anan_idle_actions.inc"` + `};`。
- `rng` 用 `esp_random()`。

- [ ] **Step 1: 写失败的测试**（rng 注入固定序列）：`no_action_before_min_gap`、`plays_after_gap_and_restores_after_duration`、`does_not_repeat_same_action_twice_in_a_row`、`single_action_list_may_repeat`、`empty_list_never_plays`、`disabled_while_playing_stops_without_restore`、`timer_restarts_when_reenabled`（禁用再启用后要重新等完整间隔）、`external_emotion_cancels_and_restarts_timer`、`gap_is_within_20_to_45_seconds`（rng 返回 0 和最大值两种情况）。再在 `test_director_logic.cc` 或新测试里加一条：待机动作播放中进入 Listening → 不产生 restore。
- [ ] **Step 2:** 构建主机测试。预期：编译失败。
- [ ] **Step 3:** 实现并接线。
- [ ] **Step 4:** 主机测试全部通过（构建目录放仓库外）。
- [ ] **Step 5:** 更新板子 README：待机小动作的行为、怎么加动作（指向 `scripts/anan_faces/README.md`）。
- [ ] **Step 6:** 提交。固件编译由控制方推送后经 CI 确认。

### 用户闸门（实机）

| 检查 | 通过条件 |
|---|---|
| 21 个表情 | 让她表达不同情绪，脸会换，全屏无边框，轻微上下浮动不显得抖 |
| 待机 | 平静脸眨眼；每 20–45 秒出现一次打瞌睡、眨一只眼或舔嘴，播完回到平静脸 |
| 唤醒 | 小动作播到一半喊她，立即进入对话，表情正常 |
| 语音 | 不卡顿 |
| 日常更新烧法 | 按 `docs/anan/flashing.md` 的第二种烧法更新后，不用重新配网 |
