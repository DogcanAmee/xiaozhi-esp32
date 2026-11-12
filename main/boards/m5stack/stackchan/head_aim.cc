#include "head_aim.h"

#include <algorithm>
#include <cmath>

namespace anan {

namespace {

int Step(float err_deg, float gain, float deadband, float max_step) {
    if (std::fabs(err_deg) <= deadband) {
        return 0;
    }
    return static_cast<int>(std::lround(std::clamp(gain * err_deg, -max_step, max_step)));
}

// A self-check yaw command must be at least this big to be judged.
constexpr int kMinCheckedYawDeg = 3;
// A pending self-check older than this is dropped (the face never came back).
constexpr int64_t kCheckExpireMs = 3000;
// A face this far out before a yaw command that is gone on the next settled frame counts as
// "further out".
constexpr float kLostCountsFromCx = 0.25f;

}  // namespace

void HeadAim::Begin(int64_t now_ms) {
    session_start_ms_ = now_ms;
    nudged_ = false;
    session_face_seen_ = false;
    check_cx_.reset();
    worse_ = 0;
    wrong_sum_ = 0;
    have_last_face_ = false;
    last_face_ms_ = -1000000;
}

std::optional<AimDelta> HeadAim::OnFrame(int64_t frame_ms, int64_t now_ms,
                                         std::optional<Face> face, bool tracking) {
    if (face) {
        have_last_face_ = true;
        session_face_seen_ = true;
        last_face_ = *face;
        last_face_ms_ = now_ms;
    }
    if (!tracking) {
        check_cx_.reset();
        worse_ = 0;
        wrong_sum_ = 0;
        return std::nullopt;
    }
    if (check_cx_ && now_ms - last_cmd_ms_ > kCheckExpireMs) {
        check_cx_.reset();
    }
    // Only frames whose capture started after the last move had finished and settled.
    if (frame_ms < last_cmd_ms_ + cfg_.move_ms + cfg_.frame_settle_ms) {
        return std::nullopt;
    }

    if (!face) {
        // The face left the frame right after a yaw command that pushed it outwards: as bad as
        // seeing it further out.
        if (check_cx_ && std::fabs(*check_cx_) >= kLostCountsFromCx) {
            check_cx_.reset();
            if (auto undo = Judge(true, now_ms)) {
                return undo;
            }
            return std::nullopt;
        }
        if (!nudged_ && !session_face_seen_ && cfg_.nudge_after_ms > 0 &&
            now_ms - session_start_ms_ > cfg_.nudge_after_ms) {
            nudged_ = true;
            Issue(now_ms);
            return AimDelta{0, cfg_.nudge_pitch_deg};
        }
        return std::nullopt;
    }

    const float cx = face->cx;
    if (check_cx_) {
        const float before = *check_cx_;
        check_cx_.reset();
        const bool same_side = (before > 0 && cx > 0) || (before < 0 && cx < 0);
        if (auto undo =
                Judge(same_side && std::fabs(cx) > std::fabs(before) + cfg_.flip_margin, now_ms)) {
            return undo;
        }
    }

    const float ey = yaw_sign_ * cx * cfg_.hfov_deg;
    const float ep = cfg_.pitch_sign * face->cy * cfg_.vfov_deg;
    AimDelta d;
    d.dyaw = Step(ey, cfg_.gain, cfg_.deadband_deg, cfg_.max_face_step_deg);
    d.dpitch = Step(ep, cfg_.gain, cfg_.deadband_deg, cfg_.max_face_step_deg);
    if (d.dyaw == 0 && d.dpitch == 0) {
        return std::nullopt;
    }
    if (std::abs(d.dyaw) >= kMinCheckedYawDeg) {
        check_cx_ = cx;
        check_dyaw_ = d.dyaw;
    }
    Issue(now_ms);
    ++face_moves_;
    last_face_delta_ = d;
    return d;
}

std::optional<AimDelta> HeadAim::Judge(bool worse, int64_t now_ms) {
    if (!worse) {
        worse_ = 0;
        wrong_sum_ = 0;
        return std::nullopt;
    }
    wrong_sum_ += check_dyaw_;
    if (++worse_ < cfg_.flip_after) {
        return std::nullopt;
    }
    // The head has been turning away from the face: flip, and undo those turns so the face is
    // back in view for the next (now correctly signed) command.
    yaw_sign_ = -yaw_sign_;
    ++flips_;
    worse_ = 0;
    AimDelta undo{-wrong_sum_, 0};
    wrong_sum_ = 0;
    Issue(now_ms);
    ++face_moves_;
    last_face_delta_ = undo;
    return undo;
}

std::optional<AimDelta> HeadAim::OnDoa(int64_t now_ms, bool valid, float angle_deg,
                                       int confidence, bool tracking) {
    if (!tracking || !valid || confidence < cfg_.doa_min_confidence) {
        return std::nullopt;
    }
    // The camera owns the head while it sees a face, and for a while after.
    if (now_ms - last_face_ms_ <= cfg_.face_lost_ms) {
        return std::nullopt;
    }
    if (std::fabs(angle_deg) <= cfg_.doa_min_angle_deg) {
        return std::nullopt;
    }
    if (now_ms - last_cmd_ms_ < cfg_.doa_min_interval_ms || now_ms < AudioUsableFrom()) {
        return std::nullopt;
    }
    const float step = std::clamp(cfg_.doa_yaw_sign * angle_deg, -cfg_.doa_max_step_deg,
                                  cfg_.doa_max_step_deg);
    AimDelta d{static_cast<int>(std::lround(step)), 0};
    check_cx_.reset();
    worse_ = 0;
    wrong_sum_ = 0;
    Issue(now_ms);
    ++doa_moves_;
    return d;
}

}  // namespace anan
