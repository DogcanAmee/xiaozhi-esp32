#pragma once

// P4 SPIKE ONLY (not kept): esp-dl face detection while Listening, feeding the behavior
// director's face-target path, with an on-screen measurement every 5 s.

#include "camera.h"
#include "esp_video_init.h"

class Display;

namespace anan {

class BehaviorDirector;

// An EspVideo whose Capture() (the photo MCP tool) and the face tracker never touch the V4L2
// buffer at the same time.
Camera* CreateTrackedCamera(const esp_video_init_config_t& config);

// Starts the tracker task. `camera` must come from CreateTrackedCamera. Call once.
void StartFaceTracker(Camera* camera, BehaviorDirector* director, Display* display);

}  // namespace anan
