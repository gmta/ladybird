/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/PermissionsAPI/PermissionNames.h>

namespace Web::PermissionsAPI::PermissionNames {

#define __ENUMERATE_PERMISSION_NAME(name, permission) \
    Utf16FlyString const& name = *new Utf16FlyString(permission##_utf16_fly_string);
ENUMERATE_PERMISSION_NAMES
#undef __ENUMERATE_PERMISSION_NAME

}
