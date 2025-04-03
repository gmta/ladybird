/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/DisplayList.h>

namespace Web::Painting {

void DisplayList::append(Command&& command, Optional<i32> scroll_frame_id)
{
    m_commands.append({ scroll_frame_id, move(command) });
}

static Optional<Gfx::IntRect> command_bounding_rectangle(Command const& command)
{
    return command.visit(
        [&](auto const& command) -> Optional<Gfx::IntRect> {
            if constexpr (requires { command.bounding_rect(); })
                return command.bounding_rect();
            else
                return {};
        });
}

static bool command_is_clip_or_mask(Command const& command)
{
    return command.visit(
        [&](auto const& command) -> bool {
            if constexpr (requires { command.is_clip_or_mask(); })
                return command.is_clip_or_mask();
            else
                return false;
        });
}

void DisplayListPlayer::execute(DisplayList& display_list, NonnullRefPtr<Gfx::PaintingSurface> surface)
{
    surface->lock_context();
    execute_impl(display_list, surface);
    surface->unlock_context();
}

void DisplayListPlayer::execute_impl(DisplayList& display_list, NonnullRefPtr<Gfx::PaintingSurface> surface)
{
    ScopeGuard guard = [this, old_surface = m_surface] {
        m_surface = old_surface;
    };
    m_surface = surface;

    auto const& commands = display_list.commands();
    auto const& scroll_state = display_list.scroll_state();
    auto device_pixels_per_css_pixel = display_list.device_pixels_per_css_pixel();

    Optional<int> last_scroll_frame_id;
    Gfx::IntPoint last_scroll_frame_offset;

    for (size_t command_index = 0; command_index < commands.size(); command_index++) {
        auto scroll_frame_id = commands[command_index].scroll_frame_id;
        auto command = commands[command_index].command;

        if (command.has<PaintScrollBar>()) {
            auto& paint_scroll_bar = command.get<PaintScrollBar>();
            auto scroll_offset = scroll_state.own_offset_for_frame_with_id(paint_scroll_bar.scroll_frame_id);
            if (paint_scroll_bar.vertical) {
                auto offset = scroll_offset.y() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.rect.translate_by(0, -offset.to_int() * device_pixels_per_css_pixel);
            } else {
                auto offset = scroll_offset.x() * paint_scroll_bar.scroll_size;
                paint_scroll_bar.rect.translate_by(-offset.to_int() * device_pixels_per_css_pixel, 0);
            }
        }

        auto scroll_rect = surface->rect();
        Gfx::IntPoint scroll_offset;
        if (scroll_frame_id.has_value()) {
            auto cumulative_offset = scroll_state.cumulative_offset_for_frame_with_id(scroll_frame_id.value());
            scroll_offset = cumulative_offset.to_type<double>().scaled(device_pixels_per_css_pixel).to_type<int>();
            scroll_rect.translate_by(-scroll_offset);
        }

        auto bounding_rect = command_bounding_rectangle(command);
        if (bounding_rect.has_value() && !bounding_rect->intersects(scroll_rect)) {
            dbgln("relevant! scroll_rect {} bounding_rect {}", scroll_rect, bounding_rect);
            // Any clip or mask that's located outside of the visible region is equivalent to a simple clip-rect,
            // so replace it with one to avoid doing unnecessary work.
            if (command_is_clip_or_mask(command)) {
            //     if (command.has<AddClipRect>()) {
            //         add_clip_rect(command.get<AddClipRect>());
            //     } else {
            //         add_clip_rect({ bounding_rect.release_value() });
            //     }
            // } else {
            //     continue;
            }
            if (command.has<AddMask>())
                add_clip_rect({ bounding_rect.release_value() });
            else
                continue;
        }

        // Apply scroll offset when scroll frame ID changes
        if (last_scroll_frame_id != scroll_frame_id) {
            if (last_scroll_frame_id.has_value())
                translate({ -last_scroll_frame_offset });
            translate({ scroll_offset });

            last_scroll_frame_id = scroll_frame_id;
            last_scroll_frame_offset = scroll_offset;
        }

#define HANDLE_COMMAND(command_type, executor_method) \
    if (command.has<command_type>()) {                \
        executor_method(command.get<command_type>()); \
    }

        // clang-format off
        HANDLE_COMMAND(DrawGlyphRun, draw_glyph_run)
        else HANDLE_COMMAND(FillRect, fill_rect)
        else HANDLE_COMMAND(DrawPaintingSurface, draw_painting_surface)
        else HANDLE_COMMAND(DrawScaledImmutableBitmap, draw_scaled_immutable_bitmap)
        else HANDLE_COMMAND(DrawRepeatedImmutableBitmap, draw_repeated_immutable_bitmap)
        else HANDLE_COMMAND(AddClipRect, add_clip_rect)
        else HANDLE_COMMAND(Save, save)
        else HANDLE_COMMAND(SaveLayer, save_layer)
        else HANDLE_COMMAND(Restore, restore)
        else HANDLE_COMMAND(Translate, translate)
        else HANDLE_COMMAND(PushStackingContext, push_stacking_context)
        else HANDLE_COMMAND(PopStackingContext, pop_stacking_context)
        else HANDLE_COMMAND(PaintLinearGradient, paint_linear_gradient)
        else HANDLE_COMMAND(PaintRadialGradient, paint_radial_gradient)
        else HANDLE_COMMAND(PaintConicGradient, paint_conic_gradient)
        else HANDLE_COMMAND(PaintOuterBoxShadow, paint_outer_box_shadow)
        else HANDLE_COMMAND(PaintInnerBoxShadow, paint_inner_box_shadow)
        else HANDLE_COMMAND(PaintTextShadow, paint_text_shadow)
        else HANDLE_COMMAND(FillRectWithRoundedCorners, fill_rect_with_rounded_corners)
        else HANDLE_COMMAND(FillPathUsingColor, fill_path_using_color)
        else HANDLE_COMMAND(FillPathUsingPaintStyle, fill_path_using_paint_style)
        else HANDLE_COMMAND(StrokePathUsingColor, stroke_path_using_color)
        else HANDLE_COMMAND(StrokePathUsingPaintStyle, stroke_path_using_paint_style)
        else HANDLE_COMMAND(DrawEllipse, draw_ellipse)
        else HANDLE_COMMAND(FillEllipse, fill_ellipse)
        else HANDLE_COMMAND(DrawLine, draw_line)
        else HANDLE_COMMAND(ApplyBackdropFilter, apply_backdrop_filter)
        else HANDLE_COMMAND(DrawRect, draw_rect)
        else HANDLE_COMMAND(DrawTriangleWave, draw_triangle_wave)
        else HANDLE_COMMAND(AddRoundedRectClip, add_rounded_rect_clip)
        else HANDLE_COMMAND(AddMask, add_mask)
        else HANDLE_COMMAND(PaintScrollBar, paint_scrollbar)
        else HANDLE_COMMAND(PaintNestedDisplayList, paint_nested_display_list)
        else HANDLE_COMMAND(ApplyOpacity, apply_opacity)
        else HANDLE_COMMAND(ApplyCompositeAndBlendingOperator, apply_composite_and_blending_operator)
        else HANDLE_COMMAND(ApplyFilters, apply_filters)
        else HANDLE_COMMAND(ApplyTransform, apply_transform)
        else HANDLE_COMMAND(ApplyMaskBitmap, apply_mask_bitmap)
        else VERIFY_NOT_REACHED();
        // clang-format on
    }

    flush();
}

}
