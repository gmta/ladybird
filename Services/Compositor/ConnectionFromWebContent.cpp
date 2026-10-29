/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Debug.h>
#include <AK/Math.h>
#include <Compositor/ConnectionFromWebContent.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/RasterizeDisplayList.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/WebGLThread.h>
#include <LibCompositing/DisplayList/Canvas2DTape.h>
#include <LibCompositing/WebGL/WebGLSharedCommandBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/Timer.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibWebCommon/Page/InputEvent.h>

namespace Compositor {

ConnectionFromWebContent::ConnectionFromWebContent(NonnullOwnPtr<IPC::Transport> transport, NonnullRefPtr<CompositorState> compositor_state, int client_id)
    : IPC::ConnectionFromClient<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport), client_id)
    , m_compositor_state(move(compositor_state))
    , m_canvas_host(m_compositor_state->skia_backend_context(), m_compositor_state->canvas_surface_registry(), m_video_frame_holds)
    , m_webgl_channel(WebGLHostChannel::create(*this, this->transport(), !WebGLThread::the()))
{
    auto* webgl_thread = WebGLThread::the();
    auto skia_backend_context_for_webgl = webgl_thread ? webgl_thread->skia_backend_context() : m_compositor_state->skia_backend_context();
    m_webgl_host = new WebGLHost(move(skia_backend_context_for_webgl), webgl_thread != nullptr, m_compositor_state->canvas_surface_registry(), m_video_frame_holds, m_webgl_channel);
}

ConnectionFromWebContent::~ConnectionFromWebContent()
{
    tear_down_webgl();
}

void ConnectionFromWebContent::die()
{
    auto protector = NonnullRefPtr { *this };
    tear_down_webgl();
    m_compositor_state->destroy_contexts_for_web_content_client(*this);
    if (m_on_death)
        m_on_death(*this);
}

void ConnectionFromWebContent::tear_down_webgl()
{
    auto* webgl_host = exchange(m_webgl_host, nullptr);
    if (!webgl_host)
        return;
    m_webgl_channel->detach();

    // The host goes away on the thread it runs on, after the jobs already handed to it.
    if (auto* webgl_thread = WebGLThread::the())
        webgl_thread->post([webgl_host] { delete webgl_host; });
    else
        delete webgl_host;

    for (auto const& canvas_id : m_webgl_canvases.keys())
        m_compositor_state->canvas_surface_registry().remove_canvas_surface(canvas_id);
    m_webgl_canvases.clear();
    if (m_webgl_buffer_release_timer)
        m_webgl_buffer_release_timer->stop();
}

// WebGL runs on the WebGL thread, if there is one, so that its work never holds up the frames this thread presents. Its
// messages go there in the order they came in. A later message that may read what WebGL drew, or release a video frame
// one of its uploads named, waits until WebGL is done with the jobs before it.
ConnectionFromWebContent::DispatchDecision ConnectionFromWebContent::dispatch_decision_for(IPC::Message const& message)
{
    namespace ServerMessages = Messages::CompositorWebContentServer;

    if (WebGLHost::handles_message(message))
        return DispatchDecision::Forward;
    if (message.message_id() == static_cast<int>(ServerMessages::MessageID::DestroyCanvasContext)) {
        if (!m_canvas_host.has_context(static_cast<ServerMessages::DestroyCanvasContext const&>(message).canvas_id()))
            return DispatchDecision::Forward;
    }
    if (message.message_id() == static_cast<int>(ServerMessages::MessageID::GetCanvasPixels)) {
        if (!m_canvas_host.has_context(static_cast<ServerMessages::GetCanvasPixels const&>(message).canvas_id()))
            return DispatchDecision::Forward;
    }

    if (m_webgl_jobs_completed < m_webgl_job_awaited) {
        m_dispatch_awaits_webgl = true;
        return DispatchDecision::Defer;
    }
    return DispatchDecision::Dispatch;
}

