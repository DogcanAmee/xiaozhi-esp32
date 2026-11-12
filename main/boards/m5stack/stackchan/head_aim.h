#pragma once

// P4 spike: decides when and how far the head turns while Listening, from face detections
// (camera) and from the direction of sound (DOA). Pure logic, no ESP-IDF dependency.
//
// Closed loop: the camera and the microphones sit in the head, so every measurement is relative
// to the pose the head was in when it was taken, and every command is a delta. Hence:
// - A measurement is only used when it was taken after the previous command had finished and
//   the head had settled (frames: the capture must start kFrameSettleMs after the command;
//   audio: see AudioUsableFrom). Measurements taken in the old pose would be corrected twice.
// - No filter state is carried across commands: each command uses one fresh, settled
//   detection.
// - The face owns the head. Sound is used only when no face has been seen for kFaceLostMs.
// - Yaw sign self-check: when, after a face yaw command, the next settled detection shows the
//   face further off-centre on the same side (or gone, if it was near the edge), twice in a
//   row, the yaw sign is flipped at run time and those turns are undone (the head was turning
//   away from the face).

#include <cstdint>
#include <optional>

namespace anan {

struct AimDelta {
    int dyaw = 0;
    int dpitch = 0;
};

struct AimConfig {
    float hfov_deg = 60.0f;
    float vfov_deg = 45.0f;
    // Initial yaw sign: +1 = a face in the right half of the (unmirrored) image turns the head to
    // positive yaw. May be flipped at run time by the self-check.
    float yaw_sign = -1.0f;
    float pitch_sign = -1.0f;  // a face in the upper half (cy < 0) -> pitch grows (looks up)
    float gain = 0.5f;
    float deadband_deg = 3.0f;
    float max_face_step_deg = 8.0f;
    int64_t move_ms = 250;          // DirectorLogic::kTrackingMoveMs
    int64_t frame_settle_ms = 150;  // after the move has finished
    int64_t audio_quiet_ms = 300;   // servo noise: no DOA during a move and this long after
    // Self-check: |cx| must grow by more than this to count as "worse".
    float flip_margin = 0.03f;
    int flip_after = 2;
    // Look up once per Listening session when no face was found for this long (rest pitch is
    // the lowest allowed, a seated user's face is usually above the frame). 0 = never.
    int64_t nudge_after_ms = 3000;
    int nudge_pitch_deg = 20;
    // Sound.
    int64_t face_lost_ms = 1000;
    int doa_min_confidence = 5;  // 0..9
    float doa_min_angle_deg = 15.0f;
    float doa_max_step_deg = 10.0f;
    int64_t doa_min_interval_ms = 400;
    float doa_yaw_sign = 1.0f;  // yaw delta = doa_yaw_sign * angle (angle > 0 = towards mic 2)
};

class HeadAim {
public:
    explicit HeadAim(const AimConfig& cfg = AimConfig()) : cfg_(cfg) {}

    // A new Listening session starts (the robot's yaw sign knowledge is kept).
    void Begin(int64_t now_ms);

    // One finished detection. `frame_ms`: no part of the frame was captured before this time.
    // `face`: centre of the chosen face, -0.5..0.5 of the image (x right, y down), or none.
    // `tracking`: the head is the tracker's (BehaviorDirector::tracking_enabled()).
    struct Face {
        float cx;
        float cy;
    };
    std::optional<AimDelta> OnFrame(int64_t frame_ms, int64_t now_ms, std::optional<Face> face,
                                    bool tracking);

    // The current smoothed sound direction (only call with a fresh estimate built from audio
    // captured at or after AudioUsableFrom()).
    std::optional<AimDelta> OnDoa(int64_t now_ms, bool valid, float angle_deg, int confidence,
                                  bool tracking);

    // Audio captured before this time may contain servo noise or describe the old pose.
    int64_t AudioUsableFrom() const { return last_cmd_ms_ + cfg_.move_ms + cfg_.audio_quiet_ms; }
    // Changes whenever a command is issued.
    uint32_t command_count() const { return commands_; }

    float yaw_sign() const { return yaw_sign_; }
    int flips() const { return flips_; }
    // Diagnostics for the status line.
    bool has_face() const { return have_last_face_; }
    Face last_face() const { return last_face_; }
    std::optional<AimDelta> last_face_delta() const { return last_face_delta_; }
    int doa_moves() const { return doa_moves_; }
    int face_moves() const { return face_moves_; }
    void ClearWindowCounters() { doa_moves_ = face_moves_ = 0; }

private:
    // Result of a self-check; on the flip, returns the move that undoes the wrong turns.
    std::optional<AimDelta> Judge(bool worse, int64_t now_ms);
    void Issue(int64_t now_ms) {
        last_cmd_ms_ = now_ms;
        ++commands_;
    }

    AimConfig cfg_;
    float yaw_sign_ = cfg_.yaw_sign;
    int64_t last_cmd_ms_ = -1000000;
    uint32_t commands_ = 0;
    int64_t session_start_ms_ = 0;
    int64_t last_face_ms_ = -1000000;
    bool nudged_ = false;
    bool session_face_seen_ = false;

    // Self-check: the face x before the last face yaw command, while waiting for the first
    // settled detection after it.
    std::optional<float> check_cx_;
    int check_dyaw_ = 0;
    int worse_ = 0;
    int wrong_sum_ = 0;  // yaw of the commands judged "worse" in the current run
    int flips_ = 0;

    bool have_last_face_ = false;
    Face last_face_{0, 0};
    std::optional<AimDelta> last_face_delta_;
    int doa_moves_ = 0;
    int face_moves_ = 0;
};

}  // namespace anan
