/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Variant.h>

namespace Web::WebAudio {

class AudioNode;

struct StartSource {
    AudioNode* node { nullptr };
    float when { 0.0f };
    float offset { 0.0f };
    float duration { AK::Infinity<float> };
};

struct StopSource {
    AudioNode* node { nullptr };
    float when { 0.0f };
};

struct StartRendering { };

// https://webaudio.github.io/web-audio-api/#control-message
using ControlMessage = Variant<StartSource, StopSource, StartRendering>;

}
