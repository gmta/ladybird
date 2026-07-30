/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/NotificationsAPI/PlatformNotification.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NotificationsAPI::PlatformNotification const& notification)
{
    TRY(encoder.encode(notification.id));
    TRY(encoder.encode(notification.title));
    TRY(encoder.encode(notification.body));
    TRY(encoder.encode(notification.language));
    TRY(encoder.encode(notification.icon_url));
    TRY(encoder.encode(notification.image_url));
    TRY(encoder.encode(notification.badge_url));
    TRY(encoder.encode(notification.silent));
    TRY(encoder.encode(notification.require_interaction));
    TRY(encoder.encode(notification.renotify));
    return {};
}

template<>
ErrorOr<Web::NotificationsAPI::PlatformNotification> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<u64>());
    auto title = TRY(decoder.decode<Utf16String>());
    auto body = TRY(decoder.decode<Utf16String>());
    auto language = TRY(decoder.decode<Utf16String>());
    auto icon_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto image_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto badge_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto silent = TRY(decoder.decode<bool>());
    auto require_interaction = TRY(decoder.decode<bool>());
    auto renotify = TRY(decoder.decode<bool>());

    return Web::NotificationsAPI::PlatformNotification {
        id,
        move(title),
        move(body),
        move(language),
        move(icon_url),
        move(image_url),
        move(badge_url),
        silent,
        require_interaction,
        renotify,
    };
}
