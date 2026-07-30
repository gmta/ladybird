/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/NotificationProviderImplementation.h>

namespace Core {

static NotificationProvider::CreateProvider s_create_provider = create_platform_notification_provider;
static NotificationProvider::IsAvailable s_is_available = platform_notification_provider_is_available;

bool NotificationProvider::is_available()
{
    return s_is_available();
}

ErrorOr<NonnullOwnPtr<NotificationProvider>> NotificationProvider::create()
{
    return s_create_provider();
}

void NotificationProvider::set_provider_functions(CreateProvider create_provider, IsAvailable is_available)
{
    s_create_provider = create_provider;
    s_is_available = is_available;
}

}
