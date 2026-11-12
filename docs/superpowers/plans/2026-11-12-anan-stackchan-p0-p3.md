# 安安 Stack-chan 固件 P0–P3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在官方 StackChan 板子上做出：稳定唤醒、待机头不动、灯环随状态变、她说话时点头摇头。

**Architecture:** 所有新代码在 `main/boards/m5stack/stackchan/`。纯逻辑（状态表、头部仲裁、动作关键帧、灯色）写成不依赖 ESP-IDF 的 C++，在主机上单测；薄适配层把它接到舵机、PY32 和设备状态。板子通过两个现成的钩子拿到事件：`Board::GetLed()` 返回的 `Led::OnStateChanged()`（状态），以及 `Display::SetEmotion()` 的子类重载（情绪）。

**Tech Stack:** ESP-IDF ≥ 6.0.1（容器 `espressif/idf:release-v6.1`），C++20，CMake + CTest（主机测试，纯 `assert`），GitHub Actions。

**Spec:** `docs/superpowers/specs/2026-11-12-anan-stackchan-firmware-design.md`

**范围说明：** 本计划只覆盖 spec 的 P0–P3。P4（人脸追踪）和 P5（脸素材、待机小动作）各自先做验证实验，结果出来后再各写一份计划。本计划里协调器已经为它们留好输入口（`OnFaceTarget`、`idle_actions_enabled`）。

## Global Constraints

- ESP-IDF ≥ 6.0.1；CI 用 `espressif/idf:release-v6.1`。
- 只改这些路径：`main/boards/m5stack/stackchan/**`、`.github/**`、`docs/**`。不改其他板子，不改 `application.*`、`main/audio/**`。必须改核心时停下来报告，不自行决定。
- 俯仰角永远在 `SAFE_PITCH_MIN`(5)–`SAFE_PITCH_MAX`(85) 内；偏航沿用 `ServoNeck` 现有限位。
- 灯环亮度上限 40%。
- 自动动作最小间隔 4000 ms；工具调用不受此限制。
- 非主任务回调里不直接改状态，一律投递到协调器自己的队列。
- 只对改动过的 C/C++ 文件跑仓库的 `.clang-format`。
- 分支 `anan/main`；每个 Task 一个 PR，PR 描述含 Testing / Risk / How to revert。
- 新文件命名空间 `anan`。

## Review Focus

1. 情绪名未知、为空或为 `nullptr` → 不做动作，不崩溃，脸回落到 `neutral`。（Task 4）
2. 状态在 100 ms 内来回切换（听 ↔ 说）→ 不重复起动作，头不抽动。（Task 4）
3. 手动转头工具在动作播放中到来 → 手动优先，动作取消，动作结束后的“回原位”不得覆盖手动位置。（Task 4）
4. `self.head.nod` / `shake` 的次数为 0、负数或 99 → 夹到 1–3。（Task 5）
5. 舵机或 PY32 初始化失败 → 协调器照常运行，对应命令变空操作，其余功能不受影响。（Task 7）

---

## File Structure

| 文件 | 责任 | 依赖 IDF |
|---|---|---|
| `servo_neck.{h,cc}` | 从 `stackchan.cc` 原样拆出的 `ServoNeck` | 是 |
| `behavior_core.{h,cc}` | 状态表、头部仲裁、情绪 → 动作、节流 | 否 |
| `head_gestures.{h,cc}` | 动作关键帧表 | 否 |
| `led_color.{h,cc}` | 状态 / 情绪 → 颜色和效果、亮度上限、RGB565 打包 | 否 |
| `led_ring.{h,cc}` | `Led` 子类；效果任务；写 PY32 LED RAM | 是 |
| `behavior_director.{h,cc}` | 队列 + 任务；把核心的输出发给脖子和灯；注册 MCP 工具 | 是 |
| `anan_display.h` | `EmoteDisplay` 子类，转发 `SetEmotion` | 是 |
| `host_tests/CMakeLists.txt`、`host_tests/test_*.cc` | 主机单测 | 否 |
| `.github/workflows/anan-stackchan.yml` | 主机单测 + 编译固件 + 上传 | — |

