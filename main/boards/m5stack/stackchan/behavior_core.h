#pragma once

// Pure decision logic for Stack-chan head / face / LED behavior.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anan {

enum class Phase { Idle, Listening, Speaking, Other };
enum class HeadOwner { Hold, Tracking, Gesture, Manual };
enum class Gesture { None, Nod, Shake, Tilt, Bow, QuickShake };
struct Pose {
    int yaw_deg;
    int pitch_deg;
};

inline constexpr int kPitchMin = 5, kPitchMax = 85;  // = SAFE_PITCH_MIN/MAX
inline constexpr int kYawMin = -45, kYawMax = 45;    // coordinator's own conservative range
inline constexpr Pose kRestPose{0, 5};               // = BOOT_INIT_YAW/PITCH_DEG
inline constexpr int64_t kAutoGestureGapMs = 4000;
inline constexpr int64_t kPhaseSettleMs = 150;  // a phase must be stable this long to take effect
inline constexpr int64_t kManualHoldMs = 8000;  // how long a manual head turn owns the head
inline constexpr uint32_t kReturnToRestMs = 1500;
// An emotion this recent when Speaking takes effect is applied as if it arrived then (the
// server sends the emotion just before it starts speaking).
inline constexpr int64_t kEmotionCarryMs = 2000;
// A tool gesture request this soon after the previous accepted one is ignored.
inline constexpr int64_t kToolGestureGapMs = 1000;

Pose ClampPose(Pose p);
Gesture GestureForEmotion(std::string_view emotion);  // unknown -> None

struct Outputs {
    std::optional<Pose> move_to;  // when set, ease the neck to this pose
    uint32_t move_ms = 0;         // 0 = caller's default speed
    Gesture start_gesture = Gesture::None;
    int gesture_count = 1;
    bool cancel_gesture = false;
    bool tracking_enabled = false;      // for P4
    bool idle_actions_enabled = false;  // for P5
    Phase led_phase = Phase::Other;     // handed to led_color
    std::string led_emotion;            // empty = use the phase color
};

// One-shot commands (move_to, start_gesture, cancel_gesture) are returned by
// exactly one Poll(); level outputs are returned by every Poll().
// Not thread-safe: drive it from a single task.
class BehaviorCore {
public:
    void OnPhase(Phase phase, int64_t now_ms);
    void OnEmotion(std::string_view emotion, int64_t now_ms);
    void OnManualMove(Pose target, int64_t now_ms);
    void OnGestureRequest(Gesture g, int count, int64_t now_ms);  // from tools; kToolGestureGapMs apart
    void OnFaceTarget(Pose target, int64_t now_ms);
    void OnGestureFinished(int64_t now_ms);
    // The head can move again (e.g. torque back on). In Idle with no manual hold, re-arm the
    // one-shot return-to-rest; otherwise nothing.
    void OnHeadAvailable(int64_t now_ms);
    Outputs Poll(int64_t now_ms);  // call every 20 ms; returns commands since the last call
    HeadOwner owner() const;
    Phase phase() const;

private:
    void Advance(int64_t now_ms);  // expire manual hold, settle pending phase
    void EnterPhase(Phase next, int64_t now_ms);
    void ApplyEmotion(std::string_view emotion, int64_t now_ms);  // only while Speaking

    Phase phase_ = Phase::Other;
    std::optional<Phase> pending_phase_;
    int64_t pending_since_ms_ = 0;

    bool manual_active_ = false;
    int64_t manual_until_ms_ = 0;
    bool gesture_active_ = false;
    std::optional<int64_t> last_auto_gesture_ms_;
    std::optional<int64_t> last_tool_gesture_ms_;
    std::string led_emotion_;
    // The last emotion that arrived outside Speaking, for kEmotionCarryMs.
    std::optional<std::string> carried_emotion_;
    int64_t carried_emotion_ms_ = 0;

    // One-shot commands waiting for the next Poll().
    std::optional<Pose> pending_move_;
    uint32_t pending_move_ms_ = 0;
    bool pending_rest_ = false;  // return to rest as soon as nobody else owns the head
    std::optional<Pose> pending_face_;
    Gesture pending_gesture_ = Gesture::None;
    int pending_gesture_count_ = 1;
    bool pending_cancel_ = false;
};

}  // namespace anan
