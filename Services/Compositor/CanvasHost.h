/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>

#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/Forward.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibCompositing/Forward.h>
#include <LibCompositing/Types.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibMedia/VideoSinkHandle.h>
#include <LibMediaCodecs/Forward.h>

class SkImage;

template<typename T>
class sk_sp;

namespace Compositing {

class CanvasSurfaceRegistry;

}

namespace Compositor {

class Canvas2DCommandPlayer;

class CanvasHost {
public:
    CanvasHost(RefPtr<Gfx::SkiaBackendContext>, CanvasSurfaceRegistry&, VideoFrameHolds const&);
    ~CanvasHost();

    bool create_2d_context(Compositing::CanvasId, Gfx::IntSize, bool alpha, bool will_read_frequently, Gfx::ColorSpace const&);
    bool has_context(Compositing::CanvasId canvas_id) const { return m_contexts.contains(canvas_id); }
    void destroy_context(Compositing::CanvasId);

    void execute_canvas_2d_stream(Vector<Compositing::Canvas2DCommandStreamSegment> const&, Vector<Compositing::FontResourceId> const& kept_font_ids, Vector<Compositing::DisplayListFontResource> const& added_fonts, Vector<Compositing::DisplayListImageFrameResource> const& image_frames);

    Compositing::CanvasSurfaceOwnerId canvas_surface_owner_id() const { return m_canvas_surface_owner_id; }

    // The pixels come back in the color space.
    Gfx::ShareableBitmap read_back_pixels(Compositing::CanvasId, Gfx::IntRect, Gfx::AlphaType, Gfx::ColorSpace const& = {});
    RefPtr<Gfx::PaintingSurface> presented_surface(Compositing::CanvasId);

    void forget_video_sink(Media::VideoSinkHandle);

    static Gfx::ShareableBitmap read_back_surface(Gfx::PaintingSurface&, Gfx::IntRect, Gfx::AlphaType = Gfx::AlphaType::Premultiplied, Gfx::ColorSpace const& = {});

private:
    struct Canvas2DContext {
        NonnullOwnPtr<Canvas2DCommandPlayer> command_player;
        NonnullRefPtr<Gfx::PaintingSurface> presented_surface;
        bool has_uncommitted_commands { false };
    };

    Canvas2DContext* context(Compositing::CanvasId);
    OwnPtr<Canvas2DCommandPlayer> create_2d_command_player(Gfx::IntSize, bool alpha, bool will_read_frequently, Gfx::ColorSpace const&);
    void present_canvas_2d_context(Compositing::CanvasId, Canvas2DContext&);
    sk_sp<SkImage> held_video_frame_image(Media::VideoSinkHandle, u64 generation, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context_of_canvas);

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    CanvasSurfaceRegistry& m_canvas_surface_registry;
    Compositing::CanvasSurfaceOwnerId m_canvas_surface_owner_id;
    HashMap<Compositing::CanvasId, Canvas2DContext> m_contexts;
    Compositing::DisplayListResourceStorage m_text_resources;
    // The image frames of the stream batch that plays.
    HashMap<u64, Gfx::DecodedImageFrame> m_image_frames_of_batch;

    VideoFrameHolds const& m_video_frame_holds;
    struct VideoFrameImage;
    HashMap<Media::VideoSinkHandle, NonnullOwnPtr<VideoFrameImage>> m_video_frame_images;
    bool m_drew_video_frame_in_segment { false };
};

}
