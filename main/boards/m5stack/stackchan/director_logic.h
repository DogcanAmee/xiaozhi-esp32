#pragma once

// One iteration of the behavior director: report a finished gesture, feed inputs to the core,
// poll it and drive the head. The FreeRTOS task in behavior_director.cc only collects inputs,
// reads the clock and calls Step(); the LED ring and the level outputs stay there.
// No ESP-IDF dependency: compiled and tested on the host.

#include <cstdint>
#include <optional>
#include <span>

#include "behavior_core.h"
#include "gesture_player.h"

namespace anan {

// The neck as seen by the director.
class HeadPort {
public:
    virtual ~HeadPort() = default;
    // False when head commands can do nothing useful (e.g. torque released).
    virtual bool Usable() const = 0;
    virtual void StageMove(Pose pose, uint32_t duration_ms) = 0;
    // Abandon any move in flight. The target stays where it was.
    virtual void Invalidate() = 0;
    // The most recently commanded target.
    virtual Pose Target() const = 0;
};

// Plain data so it can be copied into a FreeRTOS queue.
struct DirectorInput {
    enum class Type : uint8_t { Emotion, Gesture, ManualMove };
    Type type;
    int a;  // gesture id / yaw
    int b;  // gesture count / pitch
    char emotion[24];

    static DirectorInput Emotion(const char* emotion);  // null -> empty string
    static DirectorInput GestureRequest(Gesture g, int count);
    static DirectorInput ManualMove(int yaw_deg, int pitch_deg);
};

// Not thread-safe: drive it from a single task.
class DirectorLogic {
public:
    // Outputs::move_ms == 0 means "the caller's default speed".
    static constexpr uint32_t kManualMoveMs = 600;
    static constexpr uint32_t kTrackingMoveMs = 250;
    // After a gesture is cancelled with nothing else moving the head, return to its base.
    static constexpr uint32_t kReturnToBaseMs = 300;

    // `head` may be null: no head command is sent, gestures finish at once.
    explicit DirectorLogic(HeadPort* head);
    DirectorLogic(const DirectorLogic&) = delete;
    DirectorLogic& operator=(const DirectorLogic&) = delete;

    // One iteration, in this order: report a finished gesture to the core (before anything
    // that could replace it), apply `phase` if set, feed `inputs`, Poll, drive the head.
    // Returns the Poll outputs for the caller's LED and level-output handling.
    Outputs Step(int64_t now_ms, std::optional<Phase> phase, std::span<const DirectorInput> inputs);

    const BehaviorCore& core() const { return core_; }
    bool gesture_playing() const { return player_.playing(); }

private:
    bool HeadUsable() const;
    void Feed(const DirectorInput& in, int64_t now_ms);
    void ApplyHead(const Outputs& out, int64_t now_ms);

    HeadPort* head_;
    BehaviorCore core_;
    GesturePlayer player_;
    // Base pose of the gesture the player is playing; set while it plays.
    std::optional<Pose> gesture_base_;
    // HeadUsable() at the previous Step; empty before the first.
    std::optional<bool> head_was_usable_;
};

}  // namespace anan
