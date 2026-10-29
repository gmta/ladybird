/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CanvasSurfaceRegistry.h>
#include <Compositor/CompositorWebContentServerEndpoint.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/VideoFrameHolds.h>
#include <Compositor/WebGLHost.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibIPC/Message.h>
#include <LibIPC/Transport.h>
#include <LibMediaCodecs/VideoFrame.h>

namespace Compositor {

namespace ServerMessages = Messages::CompositorWebContentServer;

NonnullRefPtr<WebGLHostChannel> WebGLHostChannel::create(ConnectionFromWebContent& connection, IPC::Transport& transport, bool runs_on_main_thread)
{
    RefPtr<Core::WeakEventLoopReference> main_event_loop;
    if (!runs_on_main_thread)
        main_event_loop = Core::EventLoop::current_weak();
    return adopt_ref(*new WebGLHostChannel(connection, transport, move(main_event_loop)));
}

WebGLHostChannel::WebGLHostChannel(ConnectionFromWebContent& connection, IPC::Transport& transport, RefPtr<Core::WeakEventLoopReference> main_event_loop)
    : m_transport(&transport)
    , m_connection(&connection)
    , m_main_event_loop(move(main_event_loop))
{
}

WebGLHostChannel::~WebGLHostChannel() = default;

// The transport queues messages from any thread for its IO thread to send, so a reply goes out from the thread that
// produced it.
void WebGLHostChannel::post_reply(IPC::MessageBuffer& buffer)
{
    MutexLocker locker(m_transport_mutex);
    if (!m_transport || !m_transport->is_open())
        return;
    if (buffer.transfer_message(*m_transport).is_error())
        m_transport = nullptr;
}

void WebGLHostChannel::run_on_main_thread(MainThreadTask task)
{
    if (!m_main_event_loop) {
        if (m_connection)
            task(*m_connection);
        return;
    }
    auto event_loop = m_main_event_loop->take();
    if (!event_loop)
        return;
    event_loop->deferred_invoke([channel = NonnullRefPtr { *this }, task = move(task)] {
        // NB: The connection is only set and cleared on the main thread.
        if (channel->m_connection)
            task(*channel->m_connection);
    });
}

void WebGLHostChannel::detach()
{
    m_connection = nullptr;
    MutexLocker locker(m_transport_mutex);
    m_transport = nullptr;
}

WebGLHost::WebGLHost(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool hands_buffers_to_main_thread, CanvasSurfaceRegistry& canvas_surface_registry, VideoFrameHolds const& video_frame_holds, NonnullRefPtr<WebGLHostChannel> channel)
    : m_skia_backend_context(move(skia_backend_context))
    , m_hands_buffers_to_main_thread(hands_buffers_to_main_thread)
    , m_canvas_surface_registry(canvas_surface_registry)
    , m_video_frame_holds(video_frame_holds)
    , m_channel(move(channel))
{
}

WebGLHost::~WebGLHost() = default;

bool WebGLHost::handles_message(IPC::Message const& message)
{
    switch (message.message_id()) {
    case static_cast<int>(ServerMessages::MessageID::CreateWebglContext):
    case static_cast<int>(ServerMessages::MessageID::WebglSetCommandBuffer):
    case static_cast<int>(ServerMessages::MessageID::WebglCommandsFromSharedBuffer):
    case static_cast<int>(ServerMessages::MessageID::WebglDrainCommandBuffer):
    case static_cast<int>(ServerMessages::MessageID::WebglCommands):
    case static_cast<int>(ServerMessages::MessageID::WebglPresentCanvas):
    case static_cast<int>(ServerMessages::MessageID::WebglClearDrawingBuffer):
    case static_cast<int>(ServerMessages::MessageID::WebglSyncCall):
    case static_cast<int>(ServerMessages::MessageID::WebglReadPixels):
    case static_cast<int>(ServerMessages::MessageID::WebglReadBufferSubData):
        return true;
    default:
        return false;
    }
}

HostWebGLContext* WebGLHost::context(Compositing::CanvasId canvas_id)
{
    auto it = m_contexts.find(canvas_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

void WebGLHost::post_reply(IPC::Message const& reply)
{
    auto buffer = MUST(reply.encode());
    m_channel->post_reply(buffer);
}

void WebGLHost::did_misbehave(char const* reason)
{
    m_channel->run_on_main_thread([reason](ConnectionFromWebContent& connection) {
        connection.did_misbehave(reason);
    });
}

Vector<HostWebGLContext::UploadSource> WebGLHost::resolve_upload_sources(Vector<Compositing::WebGL::ImageSourceFrame> const& frames) const
{
    Vector<HostWebGLContext::UploadSource> sources;
    sources.ensure_capacity(frames.size());
    for (auto const& frame : frames) {
        frame.visit(
            [&](Gfx::DecodedImageFrame const& decoded_frame) {
                sources.unchecked_append(decoded_frame);
            },
            [&](Compositing::CapturedVideoFrame const& captured_frame) {
                if (auto video_frame = m_video_frame_holds.held_frame(captured_frame.sink_handle, captured_frame.generation)) {
                    sources.unchecked_append(video_frame.release_nonnull());
                    return;
                }
                sources.unchecked_append(Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 1, 1 })) } });
            });
    }
    return sources;
}

