/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibThreading/Forward.h>

namespace Compositor {

class WebGLThread {
    AK_MAKE_NONCOPYABLE(WebGLThread);
    AK_MAKE_NONMOVABLE(WebGLThread);

public:
    AK_ALLOC_WITH_KMALLOC;

    static WebGLThread* the();
    static void start_if_supported(RefPtr<Gfx::SkiaBackendContext> const& main_thread_skia_backend_context);

    void post(Function<void()>);

    RefPtr<Gfx::SkiaBackendContext> skia_backend_context() const { return m_skia_backend_context; }

private:
    explicit WebGLThread(bool has_gpu);
    intptr_t thread_main(bool has_gpu);

    NonnullRefPtr<Threading::Thread> m_thread;
    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    bool m_started { false };
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
};

}
