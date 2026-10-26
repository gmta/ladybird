/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Platform/RemoteImageCodecPlugin.h>

namespace Web::Platform {

// The decoder is only asked for once there is an image to decode, so a process that shows no image never has one.
RemoteImageCodecPlugin::RemoteImageCodecPlugin(Function<void()> request_client)
    : m_request_client(move(request_client))
{
}

// Without a client, the decoder was refused, and the images waiting for it cannot be decoded.
void RemoteImageCodecPlugin::set_client(RefPtr<ImageDecoderClient::Client> client)
{
    m_client_is_requested = false;
    m_client = move(client);
    auto pending_decodes = move(m_pending_decodes);

    if (!m_client) {
        m_client_was_refused = true;
        for (auto& pending_decode : pending_decodes)
            pending_decode.promise->reject(Error::from_string_literal("No image decoder is available"));
        return;
    }

    setup_client_callbacks();
    for (auto& pending_decode : pending_decodes)
        decode_with_client(pending_decode.bytes, move(pending_decode.promise));
}

void RemoteImageCodecPlugin::setup_client_callbacks()
{
    m_client->on_death = [this] {
        m_client = nullptr;
    };
    m_client->on_animation_frames_decoded = [this](i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps) {
        if (on_animation_frames_decoded)
            on_animation_frames_decoded(session_id, move(bitmaps));
    };
    m_client->on_animation_decode_failed = [this](i64 session_id, String) {
        if (on_animation_decode_failed)
            on_animation_decode_failed(session_id);
    };
}

RemoteImageCodecPlugin::~RemoteImageCodecPlugin() = default;

NonnullRefPtr<Core::Promise<DecodedImage>> RemoteImageCodecPlugin::decode_image(ReadonlyBytes bytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected)
{
    auto promise = Core::Promise<DecodedImage>::construct();
    if (on_resolved)
        promise->on_resolution = move(on_resolved);
    if (on_rejected)
        promise->on_rejection = move(on_rejected);

    if (m_client) {
        decode_with_client(bytes, promise);
        return promise;
    }
    if (m_client_was_refused) {
        promise->reject(Error::from_string_literal("No image decoder is available"));
        return promise;
    }

    auto bytes_copy = ByteBuffer::copy(bytes);
    if (bytes_copy.is_error()) {
        promise->reject(bytes_copy.release_error());
        return promise;
    }
    m_pending_decodes.append({ bytes_copy.release_value(), promise });
    if (!m_client_is_requested) {
        m_client_is_requested = true;
        m_request_client();
    }
    return promise;
}

void RemoteImageCodecPlugin::decode_with_client(ReadonlyBytes bytes, NonnullRefPtr<Core::Promise<DecodedImage>> promise)
{
    auto image_decoder_promise = m_client->decode_image(
        bytes,
        [promise](ImageDecoderClient::DecodedImage& result) -> ErrorOr<void> {
            // FIXME: Remove this codec plugin and just use the ImageDecoderClient directly to avoid these copies
            DecodedImage decoded_image;
            decoded_image.is_animated = result.is_animated;
            decoded_image.loop_count = result.loop_count;
            decoded_image.frame_count = result.frame_count;
            decoded_image.session_id = result.session_id;
            decoded_image.all_durations = move(result.all_durations);
            for (auto& frame : result.frames) {
                decoded_image.frames.empend(move(frame.bitmap), frame.duration);
            }
            decoded_image.color_space = move(result.color_space);
            promise->resolve(move(decoded_image));
            return {};
        },
        [promise](auto& error) {
            promise->reject(Error::copy(error));
        });
}

void RemoteImageCodecPlugin::request_animation_frames(i64 session_id, u32 start_frame_index, u32 count)
{
    if (m_client)
        m_client->request_animation_frames(session_id, start_frame_index, count);
}

void RemoteImageCodecPlugin::stop_animation_decode(i64 session_id)
{
    if (m_client)
        m_client->stop_animation_decode(session_id);
}

}