板子目录的 `*.cc` 由 `main/CMakeLists.txt:999` 的 glob 自动编入固件；`host_tests/` 是子目录，不会被编入。

---

### Task 1: 仓库、CI 和唤醒基线（P0）

**Files:**
- Create: `.github/workflows/anan-stackchan.yml`
- Create: `.github/pull_request_template.md`
- Create: `docs/superpowers/specs/2026-11-12-anan-stackchan-firmware-design.md`（从会话复制）
- Create: `docs/superpowers/plans/2026-11-12-anan-stackchan-p0-p3.md`（本文件）
- Create: `docs/anan/wake-baseline.md`

**Interfaces:**
- Produces: 仓库 `DogcanAmee/xiaozhi-esp32`，分支 `anan/main`；workflow artifact `anan-stackchan-firmware`（合并固件 `merged-binary.bin`）；`docs/anan/wake-baseline.md` 里记录的唤醒词类型。

- [ ] **Step 1:** fork `78/xiaozhi-esp32` 到 `DogcanAmee/xiaozhi-esp32`。如果 GitHub 拒绝（同网络已有 fork），新建空的公开仓库 `anan-stackchan`，推送官方 main 历史，并把官方仓库加为 `upstream` remote。从 main 建分支 `anan/main`。
- [ ] **Step 2:** 写 `.github/workflows/anan-stackchan.yml`。触发：`push` 到 `anan/**`、`pull_request` 到 `anan/main`、`workflow_dispatch`。两个 job：
  - `host-tests`：`ubuntu-latest`，`cmake -S main/boards/m5stack/stackchan/host_tests -B build-host && cmake --build build-host && ctest --test-dir build-host --output-on-failure`。目录不存在时跳过（Task 4 之前）。
  - `firmware`：容器 `espressif/idf:release-v6.1`，`python scripts/build.py m5stack/stackchan`，然后 `grep -E "WAKE_WORD|SR_WN|SR_MN|USE_DEVICE_AEC" sdkconfig` 打到日志，上传 `releases/` 下的产物为 `anan-stackchan-firmware`。
  - `permissions: contents: read`。
- [ ] **Step 3:** 写 PR 模板，三个小节：Testing、Risk（low / medium / high + 理由）、How to revert。
- [ ] **Step 4:** 提交 spec 和本计划，推送，开 PR。
- [ ] **Step 5: 验证。** workflow 两个 job 通过；artifact 里有合并固件；日志里能看到唤醒相关的 sdkconfig 行。把这些行原样记进 `docs/anan/wake-baseline.md`。
- [ ] **Step 6: 用户闸门。** 用户烧录 artifact，确认能对话、官方动画脸正常。按下表各喊 10 次并回报，写进 `wake-baseline.md`：

  | 时机 | 成功次数 /10 |
  |---|---|
  | 待机 | |
  | 她说话中 | |
  | 她刚说完 2 秒内 | |

### Task 2: 唤醒配置（P1）

**Files:**
- Modify: `main/boards/m5stack/stackchan/config.json`（`sdkconfig_append`）
- Modify: `docs/anan/wake-baseline.md`（加“改后”一列）

**Interfaces:**
- Consumes: Task 1 记录的当前唤醒词类型。

- [ ] **Step 1:** 在 `sdkconfig_append` 末尾追加：`CONFIG_USE_AFE_WAKE_WORD=y`、`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`、`CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y`。如果 Task 1 的日志显示 `build.py` 已经选了 AFE 唤醒词，只加第三行，并在 PR 里写明。
- [ ] **Step 2: 验证。** CI `firmware` job 通过；日志里的 sdkconfig 行显示 `CONFIG_USE_AFE_WAKE_WORD=y`、`CONFIG_WAKE_WORD_DETECTION_IN_LISTENING=y`，且没有 `CONFIG_USE_CUSTOM_WAKE_WORD=y`。
- [ ] **Step 3:** 提交，开 PR（风险：medium，改了音频相关配置）。
- [ ] **Step 4: 用户闸门。** 用户烧录，重复 Task 1 的 30 次测试。通过条件：每行 ≥ 9/10。
  - 如果“她说话中”她被自己的声音误唤醒：把 `她说话时关唤醒、点屏幕打断` 作为退路单开一个 Task，停下来和用户确认，因为这需要动 `application.cc`。

