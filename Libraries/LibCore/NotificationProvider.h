/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <LibCore/Export.h>

namespace Core {

struct Notification {
    String title;
    String body;
    String language;
    bool silent { false };
    bool require_interaction { false };
};

class CORE_API NotificationProvider {
public:
    using NotificationId = u64;
    using IsAvailable = bool (*)();
    using CreateProvider = ErrorOr<NonnullOwnPtr<NotificationProvider>> (*)();

    static bool is_available();
    static ErrorOr<NonnullOwnPtr<NotificationProvider>> create();
    static void set_provider_functions(CreateProvider, IsAvailable);

    virtual ~NotificationProvider() = default;

    virtual ErrorOr<void> show(NotificationId, Notification const&) = 0;
    virtual void close(NotificationId) = 0;

    // Invoked when the end user activates a notification, e.g. by clicking on it.
    Function<void(NotificationId)> on_activated;

    // Invoked when a notification disappears, either because the end user dismissed it or because the notification
    // platform expired it.
    Function<void(NotificationId)> on_closed;
};

}
