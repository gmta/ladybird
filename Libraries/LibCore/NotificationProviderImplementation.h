/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/NotificationProvider.h>

namespace Core {

bool platform_notification_provider_is_available();
ErrorOr<NonnullOwnPtr<NotificationProvider>> create_platform_notification_provider();

}
