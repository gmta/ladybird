/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::SVG {

SVGFilterElement::SVGFilterElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGFilterElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFilterElement);
}

}
