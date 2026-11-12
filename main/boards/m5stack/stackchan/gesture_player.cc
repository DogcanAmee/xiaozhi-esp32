#include "gesture_player.h"

#include <utility>

namespace anan {

GesturePlayer::GesturePlayer(StageMove stage_move) : stage_move_(std::move(stage_move)) {}

void GesturePlayer::Start(std::vector<Keyframe> frames, int64_t now_ms) {
    frames_ = std::move(frames);
    index_ = 0;
    playing_ = true;
    frame_started_ms_ = now_ms;
    int64_t total_ms = 0;
    for (const Keyframe& f : frames_) {
        total_ms += f.duration_ms;
    }
    deadline_ms_ = now_ms + total_ms + kWatchdogSlackMs;
    if (!frames_.empty() && stage_move_) {
        stage_move_(frames_[0].pose, frames_[0].duration_ms);
    }
}

void GesturePlayer::Cancel() {
    playing_ = false;
    frames_.clear();
    index_ = 0;
}

bool GesturePlayer::Tick(int64_t now_ms) {
    if (!playing_) {
        return false;
    }
    if (frames_.empty() || now_ms >= deadline_ms_) {
        Cancel();
        return true;
    }
    // The next keyframe starts when the current one's move has had its full time, measured from
    // when it was actually staged, so a late tick never shortens the following move.
    if (now_ms < frame_started_ms_ + frames_[index_].duration_ms) {
        return false;
    }
    ++index_;
    if (index_ >= frames_.size()) {
        Cancel();
        return true;
    }
    frame_started_ms_ = now_ms;
    if (stage_move_) {
        stage_move_(frames_[index_].pose, frames_[index_].duration_ms);
    }
    return false;
}

}  // namespace anan