static bool must_complete_before_later_messages(IPC::Message const& message)
{
    namespace ServerMessages = Messages::CompositorWebContentServer;

    auto names_video_frame = [](Vector<Compositing::WebGL::ImageSourceFrame> const& frames) {
        return any_of(frames, [](auto const& frame) { return frame.template has<Compositing::CapturedVideoFrame>(); });
    };
    switch (message.message_id()) {
    case static_cast<int>(ServerMessages::MessageID::WebglPresentCanvas):
    case static_cast<int>(ServerMessages::MessageID::DestroyCanvasContext):
        return true;
    case static_cast<int>(ServerMessages::MessageID::WebglCommands):
        return names_video_frame(static_cast<ServerMessages::WebglCommands const&>(message).frames());
    case static_cast<int>(ServerMessages::MessageID::WebglCommandsFromSharedBuffer):
        return names_video_frame(static_cast<ServerMessages::WebglCommandsFromSharedBuffer const&>(message).frames());
    default:
        return false;
    }
}

void ConnectionFromWebContent::forward_message(NonnullOwnPtr<IPC::Message> message)
{
    if (!m_webgl_host)
        return;

    auto job = ++m_webgl_jobs_forwarded;
    auto is_awaited = must_complete_before_later_messages(*message);
    if (is_awaited)
        m_webgl_job_awaited = job;

    auto run = [webgl_host = m_webgl_host, channel = m_webgl_channel, message = move(message), job, is_awaited]() mutable {
        webgl_host->handle(move(message));
        if (is_awaited) {
            channel->run_on_main_thread([job](ConnectionFromWebContent& connection) {
                connection.did_complete_webgl_job(job);
            });
        }
    };
    if (auto* webgl_thread = WebGLThread::the())
        webgl_thread->post(move(run));
    else
        run();
}

void ConnectionFromWebContent::did_complete_webgl_job(u64 job)
{
    m_webgl_jobs_completed = max(m_webgl_jobs_completed, job);
    if (m_dispatch_awaits_webgl && m_webgl_jobs_completed >= m_webgl_job_awaited) {
        m_dispatch_awaits_webgl = false;
        resume_dispatch();
    }
}

void ConnectionFromWebContent::did_present_webgl_canvas(Compositing::CanvasId canvas_id, WebGLPresentedBuffer buffer)
{
    auto& canvas = m_webgl_canvases.ensure(canvas_id);
    for (auto buffer_id : buffer.freed_buffer_ids)
        canvas.surfaces.remove(buffer_id);
    if (buffer.content.has_value()) {
        auto surface = buffer.content->visit(
            [&](NonnullRefPtr<Gfx::PaintingSurface>& surface) -> WebGLBufferSurface {
                return WebGLBufferSurface { move(surface), nullptr };
            },
            [&](NonnullOwnPtr<Gfx::SharedImageBuffer>& shared_image_buffer) -> WebGLBufferSurface {
#ifdef AK_OS_MACOS
                auto surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*shared_image_buffer, *m_compositor_state->skia_backend_context(), Gfx::PaintingSurface::Origin::BottomLeft);
                return WebGLBufferSurface { move(surface), move(shared_image_buffer) };
#else
                (void)shared_image_buffer;
                VERIFY_NOT_REACHED();
#endif
            });
        canvas.surfaces.set(buffer.id, move(surface));
    }

    if (canvas.presented_buffer_id.has_value() && *canvas.presented_buffer_id != buffer.id)
        retire_webgl_buffer(canvas_id, *canvas.presented_buffer_id);
    canvas.presented_buffer_id = buffer.id;

    // WebGL drew into the buffer behind the back of this surface, so drop any snapshot Skia cached of it.
    auto& surface = canvas.surfaces.get(buffer.id)->surface;
    surface->notify_content_will_change();
    m_compositor_state->canvas_surface_registry().set_canvas_surface(m_canvas_host.canvas_surface_owner_id(), canvas_id, surface);
}