### Task 3: 拆出 ServoNeck（P2，纯搬运）

**Files:**
- Create: `main/boards/m5stack/stackchan/servo_neck.h`、`servo_neck.cc`
- Modify: `main/boards/m5stack/stackchan/stackchan.cc`（删除类定义，改为 `#include "servo_neck.h"`）

**Interfaces:**
- Produces: `class ServoNeck`，公开接口与现状完全一致：`bool Begin()`、`void StartMove(int yaw_deg, int pitch_deg, uint32_t duration_ms)`、`void InvalidateRequests()`、`bool SetTorque(bool on)`、`void Tick()`、`static constexpr uint32_t MOTION_TICK_MS = 20`。新增一个只读方法 `bool IsMoving() const`（返回当前是否有未完成的缓动）和 `void GetTarget(int& yaw_deg, int& pitch_deg) const`。

- [ ] **Step 1:** 把 `stackchan.cc` 里 `class ServoNeck` 整段移到新文件。不改任何逻辑、常量和注释。加上面两个只读方法。
- [ ] **Step 2: 验证。** `git diff --stat` 显示 `stackchan.cc` 净减少的行数约等于 `servo_neck.*` 新增行数；CI `firmware` job 通过。
- [ ] **Step 3:** 提交，开 PR（风险：low）。不需要单独烧录，和 Task 7 一起验收。

### Task 4: behavior_core（纯逻辑）

**Files:**
- Create: `main/boards/m5stack/stackchan/behavior_core.h`、`behavior_core.cc`
- Create: `main/boards/m5stack/stackchan/host_tests/CMakeLists.txt`
- Test: `main/boards/m5stack/stackchan/host_tests/test_behavior_core.cc`

**Interfaces:**
- Produces（全部在 `namespace anan`）：

```cpp
enum class Phase { Idle, Listening, Speaking, Other };
enum class HeadOwner { Hold, Tracking, Gesture, Manual };
enum class Gesture { None, Nod, Shake, Tilt, Bow, QuickShake };
struct Pose { int yaw_deg; int pitch_deg; };

inline constexpr int kPitchMin = 5, kPitchMax = 85;      // = SAFE_PITCH_MIN/MAX
inline constexpr int kYawMin = -45, kYawMax = 45;         // 协调器自己的保守范围
inline constexpr Pose kRestPose{0, 5};                    // = BOOT_INIT_YAW/PITCH_DEG
inline constexpr int64_t kAutoGestureGapMs = 4000;
inline constexpr int64_t kPhaseSettleMs = 150;            // 状态稳定这么久才生效
inline constexpr int64_t kManualHoldMs = 8000;            // 手动转头后占用头的时长
inline constexpr uint32_t kReturnToRestMs = 1500;

Pose ClampPose(Pose p);
Gesture GestureForEmotion(std::string_view emotion);      // 未知 → None

struct Outputs {
    std::optional<Pose> move_to;        // 有值时让脖子缓动到这里
    uint32_t move_ms = 0;
    Gesture start_gesture = Gesture::None;
    int gesture_count = 1;
    bool cancel_gesture = false;
    bool tracking_enabled = false;      // P4 用
    bool idle_actions_enabled = false;  // P5 用
    Phase led_phase = Phase::Other;     // 交给 led_color
    std::string led_emotion;            // 空 = 用状态色
};

class BehaviorCore {
public:
    void OnPhase(Phase phase, int64_t now_ms);
    void OnEmotion(std::string_view emotion, int64_t now_ms);
    void OnManualMove(Pose target, int64_t now_ms);
    void OnGestureRequest(Gesture g, int count, int64_t now_ms);   // 来自工具，不节流
    void OnFaceTarget(Pose target, int64_t now_ms);
    void OnGestureFinished(int64_t now_ms);
    Outputs Poll(int64_t now_ms);       // 每 20 ms 调一次；返回自上次以来要执行的命令
    HeadOwner owner() const;
    Phase phase() const;
};
```

情绪 → 动作表：`happy`、`laughing`、`confident` → `Nod`；`confused`、`thinking` → `Tilt`；`sad`、`crying` → `Bow`；`angry`、`shocked` → `QuickShake`；其余 → `None`。