void WebGLHost::present(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    auto* context = this->context(canvas_id);
    VERIFY(context);
    auto presented = MUST(context->prepare_for_compositing(preserve_drawing_buffer));

    // The main thread composites with a Skia context of its own, so it gets a surface of its own for the buffer: a CPU
    // copy wrapped again, or the shared image the buffer draws into. It keeps that surface until the buffer is freed.
    auto& wrapped_buffer_ids = m_buffer_ids_wrapped_by_main_thread.ensure(canvas_id);
    WebGLPresentedBuffer buffer { .id = presented.id, .content = {}, .freed_buffer_ids = {} };
    wrapped_buffer_ids.remove_all_matching([&](u64 buffer_id) {
        if (context->has_buffer(buffer_id))
            return false;
        buffer.freed_buffer_ids.append(buffer_id);
        return true;
    });
    if (!wrapped_buffer_ids.contains_slow(presented.id)) {
        wrapped_buffer_ids.append(presented.id);
        buffer.content = [&] -> Variant<NonnullRefPtr<Gfx::PaintingSurface>, NonnullOwnPtr<Gfx::SharedImageBuffer>> {
            if (presented.bitmap)
                return Gfx::PaintingSurface::wrap_bitmap(*presented.bitmap);
#ifdef AK_OS_MACOS
            if (m_hands_buffers_to_main_thread) {
                VERIFY(presented.shared_image_buffer);
                return make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::import_from_shared_image(presented.shared_image_buffer->export_shared_image()));
            }
#endif
            VERIFY(!m_hands_buffers_to_main_thread);
            return presented.surface;
        }();
    }

    m_channel->run_on_main_thread([canvas_id, buffer = move(buffer)](ConnectionFromWebContent& connection) mutable {
        connection.did_present_webgl_canvas(canvas_id, move(buffer));
    });
}

void WebGLHost::release_presented_buffer(Compositing::CanvasId canvas_id, u64 buffer_id)
{
    if (auto* context = this->context(canvas_id))
        context->release_presented_buffer(buffer_id);
}