void ConnectionFromWebContent::did_destroy_webgl_canvas(Compositing::CanvasId canvas_id)
{
    if (m_webgl_canvases.remove(canvas_id))
        m_compositor_state->canvas_surface_registry().remove_canvas_surface(canvas_id);
}

// The page may draw into a buffer again once nothing reads it, which the GPU may still do for frames already submitted.
void ConnectionFromWebContent::retire_webgl_buffer(Compositing::CanvasId canvas_id, u64 buffer_id)
{
    auto skia_backend_context = m_compositor_state->skia_backend_context();
    if (!skia_backend_context) {
        release_webgl_buffer(canvas_id, buffer_id);
        return;
    }

    ++m_pending_webgl_buffer_releases;
    skia_backend_context->submit_with_completion_callback([weak_this = make_weak_ptr<ConnectionFromWebContent>(), canvas_id, buffer_id] {
        auto strong_this = weak_this.strong_ref();
        if (!strong_this)
            return;
        --strong_this->m_pending_webgl_buffer_releases;
        strong_this->release_webgl_buffer(canvas_id, buffer_id);
    });

    if (!m_webgl_buffer_release_timer) {
        m_webgl_buffer_release_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(4), [this, skia_backend_context] {
            skia_backend_context->check_async_work_completion();
            if (m_pending_webgl_buffer_releases == 0)
                m_webgl_buffer_release_timer->stop();
        });
    }
    if (!m_webgl_buffer_release_timer->is_active())
        m_webgl_buffer_release_timer->start();
}

void ConnectionFromWebContent::release_webgl_buffer(Compositing::CanvasId canvas_id, u64 buffer_id)
{
    auto* webgl_host = m_webgl_host;
    if (!webgl_host)
        return;
    if (auto* webgl_thread = WebGLThread::the())
        webgl_thread->post([webgl_host, canvas_id, buffer_id] { webgl_host->release_presented_buffer(canvas_id, buffer_id); });
    else
        webgl_host->release_presented_buffer(canvas_id, buffer_id);
}

void ConnectionFromWebContent::install_video_presentation_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Compositor: Could not take up a video presentation channel: {}", transport_or_error.error());
        return;
    }
    m_video_presentation_connection = adopt_ref(*new Media::VideoPresentationClientConnection(transport_or_error.release_value()));
    m_compositor_state->create_video_edges(*this);

    dbgln_if(VIDEO_PRESENTATION_CHANNEL_DEBUG, "Compositor: established video presentation channel for WebContent (client_id={})", client_id());
}

void ConnectionFromWebContent::install_render_clock_channel(IPC::TransportHandle handle)
{
    auto transport_or_error = handle.create_transport();
    if (transport_or_error.is_error()) {
        dbgln("Compositor: Could not take up a render clock channel: {}", transport_or_error.error());
        return;
    }
    // A new channel replaces the one before it, whose requests were for the contexts the process armed then.
    m_render_clock_connection = RenderClockConnection::construct(transport_or_error.release_value(), client_id());
    m_render_clock_connection->on_request_clock_tick = [this](Web::CompositorContextId context_id, double maximum_frames_per_second) {
        if (!context_is_owned_by_this_connection(context_id))
            return;
        if (!isfinite(maximum_frames_per_second) || maximum_frames_per_second <= 0) {
            did_misbehave("WebContent sent an invalid maximum clock tick rate");
            return;
        }
        m_compositor_state->request_clock_tick(context_id, maximum_frames_per_second);
    };
}

void ConnectionFromWebContent::clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds, Vector<Web::CompositorScrollOffset> const& scroll_offsets)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_clock_tick(context_id, frame_time_nanoseconds, frame_interval_milliseconds, scroll_offsets);
}

void ConnectionFromWebContent::pointer_moved(Web::CompositorContextId context_id, Web::DevicePixelPoint position, u32 buttons, bool scrolled_since_frame, u64 input_event_id)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_pointer_moved(context_id, position, buttons, scrolled_since_frame, input_event_id);
}