行为规则：
- 仲裁优先级 `Manual > Gesture > Tracking > Hold`。
- `Idle`：进入时 `move_to = kRestPose`、`move_ms = kReturnToRestMs`，之后不再发任何头部命令；`idle_actions_enabled = true`。
- `Listening`：`tracking_enabled = true`；`OnFaceTarget` 的目标经 `ClampPose` 后作为 `move_to`。
- `Speaking`：`tracking_enabled = false`；不发 `move_to`（头停在原位）；`OnEmotion` 映射出的动作在距上次自动动作 ≥ `kAutoGestureGapMs` 时才发。
- 状态切换要稳定 `kPhaseSettleMs` 才生效；稳定前 `Poll` 维持旧状态的输出。
- `OnManualMove` 立即取消正在播的动作（`cancel_gesture = true`），占用头 `kManualHoldMs`；期间自动动作和追踪都不发。
- 动作结束后的回位由 `head_gestures` 的关键帧自己完成，核心不再额外发 `move_to`。

- [ ] **Step 1:** 写 `host_tests/CMakeLists.txt`：C++20，把 `../behavior_core.cc` 和各 `test_*.cc` 编成独立可执行文件并 `add_test`。测试用 `<cassert>`，`main()` 里顺序调用各用例。
- [ ] **Step 2: 写失败的测试**（`test_behavior_core.cc`），每条一个函数：

```cpp
void clamp_pose_limits_pitch_and_yaw();        // {90,100}→{45,85}; {-90,0}→{-45,5}
void emotion_table_maps_known_names();          // happy→Nod, thinking→Tilt, sad→Bow, angry→QuickShake
void unknown_or_empty_emotion_maps_to_none();   // "", "banana" → None
void idle_returns_to_rest_once_then_is_silent();// 进 Idle 后首个 Poll 有 move_to==kRestPose/1500；之后 10 s 内的 Poll 全无 move_to、无动作
void idle_enables_idle_actions_only();          // idle_actions_enabled 真，tracking_enabled 假
void listening_enables_tracking_and_follows_face(); // OnFaceTarget({20,30}) → move_to=={20,30}
void speaking_disables_tracking_and_holds();    // OnFaceTarget 被忽略，无 move_to
void speaking_emotion_starts_gesture();         // OnEmotion("happy") → start_gesture==Nod
void auto_gestures_are_throttled_4s();          // t=0 happy→Nod；t=3999 happy→None；t=4000 happy→Nod
void tool_gesture_ignores_throttle();           // OnGestureRequest 紧跟自动动作仍然发出
void manual_move_cancels_gesture_and_wins();    // 动作中 OnManualMove → cancel_gesture 真，move_to==目标，owner()==Manual
void manual_hold_blocks_auto_gesture_and_tracking(); // 8 s 内 happy/OnFaceTarget 无输出；8 s 后恢复
void phase_flapping_within_150ms_is_ignored();  // Speaking→Listening→Speaking 各隔 50 ms：不发新动作、不发 move_to
void emotion_outside_speaking_does_not_gesture(); // Idle/Listening 下 OnEmotion("happy") → None
void led_phase_and_emotion_follow_inputs();     // led_phase 跟随稳定后的状态；Speaking 下 led_emotion=="angry"
```

- [ ] **Step 3:** 运行 `cmake -S main/boards/m5stack/stackchan/host_tests -B build-host && cmake --build build-host`。预期：编译失败（`behavior_core.h` 不存在）。
- [ ] **Step 4:** 实现 `behavior_core.{h,cc}`。只用标准库，不包含任何 IDF 头文件。
- [ ] **Step 5:** 运行 `cmake --build build-host && ctest --test-dir build-host --output-on-failure`。预期：全部通过。
- [ ] **Step 6:** 提交，开 PR（风险：low）。

### Task 5: head_gestures（纯逻辑）

**Files:**
- Create: `main/boards/m5stack/stackchan/head_gestures.h`、`head_gestures.cc`
- Test: `main/boards/m5stack/stackchan/host_tests/test_head_gestures.cc`
- Modify: `host_tests/CMakeLists.txt`（加新测试）

