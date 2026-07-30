/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>

namespace Web::NotificationsAPI {

// The subset of a notification that the browser process needs in order to display it using the platform's
// notification API.
struct PlatformNotification {
    u64 id { 0 };
    Utf16String title;
    Utf16String body;
    Utf16String language;
    Optional<URL::URL> icon_url;
    Optional<URL::URL> image_url;
    Optional<URL::URL> badge_url;
    bool silent { false };
    bool require_interaction { false };
    bool renotify { false };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::NotificationsAPI::PlatformNotification const&);

template<>
WEB_API ErrorOr<Web::NotificationsAPI::PlatformNotification> decode(Decoder&);

}