void ConnectionFromWebContent::pointer_left(Web::CompositorContextId context_id, u64 input_event_id)
{
    if (m_render_clock_connection)
        m_render_clock_connection->async_pointer_left(context_id, input_event_id);
}

void ConnectionFromWebContent::add_video_sink(Media::VideoSinkHandle video_sink_handle, NonnullRefPtr<Compositing::VideoFramePresentationPage> presentation_page)
{
    m_video_frame_holds.add_sink(video_sink_handle, move(presentation_page));
    m_compositor_state->add_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    m_video_frame_holds.remove_sink(video_sink_handle);
    m_canvas_host.forget_video_sink(video_sink_handle);
    m_compositor_state->remove_video_sink(*this, video_sink_handle);
}

void ConnectionFromWebContent::release_video_frame_hold(Media::VideoSinkHandle video_sink_handle, u64 generation)
{
    if (m_video_frame_holds.release_hold(video_sink_handle, generation).is_error())
        did_misbehave("WebContent released a video frame hold that it did not take");
}

void ConnectionFromWebContent::video_sink_presented_frame(Media::VideoSinkHandle video_sink_handle, RefPtr<Media::VideoFrame> frame, Media::DisplayingVideoSinkFrameCounts new_frame_counts)
{
    m_video_frame_holds.present_frame(video_sink_handle, move(frame), new_frame_counts);
}

void ConnectionFromWebContent::video_sink_counted_frames(Media::VideoSinkHandle video_sink_handle, Media::DisplayingVideoSinkFrameCounts new_frame_counts)
{
    m_video_frame_holds.add_frame_counts(video_sink_handle, new_frame_counts);
}

void ConnectionFromWebContent::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    m_compositor_state->set_video_sink_ticking(*this, video_sink_handle, should_tick);
}

void ConnectionFromWebContent::create_video_edge(Media::VideoSinkHandle video_sink_handle)
{
    if (!m_video_presentation_connection)
        return;
    auto result = m_video_presentation_connection->create_edge(video_sink_handle, [this, video_sink_handle](NonnullRefPtr<Media::DisplayingVideoSink> sink) {
        m_compositor_state->on_video_sink_ready(*this, video_sink_handle, move(sink));
    });
    if (result.is_error())
        dbgln("Compositor: Not showing video sink {}: {}", video_sink_handle.value(), result.error());
}

void ConnectionFromWebContent::release_video_edge(Media::VideoSinkHandle video_sink_handle)
{
    if (m_video_presentation_connection)
        m_video_presentation_connection->release_edge(video_sink_handle);
}

void ConnectionFromWebContent::notify_compositor_lost()
{
    async_did_lose_compositor();
}

void ConnectionFromWebContent::request_rendering_update()
{
    async_request_rendering_update();
}

void ConnectionFromWebContent::rendering_opportunity(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds)
{
    async_rendering_opportunity(context_id, frame_time_nanoseconds, frame_interval_milliseconds);
}

void ConnectionFromWebContent::async_scroll_updates(Web::CompositorContextId context_id, Compositing::PendingAsyncScrollUpdates const& updates)
{
    async_async_scroll_updates(context_id, updates);
}

void ConnectionFromWebContent::overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility> const& visibilities)
{
    async_overlay_scrollbar_visibilities_changed(visibilities);
}

void ConnectionFromWebContent::dispatch_mouse_event_to_web_content(u64 page_id, Web::MouseEvent const& event)
{
    async_mouse_event(page_id, event);
}

void ConnectionFromWebContent::dispatch_key_event_to_web_content(u64 page_id, Web::KeyEvent const& event)
{
    async_key_event(page_id, event);
}

void ConnectionFromWebContent::dispatch_pinch_event_to_web_content(u64 page_id, Web::PinchEvent const& event)
{
    async_pinch_event(page_id, event);
}