**Interfaces:**
- Consumes: `anan::Pose`、`anan::Gesture`、`anan::ClampPose`（Task 4）。
- Produces:

```cpp
struct Keyframe { Pose pose; uint32_t duration_ms; };
int ClampGestureCount(int count);                                   // 夹到 1..3
std::vector<Keyframe> BuildGesture(Gesture g, Pose base, int count); // None → 空
```

幅度和时长（相对 `base`）：

| 动作 | 关键帧（每次重复） | 结尾 |
|---|---|---|
| `Nod` | pitch +10 / 220 ms → pitch +0 / 220 ms | 回 `base` |
| `Shake` | yaw −12 / 200 ms → yaw +12 / 300 ms | 回 `base` / 200 ms |
| `QuickShake` | yaw −8 / 120 ms → yaw +8 / 160 ms | 回 `base` / 120 ms |
| `Tilt` | yaw +10, pitch +6 / 400 ms → 停 600 ms | 回 `base` / 400 ms；忽略 `count` |
| `Bow` | pitch +8 / 600 ms → 停 500 ms | 回 `base` / 600 ms；忽略 `count`（这台机器俯仰下限是 5 度，不能低头，所以用一次慢抬头代替） |

每个关键帧的姿态都过 `ClampPose`。俯仰增量为正是因为静止姿态的俯仰就是下限 5 度，向下没有行程。如果 `base.pitch_deg + 增量 > kPitchMax`，该帧改用负的同样幅度。

- [ ] **Step 1: 写失败的测试：**

```cpp
void none_gesture_is_empty();
void every_gesture_ends_at_base();             // 对 5 种动作、base={0,5} 和 {30,40}，末帧 pose==base
void all_keyframes_within_limits();            // base 取四个角 {±45, 5/85}，所有帧在限位内
void nod_repeats_by_count();                   // count=2 的帧数 == 2×count=1 的帧数
void count_is_clamped_1_to_3();                // ClampGestureCount(0)==1, (-5)==1, (99)==3
void tilt_and_bow_ignore_count();              // count=3 与 count=1 帧数相同
void durations_are_nonzero();
```

- [ ] **Step 2:** 运行 `cmake --build build-host`。预期：编译失败。
- [ ] **Step 3:** 实现。
- [ ] **Step 4:** 运行 `ctest --test-dir build-host --output-on-failure`。预期：全部通过。
- [ ] **Step 5:** 提交，开 PR（风险：low）。

### Task 6: 灯环

**Files:**
- Create: `main/boards/m5stack/stackchan/led_color.h`、`led_color.cc`（纯逻辑）
- Create: `main/boards/m5stack/stackchan/led_ring.h`、`led_ring.cc`
- Create: `main/boards/m5stack/stackchan/py32_io_expander.h`（把 `class Py32IoExpander` 从 `stackchan.cc` 原样移出，header-only，加 `WriteLeds`）
- Modify: `main/boards/m5stack/stackchan/stackchan.cc`（改为 include 新头文件。本任务不改 `GetLed()`，那是 Task 7 的事）
- Test: `main/boards/m5stack/stackchan/host_tests/test_led_color.cc`

**Interfaces:**
- Consumes: `anan::Phase`（Task 4）；`Py32IoExpander` 现有的 `REG_LED_CFG`(0x24)、`REG_LED_RAM_START`(0x30)、`SafeWriteRaw`。
- Produces:

```cpp
// led_color.h
enum class LedEffect { Off, Solid, Breathe, FadeOut };
struct Rgb { uint8_t r, g, b; };
struct LedCmd { LedEffect effect; Rgb color; };
inline constexpr int kLedCount = 12;
inline constexpr int kMaxBrightnessPct = 40;
LedCmd LedForState(Phase phase, std::string_view emotion);
Rgb ApplyBrightness(Rgb c, int pct);          // pct 先夹到 0..kMaxBrightnessPct
uint16_t PackRgb565(Rgb c);
int BreatheLevelPct(int64_t t_ms);            // 周期 2400 ms，范围 8..kMaxBrightnessPct

// py32_io_expander.h, Py32IoExpander
bool WriteLeds(const uint16_t* rgb565, size_t count);   // 写 LED RAM 并 latch；失败返回 false

// led_ring.h
class LedRing : public Led {
public:
    LedRing(Py32IoExpander* py32, std::function<void()> on_state_changed);
    void OnStateChanged() override;           // 只调用回调，不自己决定颜色
    void Apply(const anan::LedCmd& cmd);      // 由协调器调用
};
```

