/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/ControlMessageQueue.h>

namespace Web::WebAudio {

void ControlMessageQueue::enqueue(ControlMessage message)
{
    Threading::MutexLocker locker(m_mutex);
    m_messages.append(move(message));
    m_condition.signal();
}

Vector<ControlMessage> ControlMessageQueue::drain()
{
    Threading::MutexLocker locker(m_mutex);
    return move(m_messages);
}

bool ControlMessageQueue::wait_for_messages()
{
    Threading::MutexLocker locker(m_mutex);
    while (m_messages.is_empty() && !m_exit)
        m_condition.wait();
    return !m_exit;
}

void ControlMessageQueue::signal_exit()
{
    Threading::MutexLocker locker(m_mutex);
    m_exit = true;
    m_condition.broadcast();
}

}