bool ConnectionFromWebContent::context_is_owned_by_this_connection(Web::CompositorContextId context_id)
{
    switch (m_compositor_state->check_context_owner(context_id, *this)) {
    case CompositorState::ContextOwnerCheckResult::OwnedByClient:
        return true;
    case CompositorState::ContextOwnerCheckResult::ContextUnavailable:
        return false;
    case CompositorState::ContextOwnerCheckResult::ConflictingOwner:
        did_misbehave("WebContent tried to use a compositor context owned by another connection");
        return false;
    }

    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (!isfinite(maximum_frames_per_second) || maximum_frames_per_second <= 0) {
        did_misbehave("WebContent sent an invalid maximum frame rate");
        return;
    }
    m_compositor_state->request_rendering_opportunity(context_id, maximum_frames_per_second);
}

void ConnectionFromWebContent::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->hurry_rendering_opportunity(context_id);
}

void ConnectionFromWebContent::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->set_parent_context(context_id, parent_context_id);
}

void ConnectionFromWebContent::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->stop_presenting_to_client(context_id);
}

void ConnectionFromWebContent::destroy_context(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->destroy_context(context_id);
}

static bool display_list_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_DISPLAY_LIST_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

void ConnectionFromWebContent::update_display_list(Web::CompositorContextId context_id, Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties display_list_properties, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction, Compositing::ScrollStateSnapshot scroll_state_snapshot)
{
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto display_list = Compositing::DisplayList::create_from_shared_buffer(move(display_list_properties), move(display_list_buffer), tape_size, run_count);
    if (display_list.is_error()) {
        dbgln("Compositor: Rejecting display list from WebContent: {}", display_list.error());
        did_misbehave("WebContent published a display list its shared buffer does not hold");
        return;
    }
    auto adopt_time = timer.elapsed_time();
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_display_list(context_id, display_list.release_value(), move(visual_context_tree), move(resource_transaction), move(scroll_state_snapshot));
    if (display_list_timing_enabled())
        dbgln("DISPLAY_LIST_RECEIVE bytes={} adopt={} µs install={} µs", tape_size, adopt_time.to_microseconds(), (timer.elapsed_time() - adopt_time).to_microseconds());
}

void ConnectionFromWebContent::update_display_list_resources(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction resource_transaction)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_display_list_resources(context_id, move(resource_transaction));
}

void ConnectionFromWebContent::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_visual_context_tree(context_id, move(visual_context_tree), move(resource_transaction));
}

void ConnectionFromWebContent::update_scroll_state(Web::CompositorContextId context_id, Compositing::ScrollStateSnapshot scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->update_scroll_state(context_id, move(scroll_state_snapshot), move(keyboard_scroll_state));
}

Messages::CompositorWebContentServer::ReserveCanvasIdsResponse ConnectionFromWebContent::reserve_canvas_ids()
{
    static constexpr u64 reserved_canvas_id_count = 1 << 20;
    if (!m_reserved_canvas_ids.is_used_up()) {
        did_misbehave("WebContent reserved canvas IDs before using up its range");
        return { Compositing::CanvasId { 0 }, 0 };
    }
    auto first_canvas_id = m_compositor_state->canvas_surface_registry().reserve_canvas_ids(reserved_canvas_id_count);
    m_reserved_canvas_ids = { first_canvas_id.value(), first_canvas_id.value() + reserved_canvas_id_count };
    return { first_canvas_id, reserved_canvas_id_count };
}

// NB: The pixels of a 2D context are in one of the predefined color spaces of a canvas, which WebContent describes by
//     coding-independent code points, or as sRGB without color space data. Nothing else can name one.
//     https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
static bool is_predefined_canvas_color_space(Gfx::ColorSpace const& color_space)
{
    return color_space.data().visit(
        [](Empty) { return true; },
        [](Gfx::ColorSpace::CICP const& cicp) {
            auto has_valid_primaries = cicp.color_primaries == Media::ColorPrimaries::BT709 || cicp.color_primaries == Media::ColorPrimaries::SMPTE432;
            auto has_valid_transfer_characteristics = cicp.transfer_characteristics == Media::TransferCharacteristics::SRGB || cicp.transfer_characteristics == Media::TransferCharacteristics::Linear;
            return has_valid_primaries && has_valid_transfer_characteristics;
        },
        [](NonnullRefPtr<Gfx::ColorSpace::ICCProfile const> const&) { return false; });
}

