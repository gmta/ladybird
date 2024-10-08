/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG2/types.html#InterfaceSVGAnimatedEnumeration
class SVGAnimatedEnumeration : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGAnimatedEnumeration, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(SVGAnimatedEnumeration);

private:
    u16 m_base_val { 0 };
    u16 m_anim_val { 0 };
};

}
