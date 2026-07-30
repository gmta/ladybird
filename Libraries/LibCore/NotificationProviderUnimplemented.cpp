/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/NotificationProviderImplementation.h>

namespace Core {

bool platform_notification_provider_is_available()
{
    return false;
}

ErrorOr<NonnullOwnPtr<NotificationProvider>> create_platform_notification_provider()
{
    return Error::from_string_literal("Notifications are not available for this platform");
}

}
