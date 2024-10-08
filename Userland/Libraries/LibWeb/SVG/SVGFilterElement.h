/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGFilterElement final : public SVGElement {
    WEB_PLATFORM_OBJECT(SVGFilterElement, SVGElement);
    JS_DECLARE_ALLOCATOR(SVGFilterElement);

private:
    SVGFilterElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

};

}