颜色表：`Idle` → `Off`；`Listening` → `Solid` 绿 `{0,200,60}`；`Speaking` 无强情绪 → `Breathe` 蓝 `{30,90,255}`；`Other` → `Breathe` 白 `{180,180,180}`。`Speaking` 下的强情绪（`Breathe`）：`angry` 红 `{255,40,30}`、`happy`/`laughing` 黄 `{255,190,40}`、`sad`/`crying` 深蓝 `{40,60,200}`、`loving` 粉 `{255,90,150}`。从非 `Idle` 进入 `Idle` 时先 `FadeOut`（600 ms）再 `Off`，这个过渡由 `LedRing` 处理。

- [ ] **Step 1: 写失败的测试：**

```cpp
void idle_is_off();
void listening_is_solid_green();
void speaking_default_is_breathing_blue();
void speaking_strong_emotions_use_emotion_color();   // 上表四组逐个断言
void speaking_weak_or_unknown_emotion_uses_blue();   // "thinking", "", "banana"
void emotion_ignored_outside_speaking();             // Listening + "angry" 仍是绿
void brightness_never_exceeds_40pct();               // ApplyBrightness({255,255,255},100) 各通道 ≤ 102
void breathe_stays_within_8_to_40();                 // 取 0..4800 ms 每 20 ms 一点
void pack_rgb565_known_values();                     // {255,0,0}→0xF800, {0,255,0}→0x07E0, {0,0,255}→0x001F
```

- [ ] **Step 2:** 运行构建。预期：编译失败。
- [ ] **Step 3:** 实现 `led_color.{h,cc}`。运行 `ctest`，预期全部通过。
- [ ] **Step 4:** 实现 `Py32IoExpander::WriteLeds` 和 `LedRing`。`LedRing` 起一个低优先级任务（栈 3072，优先级 2），每 40 ms 按当前效果算一帧并 `WriteLeds`；`Off` 和 `Solid` 只在命令变化时写一次。`py32 == nullptr` 时 `Apply` 直接返回。旧 fork 的 LED 写入顺序见 `DogcanAmee/stackchan-xiaozhi-firmware` 的 `m5stack_core_s3.cc`（搜 `UpdateLedsFromEmotion`），仅作寄存器用法参考。
- [ ] **Step 5: 验证。** CI 两个 job 通过。
- [ ] **Step 6:** 提交，开 PR（风险：low）。

### Task 7: 接线——协调器、情绪钩子、MCP 工具（P2 + P3）

**Files:**
- Create: `main/boards/m5stack/stackchan/behavior_director.h`、`behavior_director.cc`
- Create: `main/boards/m5stack/stackchan/anan_display.h`
- Modify: `main/boards/m5stack/stackchan/stackchan.cc`
- Modify: `main/boards/m5stack/stackchan/README.md`（工具表和行为说明）

**Interfaces:**
- Consumes: `BehaviorCore`、`Outputs`（Task 4）；`BuildGesture`、`ClampGestureCount`（Task 5）；`LedForState`、`LedRing::Apply`（Task 6）；`ServoNeck::StartMove / InvalidateRequests / IsMoving / GetTarget`（Task 3）。
- Produces:

```cpp
class BehaviorDirector {
public:
    BehaviorDirector(ServoNeck* neck, LedRing* led);   // 两者都可为 nullptr
    void Start();                                       // 建队列（长度 16）和任务（栈 4096，优先级 3）
    void NotifyStateChanged();                          // 任意任务可调；读 Application::GetDeviceState()
    void NotifyEmotion(const char* emotion);            // 任意任务可调；nullptr 安全
    void RequestGesture(anan::Gesture g, int count);    // MCP 工具用
    void RequestManualMove(int yaw_deg, int pitch_deg); // 现有 self.neck.set_angles 改走这里
};

// anan_display.h
class AnanDisplay : public emote::EmoteDisplay {
public:
    using EmoteDisplay::EmoteDisplay;
    void SetDirector(BehaviorDirector* d);
    bool SetEmotion(const char* emotion) override;      // 先转发给 director，再调父类；父类返回 false 时改设 "neutral"
};
```