void ConnectionFromWebContent::create_canvas_2d_context(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool alpha, bool will_read_frequently, Gfx::ColorSpace color_space)
{
    // NB: WebContent names its contexts with the IDs of its range in order, so each ID names one context only.
    if (m_reserved_canvas_ids.is_used_up() || canvas_id.value() != m_reserved_canvas_ids.next) {
        did_misbehave("WebContent named a 2D canvas context with an ID other than the next one it reserved");
        return;
    }
    if (!is_predefined_canvas_color_space(color_space)) {
        did_misbehave("WebContent asked for a 2D canvas context in a color space that is not a predefined one");
        return;
    }
    ++m_reserved_canvas_ids.next;
    // NB: A context that cannot be created is not a protocol violation: commands recorded for it are dropped.
    (void)m_canvas_host.create_2d_context(canvas_id, size, alpha, will_read_frequently, color_space);
}

void ConnectionFromWebContent::update_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> segments, Vector<Compositing::FontResourceId> kept_font_ids, Vector<Compositing::DisplayListFontResource> added_fonts, Vector<Compositing::DisplayListImageFrameResource> image_frames)
{
    for (auto const& segment : segments) {
        if (Compositing::validate_canvas_2d_tape(segment.tape).is_error()) {
            did_misbehave("WebContent sent a canvas 2D stream that does not validate");
            return;
        }
    }
    m_canvas_host.execute_canvas_2d_stream(segments, kept_font_ids, added_fonts, image_frames);
}

void ConnectionFromWebContent::destroy_canvas_context(Compositing::CanvasId canvas_id)
{
    m_canvas_host.destroy_context(canvas_id);
}

Messages::CompositorWebContentServer::GetCanvasPixelsResponse ConnectionFromWebContent::get_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect, Gfx::AlphaType alpha_type, Gfx::ColorSpace color_space)
{
    if (!Gfx::is_valid_alpha_type(to_underlying(alpha_type))) {
        did_misbehave("WebContent asked for canvas pixels with an invalid alpha type");
        return Gfx::ShareableBitmap {};
    }
    if (!is_predefined_canvas_color_space(color_space)) {
        did_misbehave("WebContent asked for canvas pixels in a color space that is not a predefined one");
        return Gfx::ShareableBitmap {};
    }
    return m_canvas_host.read_back_pixels(canvas_id, rect, alpha_type, color_space);
}

Messages::CompositorWebContentServer::RasterizeDisplayListResponse ConnectionFromWebContent::rasterize_display_list(Core::AnonymousBuffer display_list_buffer, u64 tape_size, u64 run_count, Compositing::DisplayList::Properties display_list_properties, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction resource_transaction, Gfx::ShareableBitmap target_bitmap)
{
    auto display_list = Compositing::DisplayList::create_from_shared_buffer(move(display_list_properties), move(display_list_buffer), tape_size, run_count);
    if (display_list.is_error()) {
        dbgln("Compositor: Rejecting display list to rasterize from WebContent: {}", display_list.error());
        did_misbehave("WebContent asked to rasterize a display list its shared buffer does not hold");
        return false;
    }
    if (!target_bitmap.is_valid())
        return false;
    if (auto result = Compositor::rasterize_display_list(*display_list.value(), visual_context_tree, move(resource_transaction), *target_bitmap.bitmap()); result.is_error()) {
        dbgln("Compositor: Not rasterizing display list from WebContent: {}", result.error());
        return false;
    }
    return true;
}

Messages::CompositorWebContentServer::AllocatePlaceholderCanvasResponse ConnectionFromWebContent::allocate_placeholder_canvas()
{
    auto allocation = m_compositor_state->allocate_placeholder_canvas(*this);
    return { allocation.canvas_id, allocation.secret };
}

