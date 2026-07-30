/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/Export.h>

namespace Web::PermissionsAPI::PermissionNames {

// https://w3c.github.io/permissions/#permission-registry
#define ENUMERATE_PERMISSION_NAMES                          \
    __ENUMERATE_PERMISSION_NAME(geolocation, "geolocation") \
    __ENUMERATE_PERMISSION_NAME(notifications, "notifications")

#define __ENUMERATE_PERMISSION_NAME(name, permission) extern WEB_API Utf16FlyString const& name;
ENUMERATE_PERMISSION_NAMES
#undef __ENUMERATE_PERMISSION_NAME

}
