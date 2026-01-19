/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#control-message-queue
class WEB_API ControlMessageQueue {

public:
    void enqueue(ControlMessage);   // Called by the control thread.
    Vector<ControlMessage> drain(); // Called by the rendering thread.

    // Wait for messages to be available (blocks until messages arrive or exit is signaled)
    // Returns true if messages are available, false if exit was signaled
    bool wait_for_messages();

    // Signal the queue to wake up any waiting threads (used for shutdown)
    void signal_exit();

private:
    mutable Threading::Mutex m_mutex;
    Threading::ConditionVariable m_condition { m_mutex };
    Vector<ControlMessage> m_messages;
    bool m_exit { false };
};

}