void ConnectionFromWebContent::release_placeholder_canvas(Compositing::CanvasId canvas_id)
{
    m_compositor_state->release_placeholder_canvas(*this, canvas_id);
}

void ConnectionFromWebContent::commit_placeholder_canvas(Compositing::CanvasId canvas_id, u64 secret, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize size, bool origin_clean)
{
    RefPtr<Gfx::PaintingSurface> source_surface;
    if (source_canvas_id.has_value()) {
        source_surface = m_canvas_host.presented_surface(*source_canvas_id);
        if (!source_surface) {
            if (auto canvas = m_webgl_canvases.get(*source_canvas_id); canvas.has_value() && canvas->presented_buffer_id.has_value())
                source_surface = canvas->surfaces.get(*canvas->presented_buffer_id)->surface;
        }
        if (!source_surface)
            return;
    }
    m_compositor_state->commit_placeholder_canvas(canvas_id, secret, move(source_surface), size, origin_clean);
}

Messages::CompositorWebContentServer::GetPlaceholderCanvasPixelsResponse ConnectionFromWebContent::get_placeholder_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto result = m_compositor_state->read_placeholder_canvas_pixels(*this, canvas_id, rect);
    return { move(result.pixels), result.origin_clean };
}

void ConnectionFromWebContent::placeholder_canvas_committed(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool origin_clean)
{
    async_placeholder_canvas_committed(canvas_id, size, origin_clean);
}

// NB: WebGL messages go to the WebGL host as they come in; see dispatch_decision_for().
Messages::CompositorWebContentServer::CreateWebglContextResponse ConnectionFromWebContent::create_webgl_context(Compositing::WebGL::WebGLVersion, Gfx::IntSize, bool, bool, bool)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_set_command_buffer(Compositing::CanvasId, Core::AnonymousBuffer)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_commands_from_shared_buffer(Compositing::CanvasId, u64, u64, u64, Vector<Compositing::WebGL::ImageSourceFrame>)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_drain_command_buffer(Compositing::CanvasId)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_commands(Compositing::CanvasId, Core::AnonymousBuffer, Vector<Compositing::WebGL::ImageSourceFrame>)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_present_canvas(Compositing::CanvasId, bool)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::webgl_clear_drawing_buffer(Compositing::CanvasId)
{
    VERIFY_NOT_REACHED();
}

Messages::CompositorWebContentServer::WebglSyncCallResponse ConnectionFromWebContent::webgl_sync_call(Compositing::CanvasId, ByteBuffer)
{
    VERIFY_NOT_REACHED();
}

Messages::CompositorWebContentServer::WebglReadPixelsResponse ConnectionFromWebContent::webgl_read_pixels(Compositing::CanvasId, i32, i32, i32, i32, u32, u32, i32, Core::AnonymousBuffer)
{
    VERIFY_NOT_REACHED();
}

Messages::CompositorWebContentServer::WebglReadBufferSubDataResponse ConnectionFromWebContent::webgl_read_buffer_sub_data(Compositing::CanvasId, u32, i64, i64, Core::AnonymousBuffer)
{
    VERIFY_NOT_REACHED();
}

void ConnectionFromWebContent::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    if (context_is_owned_by_this_connection(context_id))
        m_compositor_state->invalidate_keyboard_scroll_state(context_id, generation);
}

void ConnectionFromWebContent::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->invalidate_wheel_event_listener_state(context_id, generation);
}

Messages::CompositorWebContentServer::AsyncScrollByResponse ConnectionFromWebContent::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelStepDispatch wheel_step_dispatch, u64 input_event_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::AsyncScrollEnqueueResult {};
    auto result = m_compositor_state->async_scroll_by(context_id, document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking, wheel_step_dispatch, input_event_id);
    if (result.accepted)
        async_request_rendering_update();
    return result;
}

