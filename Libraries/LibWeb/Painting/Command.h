/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibGfx/TextAlignment.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>
#include <LibWeb/Painting/GradientData.h>
#include <LibWeb/Painting/PaintBoxShadowParams.h>
#include <LibWeb/Painting/PaintStyle.h>

namespace Web::Painting {

class DisplayList;

struct DrawGlyphRun {
    NonnullRefPtr<Gfx::GlyphRun> glyph_run;
    double scale { 1 };
    Gfx::IntRect rect;
    Gfx::FloatPoint translation;
    Color color;
    Gfx::Orientation orientation { Gfx::Orientation::Horizontal };

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct FillRect {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct DrawPaintingSurface {
    Gfx::IntRect dst_rect;
    NonnullRefPtr<Gfx::PaintingSurface> surface;
    Gfx::IntRect src_rect;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return dst_rect; }
};

struct DrawScaledImmutableBitmap {
    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap;
    Gfx::ScalingMode scaling_mode;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return clip_rect; }
};

struct DrawRepeatedImmutableBitmap {
    struct Repeat {
        bool x { false };
        bool y { false };
    };

    Gfx::IntRect dst_rect;
    Gfx::IntRect clip_rect;
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap;
    Gfx::ScalingMode scaling_mode;
    Repeat repeat;
};

struct Save { };
struct SaveLayer { };
struct Restore { };

struct Translate {
    Gfx::IntPoint delta;
};

struct AddClipRect {
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip_or_mask() const { return true; }
};

struct StackingContextTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;
};

struct PushStackingContext {
    float opacity;
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator;
    bool isolate;
    // The bounding box of the source paintable (pre-transform).
    Gfx::IntRect source_paintable_rect;
    // A translation to be applied after the stacking context has been transformed.
    StackingContextTransform transform;
    Optional<Gfx::Path> clip_path = {};
};

struct PopStackingContext { };

struct PaintLinearGradient {
    Gfx::IntRect gradient_rect;
    LinearGradientData linear_gradient_data;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return gradient_rect; }
};

struct PaintOuterBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
};

struct PaintInnerBoxShadow {
    PaintBoxShadowParams box_shadow_params;

    [[nodiscard]] Gfx::IntRect bounding_rect() const;
};

struct PaintTextShadow {
    NonnullRefPtr<Gfx::GlyphRun> glyph_run;
    double glyph_run_scale { 1 };
    Gfx::IntRect shadow_bounding_rect;
    Gfx::IntRect text_rect;
    Gfx::FloatPoint draw_location;
    int blur_radius;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return { draw_location.to_type<int>(), shadow_bounding_rect.size() }; }
};

struct FillRectWithRoundedCorners {
    Gfx::IntRect rect;
    Color color;
    CornerRadii corner_radii;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct FillPathUsingColor {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    Color color;
    Gfx::WindingRule winding_rule;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }
};

struct FillPathUsingPaintStyle {
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    PaintStyle paint_style;
    Gfx::WindingRule winding_rule;
    float opacity;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }
};

struct StrokePathUsingColor {
    Gfx::Path::CapStyle cap_style;
    Gfx::Path::JoinStyle join_style;
    float miter_limit;
    Vector<float> dash_array;
    float dash_offset;
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    Color color;
    float thickness;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }
};

struct StrokePathUsingPaintStyle {
    Gfx::Path::CapStyle cap_style;
    Gfx::Path::JoinStyle join_style;
    float miter_limit;
    Vector<float> dash_array;
    float dash_offset;
    Gfx::IntRect path_bounding_rect;
    Gfx::Path path;
    PaintStyle paint_style;
    float thickness;
    float opacity = 1.0f;
    Gfx::FloatPoint aa_translation;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return path_bounding_rect; }
};

struct DrawEllipse {
    Gfx::IntRect rect;
    Color color;
    int thickness;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct FillEllipse {
    Gfx::IntRect rect;
    Color color;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct DrawLine {
    Color color;
    Gfx::IntPoint from;
    Gfx::IntPoint to;
    int thickness;
    Gfx::LineStyle style;
    Color alternate_color;
};

struct ApplyBackdropFilter {
    Gfx::IntRect backdrop_region;
    BorderRadiiData border_radii_data;
    Vector<Gfx::Filter> backdrop_filter;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return backdrop_region; }
};

struct DrawRect {
    Gfx::IntRect rect;
    Color color;
    bool rough;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct PaintRadialGradient {
    Gfx::IntRect rect;
    RadialGradientData radial_gradient_data;
    Gfx::IntPoint center;
    Gfx::IntSize size;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct PaintConicGradient {
    Gfx::IntRect rect;
    ConicGradientData conic_gradient_data;
    Gfx::IntPoint position;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct DrawTriangleWave {
    Gfx::IntPoint p1;
    Gfx::IntPoint p2;
    Color color;
    int amplitude;
    int thickness;
};

struct AddRoundedRectClip {
    CornerRadii corner_radii;
    Gfx::IntRect border_rect;
    CornerClip corner_clip;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return border_rect; }
    bool is_clip_or_mask() const { return true; }
};

struct AddMask {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
    bool is_clip_or_mask() const { return true; }
};

struct PaintNestedDisplayList {
    RefPtr<DisplayList> display_list;
    Gfx::IntRect rect;

    [[nodiscard]] Gfx::IntRect bounding_rect() const { return rect; }
};

struct PaintScrollBar {
    int scroll_frame_id;
    Gfx::IntRect rect;
    CSSPixelFraction scroll_size;
    bool vertical;
};

struct ApplyOpacity {
    float opacity;
};

struct ApplyCompositeAndBlendingOperator {
    Gfx::CompositingAndBlendingOperator compositing_and_blending_operator;
};

struct ApplyFilters {
    Vector<Gfx::Filter> filter;
};

struct ApplyTransform {
    Gfx::FloatPoint origin;
    Gfx::FloatMatrix4x4 matrix;
};

struct ApplyMaskBitmap {
    Gfx::IntPoint origin;
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap;
    Gfx::Bitmap::MaskKind kind;
};

using Command = Variant<
    DrawGlyphRun,
    FillRect,
    DrawPaintingSurface,
    DrawScaledImmutableBitmap,
    DrawRepeatedImmutableBitmap,
    Save,
    SaveLayer,
    Restore,
    Translate,
    AddClipRect,
    PushStackingContext,
    PopStackingContext,
    PaintLinearGradient,
    PaintRadialGradient,
    PaintConicGradient,
    PaintOuterBoxShadow,
    PaintInnerBoxShadow,
    PaintTextShadow,
    FillRectWithRoundedCorners,
    FillPathUsingColor,
    FillPathUsingPaintStyle,
    StrokePathUsingColor,
    StrokePathUsingPaintStyle,
    DrawEllipse,
    FillEllipse,
    DrawLine,
    ApplyBackdropFilter,
    DrawRect,
    DrawTriangleWave,
    AddRoundedRectClip,
    AddMask,
    PaintNestedDisplayList,
    PaintScrollBar,
    ApplyOpacity,
    ApplyCompositeAndBlendingOperator,
    ApplyFilters,
    ApplyTransform,
    ApplyMaskBitmap>;
}