void WebGLHost::handle(NonnullOwnPtr<IPC::Message> message)
{
    switch (message->message_id()) {
    case static_cast<int>(ServerMessages::MessageID::CreateWebglContext): {
        auto& request = static_cast<ServerMessages::CreateWebglContext&>(*message);
        auto context = HostWebGLContext::create(m_skia_backend_context, request.webgl_version(), { .depth = request.depth(), .stencil = request.stencil(), .antialias = request.antialias() }, request.size());
        if (!context) {
            post_reply(ServerMessages::CreateWebglContextResponse { false, Compositing::CanvasId { 0 }, {} });
            return;
        }
        auto canvas_id = m_canvas_surface_registry.allocate_canvas_id();
        auto supported_extensions = context->gl_context().get_supported_opengl_extensions();
        m_contexts.set(canvas_id, context.release_nonnull());
        post_reply(ServerMessages::CreateWebglContextResponse { true, canvas_id, move(supported_extensions) });
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglSetCommandBuffer): {
        auto& request = static_cast<ServerMessages::WebglSetCommandBuffer&>(*message);
        auto shared_command_buffer = Compositing::WebGL::WebGLSharedCommandBuffer::adopt_received_buffer(request.take_command_buffer());
        if (!shared_command_buffer.has_value()) {
            did_misbehave("WebContent sent an invalid WebGL shared command buffer");
            return;
        }
        auto* context = this->context(request.canvas_id());
        VERIFY(context);
        context->set_shared_command_buffer(shared_command_buffer.release_value());
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglCommandsFromSharedBuffer): {
        auto& request = static_cast<ServerMessages::WebglCommandsFromSharedBuffer&>(*message);
        auto* context = this->context(request.canvas_id());
        VERIFY(context);

        // WebContent can rewrite shared bytes while they execute, so a malformed stream is a misbehaving peer rather
        // than a Compositor bug; report instead of crashing.
        auto commands = context->shared_command_buffer_range(request.offset(), request.size_in_bytes());
        if (!commands.has_value() || context->execute_commands(*commands, resolve_upload_sources(request.frames())).is_error()) {
            did_misbehave("WebContent published an invalid WebGL shared command buffer range");
            return;
        }
        context->store_executed_flush_sequence_number(request.flush_sequence_number());
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglDrainCommandBuffer):
        // The empty reply is the point: it proves every earlier WebGL message on this connection, including all
        // published command ranges, has already been processed.
        post_reply(ServerMessages::WebglDrainCommandBufferResponse {});
        return;
    case static_cast<int>(ServerMessages::MessageID::WebglCommands): {
        auto& request = static_cast<ServerMessages::WebglCommands&>(*message);
        if (!request.commands().is_valid()) {
            did_misbehave("WebContent sent an invalid WebGL command buffer");
            return;
        }
        auto* context = this->context(request.canvas_id());
        VERIFY(context);
        MUST(context->execute_commands(request.commands().bytes(), resolve_upload_sources(request.frames())));
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglPresentCanvas): {
        auto& request = static_cast<ServerMessages::WebglPresentCanvas&>(*message);
        present(request.canvas_id(), request.preserve_drawing_buffer());
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglClearDrawingBuffer): {
        auto& request = static_cast<ServerMessages::WebglClearDrawingBuffer&>(*message);
        if (auto* context = this->context(request.canvas_id()))
            context->clear_drawing_buffer();
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglSyncCall): {
        auto& request = static_cast<ServerMessages::WebglSyncCall&>(*message);
        auto* context = this->context(request.canvas_id());
        VERIFY(context);
        post_reply(ServerMessages::WebglSyncCallResponse { MUST(context->execute_sync_call(request.request())) });
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglReadPixels): {
        auto& request = static_cast<ServerMessages::WebglReadPixels&>(*message);
        auto buf_size = request.buf_size();
        auto pixels = request.take_pixels();
        if (buf_size < 0 || (buf_size > 0 && (!pixels.is_valid() || pixels.size() < static_cast<size_t>(buf_size)))) {
            did_misbehave("WebContent sent an invalid WebGL readPixels buffer");
            post_reply(ServerMessages::WebglReadPixelsResponse { 0, 0, 0 });
            return;
        }
        auto* context = this->context(request.canvas_id());
        VERIFY(context);
        auto result = context->read_pixels_robust_angle(request.x(), request.y(), request.width(), request.height(), request.format(), request.type(), buf_size, move(pixels));
        post_reply(ServerMessages::WebglReadPixelsResponse { result.length, result.columns, result.rows });
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::WebglReadBufferSubData): {
        auto& request = static_cast<ServerMessages::WebglReadBufferSubData&>(*message);
        auto size = request.size();
        auto data = request.take_data();
        if (size < 0 || (size > 0 && (!data.is_valid() || data.size() < static_cast<size_t>(size)))) {
            did_misbehave("WebContent sent an invalid WebGL buffer readback target");
            post_reply(ServerMessages::WebglReadBufferSubDataResponse { false });
            return;
        }
        auto* context = this->context(request.canvas_id());
        VERIFY(context);
        post_reply(ServerMessages::WebglReadBufferSubDataResponse { context->read_buffer_sub_data(request.target(), request.offset(), size, move(data)) });
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::DestroyCanvasContext): {
        auto& request = static_cast<ServerMessages::DestroyCanvasContext&>(*message);
        if (!m_contexts.remove(request.canvas_id()))
            return;
        m_buffer_ids_wrapped_by_main_thread.remove(request.canvas_id());
        m_channel->run_on_main_thread([canvas_id = request.canvas_id()](ConnectionFromWebContent& connection) {
            connection.did_destroy_webgl_canvas(canvas_id);
        });
        return;
    }
    case static_cast<int>(ServerMessages::MessageID::GetCanvasPixels): {
        auto& request = static_cast<ServerMessages::GetCanvasPixels&>(*message);
        auto* context = this->context(request.canvas_id());

        // NB: The drawing buffer of a WebGL context is sRGB.
        if (!context || request.alpha_type() != Gfx::AlphaType::Premultiplied || !request.color_space().data().has<Empty>()) {
            post_reply(ServerMessages::GetCanvasPixelsResponse { Gfx::ShareableBitmap {} });
            return;
        }
        post_reply(ServerMessages::GetCanvasPixelsResponse { context->read_back_drawing_buffer(request.rect()) });
        return;
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

}
