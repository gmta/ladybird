/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <Compositor/HostWebGLContext.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/WebGL/ImageSourceFrame.h>
#include <LibCore/Forward.h>
#include <LibGfx/Forward.h>
#include <LibIPC/Forward.h>

namespace Compositor {

struct WebGLPresentedBuffer {
    u64 id { 0 };
    Optional<Variant<NonnullRefPtr<Gfx::PaintingSurface>, NonnullOwnPtr<Gfx::SharedImageBuffer>>> content;
    Vector<u64> freed_buffer_ids;
};

class WebGLHostChannel final : public AtomicRefCounted<WebGLHostChannel> {
public:
    using MainThreadTask = Function<void(ConnectionFromWebContent&)>;

    static NonnullRefPtr<WebGLHostChannel> create(ConnectionFromWebContent&, IPC::Transport&, bool runs_on_main_thread);
    ~WebGLHostChannel();

    void post_reply(IPC::MessageBuffer&);
    void run_on_main_thread(MainThreadTask);
    void detach();

private:
    WebGLHostChannel(ConnectionFromWebContent&, IPC::Transport&, RefPtr<Core::WeakEventLoopReference> main_event_loop);

    Mutex m_transport_mutex;
    IPC::Transport* m_transport { nullptr };
    ConnectionFromWebContent* m_connection { nullptr };
    RefPtr<Core::WeakEventLoopReference> m_main_event_loop;
};

class WebGLHost {
    AK_MAKE_NONCOPYABLE(WebGLHost);
    AK_MAKE_NONMOVABLE(WebGLHost);

public:
    AK_ALLOC_WITH_KMALLOC;

    WebGLHost(RefPtr<Gfx::SkiaBackendContext>, bool hands_buffers_to_main_thread, CanvasSurfaceRegistry&, VideoFrameHolds const&, NonnullRefPtr<WebGLHostChannel>);
    ~WebGLHost();

    static bool handles_message(IPC::Message const&);

    void handle(NonnullOwnPtr<IPC::Message>);
    void release_presented_buffer(Compositing::CanvasId, u64 buffer_id);

private:
    HostWebGLContext* context(Compositing::CanvasId);
    Vector<HostWebGLContext::UploadSource> resolve_upload_sources(Vector<Compositing::WebGL::ImageSourceFrame> const&) const;
    void present(Compositing::CanvasId, bool preserve_drawing_buffer);
    void post_reply(IPC::Message const&);
    void did_misbehave(char const*);

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    bool m_hands_buffers_to_main_thread { false };
    CanvasSurfaceRegistry& m_canvas_surface_registry;
    VideoFrameHolds const& m_video_frame_holds;
    NonnullRefPtr<WebGLHostChannel> m_channel;
    HashMap<Compositing::CanvasId, NonnullOwnPtr<HostWebGLContext>> m_contexts;
    HashMap<Compositing::CanvasId, Vector<u64>> m_buffer_ids_wrapped_by_main_thread;
};

}
