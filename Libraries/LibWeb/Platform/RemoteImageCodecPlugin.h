/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibImageDecoderClient/Client.h>
#include <LibWeb/Export.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::Platform {

class WEB_API RemoteImageCodecPlugin final : public ImageCodecPlugin {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit RemoteImageCodecPlugin(Function<void()> request_client);
    virtual ~RemoteImageCodecPlugin() override;

    virtual NonnullRefPtr<Core::Promise<DecodedImage>> decode_image(ReadonlyBytes, Function<ErrorOr<void>(DecodedImage&)> on_resolved, Function<void(Error&)> on_rejected) override;

    virtual void request_animation_frames(i64 session_id, u32 start_frame_index, u32 count) override;
    virtual void stop_animation_decode(i64 session_id) override;

    void set_client(RefPtr<ImageDecoderClient::Client>);

private:
    void setup_client_callbacks();
    void decode_with_client(ReadonlyBytes, NonnullRefPtr<Core::Promise<DecodedImage>>);

    struct PendingDecode {
        ByteBuffer bytes;
        NonnullRefPtr<Core::Promise<DecodedImage>> promise;
    };

    RefPtr<ImageDecoderClient::Client> m_client;
    Function<void()> m_request_client;
    Vector<PendingDecode> m_pending_decodes;
    bool m_client_is_requested { false };
    bool m_client_was_refused { false };
};

}