状态映射：`kDeviceStateIdle` → `Idle`；`kDeviceStateListening` → `Listening`；`kDeviceStateSpeaking` → `Speaking`；其余 → `Other`。

MCP 工具：
- `self.head.nod`：描述 “Nod the head. Call this when you agree or say yes.”；参数 `count` 整数，默认 1，范围 1–3。
- `self.head.shake`：描述 “Shake the head. Call this when you disagree or say no.”；参数同上。
- 两者在 `neck == nullptr` 时返回错误 “the neck is unavailable”。
- `self.neck.set_angles` 保留名字和参数，实现改为调用 `RequestManualMove`。

- [ ] **Step 1:** 实现 `BehaviorDirector`。任务循环：每 20 ms 取空队列里的事件喂给 `BehaviorCore`，调 `Poll`，执行输出：`cancel_gesture` → `InvalidateRequests()` 并清空当前关键帧序列；`start_gesture` → 以 `GetTarget()` 为 base 调 `BuildGesture`，逐帧 `StartMove`，上一帧时长到了再发下一帧，播完调 `OnGestureFinished`；`move_to` → `StartMove`；LED 命令只在变化时 `Apply`。队列满时丢弃最旧的情绪事件，不丢状态事件。
- [ ] **Step 2:** 实现 `AnanDisplay`。
- [ ] **Step 3:** 改 `stackchan.cc`：`CONFIG_USE_EMOTE_MESSAGE_STYLE` 分支里用 `AnanDisplay` 代替 `EmoteDisplay`；`InitializeServoNeck` 和 `InitializePy32` 之后创建 `LedRing` 和 `BehaviorDirector` 并 `Start()`；`GetLed()` 返回 `LedRing`；注册两个新工具；`set_angles` 改走协调器。检查文件里 `IDLE_FACE_*`、`IDLE_MOTION_DURATION_MS` 相关代码：如果有任何在待机时调用 `StartMove` 的路径，删掉；如果只是未使用的常量，删掉常量。
- [ ] **Step 4:** 更新板子 README：工具表加两行；加一段“行为规则”，内容同 spec 3.1 的状态表。
- [ ] **Step 5: 验证。** CI 两个 job 通过。`grep -n "StartMove" main/boards/m5stack/stackchan/stackchan.cc` 只剩开机回中那一处。
- [ ] **Step 6:** 提交，开 PR（风险：medium，改了板子的显示和 LED 入口）。按用户的评审流程：贴测试和编译输出 → 新上下文 subagent 审 diff → 安全审查单独一遍。
- [ ] **Step 7: 用户闸门（P2 + P3）。** 用户烧录并逐条确认：

  | 检查 | 通过条件 |
  |---|---|
  | 待机 5 分钟 | 舵机零动作、无声音；灯关 |
  | 唤醒后 | 灯变绿 |
  | 她说话时 | 灯蓝色呼吸；头停住不漂 |
  | 问“一加一等于二吗” | 她点头 |
  | 问“太阳从西边出来吗” | 她摇头 |
  | 让她讲个开心的事 | 有轻点头，但不连续晃 |
  | 说“头转向左边” | 头转过去并停住约 8 秒，不被弹回 |
  | 对话结束 | 头慢速回正中，灯渐暗到关 |
  | 整个过程 | 语音不卡顿；Task 2 的唤醒成绩不下降 |

---

## 后续计划（不在本文件内）

- **P4 人脸追踪：** 先做 spike，量 esp-dl 人脸检测的帧率、内存和对语音的影响。结果出来后写计划。输出接 `BehaviorDirector` 的一个新方法 `NotifyFaceTarget(yaw, pitch)`，对应核心里已有的 `OnFaceTarget`。
- **P5 脸和待机小动作：** 先用 `neutral` 跑通“PNG 序列 → `.eaf` → assets 分区 → 上机”整条管线，确认颜色和每帧体积。结果出来后写计划。固件侧用核心里已有的 `idle_actions_enabled`。