static bool is_valid_scroll_request(Compositing::ScrollRequest const& scroll_request)
{
    auto is_finite = [](Gfx::FloatPoint point) { return isfinite(point.x()) && isfinite(point.y()); };
    if (scroll_request.jump_offset.has_value() && !is_finite(scroll_request.jump_offset.value()))
        return false;
    if (auto const& relative_scroll = scroll_request.relative_scroll; relative_scroll.has_value()) {
        if (!is_finite(relative_scroll->delta) || !is_finite(relative_scroll->lower_bound) || !is_finite(relative_scroll->upper_bound))
            return false;
        if (relative_scroll->lower_bound.x() > relative_scroll->upper_bound.x() || relative_scroll->lower_bound.y() > relative_scroll->upper_bound.y())
            return false;
    }
    if (scroll_request.visual_viewport_scale.has_value()) {
        auto scale = scroll_request.visual_viewport_scale.value();
        if (!isfinite(scale) || scale <= 0)
            return false;
    }
    if (auto const& smooth_scroll = scroll_request.smooth_scroll; smooth_scroll.has_value()) {
        if (!is_finite(smooth_scroll->destination_offset))
            return false;
        // WebContent names the compositor's own scrolls only to head them somewhere else.
        if (smooth_scroll->operation_id < Compositing::first_operation_id_the_main_thread_requests && !smooth_scroll->retargets_operation)
            return false;
    }
    return true;
}

void ConnectionFromWebContent::apply_scroll_requests(Web::CompositorContextId context_id, Vector<Compositing::ScrollRequest> scroll_requests)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (!all_of(scroll_requests, is_valid_scroll_request)) {
        did_misbehave("Invalid scroll request");
        return;
    }
    m_compositor_state->apply_scroll_requests(context_id, scroll_requests);
}

Messages::CompositorWebContentServer::ScrollStateForTestingResponse ConnectionFromWebContent::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller)
{
    if (!context_is_owned_by_this_connection(context_id))
        return String {};
    return m_compositor_state->scroll_state_for_testing(context_id, stretched_scroller);
}

Messages::CompositorWebContentServer::RouteFirstWheelStepForTestingResponse ConnectionFromWebContent::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    if (!context_is_owned_by_this_connection(context_id))
        return { {}, {}, {} };
    auto result = m_compositor_state->route_first_wheel_step_for_testing(context_id, position, delta_in_css_pixels);
    if (!result.has_value())
        return { {}, {}, {} };
    return { result->route.decision, result->route.scroller, result->document_id };
}

void ConnectionFromWebContent::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->begin_input_trace_for_testing(context_id);
}

void ConnectionFromWebContent::clear_per_test_input_state_for_testing(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->clear_per_test_input_state_for_testing(context_id);
}

Messages::CompositorWebContentServer::TakeInputTraceForTestingResponse ConnectionFromWebContent::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Vector<Web::InputTraceRecord> {};
    return m_compositor_state->take_input_trace_for_testing(context_id);
}

Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdatesResponse ConnectionFromWebContent::take_pending_async_scroll_updates(Web::CompositorContextId context_id)
{
    if (!context_is_owned_by_this_connection(context_id))
        return Compositing::PendingAsyncScrollUpdates {};
    return m_compositor_state->take_pending_async_scroll_updates(context_id);
}

void ConnectionFromWebContent::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (!isfinite(device_pixel_ratio) || device_pixel_ratio <= 0) {
        did_misbehave("WebContent sent an invalid device pixel ratio");
        return;
    }
    m_compositor_state->viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

void ConnectionFromWebContent::present_frame(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    m_compositor_state->present_frame(context_id, viewport_rect);
}

void ConnectionFromWebContent::request_screenshot(Web::CompositorContextId context_id, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap)
{
    if (!context_is_owned_by_this_connection(context_id))
        return;
    if (m_compositor_state->request_screenshot(context_id, target_bitmap))
        async_did_complete_screenshot(request_id);
    else
        async_did_fail_screenshot(request_id);
}

}
