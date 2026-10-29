/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/SkiaBackendContext.h>
#include <Compositor/WebGLThread.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibThreading/Thread.h>

namespace Compositor {

static WebGLThread* s_the;

WebGLThread* WebGLThread::the()
{
    return s_the;
}

// WebGL runs on a thread of its own, so that compiling a page's shaders or creating its first context never holds up the
// frames the main thread presents, such as those of a scroll. Elsewhere it runs on the main thread, which hands its
// buffers to compositing without the handoff between GPU contexts that a thread of its own needs.
void WebGLThread::start_if_supported([[maybe_unused]] RefPtr<Gfx::SkiaBackendContext> const& main_thread_skia_backend_context)
{
#ifdef AK_OS_MACOS
    VERIFY(!s_the);
    s_the = new WebGLThread(main_thread_skia_backend_context != nullptr);
#endif
}

WebGLThread::WebGLThread(bool has_gpu)
    : m_thread(Threading::Thread::construct("WebGL"sv, [this, has_gpu] {
        return thread_main(has_gpu);
    }))
{
    m_thread->start();
    MutexLocker locker(m_mutex);
    m_condition.wait_while([&] { return !m_started; });
}

intptr_t WebGLThread::thread_main(bool has_gpu)
{
    // Below the main thread, whose frames come first.
    if (auto result = Core::Platform::set_current_thread_qos(Core::Platform::ThreadQoS::UserInitiated); result.is_error())
        dbgln("Compositor: Could not set the WebGL thread's QoS: {}", result.error());

    Core::EventLoop event_loop;
    {
        MutexLocker locker(m_mutex);
        // A Skia context belongs to the thread that uses it, so the WebGL thread has one of its own.
        if (has_gpu)
            m_skia_backend_context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
        m_event_loop = Core::EventLoop::current_weak();
        m_started = true;
        m_condition.broadcast();
    }
    return event_loop.exec();
}

void WebGLThread::post(Function<void()> function)
{
    auto event_loop = m_event_loop->take();
    VERIFY(event_loop);
    event_loop->deferred_invoke(move(function));
}

}
