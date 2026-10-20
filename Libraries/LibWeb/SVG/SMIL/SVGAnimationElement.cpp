/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SMIL/SVGAnimationElement.h>

#include <AK/Math.h>
#include <LibURL/URL.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/FragmentIdentifier.h>
#include <LibWeb/SVG/SMIL/SMILAnimation.h>
#include <LibWeb/SVG/SMIL/SMILTimeContainer.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::SVG {

SVGAnimationElement::SVGAnimationElement(DOM::Document& document, DOM::QualifiedName name)
    : SVGElement(document, name)
{
}

void SVGAnimationElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
}

void SVGAnimationElement::inserted()
{
    Base::inserted();
    update_animation();
}

void SVGAnimationElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);
    update_animation();
}

void SVGAnimationElement::moved_from(IsSubtreeRoot is_subtree_root, GC::Ptr<Node> old_ancestor)
{
    Base::moved_from(is_subtree_root, old_ancestor);
    update_animation();
}

void SVGAnimationElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);
    if (first_is_one_of(name, AttributeNames::attributeName, AttributeNames::dur, AttributeNames::fill, AttributeNames::repeatCount) && !namespace_.has_value())
        update_animation();
    else if (name == AttributeNames::href && (!namespace_.has_value() || namespace_ == Namespace::XLink))
        update_animation();
}

void SVGAnimationElement::element_with_id_changed(Badge<DOM::Document>, Utf16FlyString const& id)
{
    // Which element has the ID that the href refers to may have changed, and with it the target element.
    if (auto referenced_element_id = this->referenced_element_id(); referenced_element_id.has_value() && *referenced_element_id == id)
        update_animation();
}

GC::Ptr<SMILTimeContainer> SVGAnimationElement::time_container()
{
    auto outermost_svg_element = this->outermost_svg_element();
    return outermost_svg_element ? outermost_svg_element->time_container() : nullptr;
}

// https://svgwg.org/svg2-draft/linking.html#XLinkRefAttrs
Optional<Utf16String> SVGAnimationElement::href() const
{
    // For backwards compatibility, elements with an ‘href’ attribute also recognize an ‘href’ attribute in the XLink
    // namespace [xlink]. When the ‘href’ attribute is present in both the XLink namespace and without a namespace, the
    // value of the attribute without a namespace shall be used. The attribute in the XLink namespace shall be ignored.
    if (auto href = get_attribute_ns(OptionalNone {}, AttributeNames::href); href.has_value())
        return href;
    return get_attribute_ns(Namespace::XLink, AttributeNames::href);
}

Optional<Utf16String> SVGAnimationElement::referenced_element_id() const
{
    auto href = this->href();
    if (!href.has_value())
        return {};

    // Only an element in this document can be the target element, so a URL that refers to another document refers to
    // no element.
    auto url = document().encoding_parse_url(*href);
    if (!url.has_value() || !url->fragment().has_value() || !url->equals(document().base_url(), URL::ExcludeFragment::Yes))
        return {};
    return decode_fragment_identifier(*url->fragment());
}

// https://svgwg.org/specs/animations/#HrefAttribute
GC::Ptr<SVGElement> SVGAnimationElement::target_element()
{
    // A URL reference to the element which is the target of this animation element and which therefore will be
    // modified over time. The URL must point to exactly one target element which is capable of being the target of the
    // given animation element. If the URL points to multiple target elements, if the given target element is not
    // capable of being a target of the given animation element, or if the given target element is not part of the
    // current document, then the animation element will not affect any target element.
    // NB: Only an SVG element in the same tree as this element can be a target.
    if (href().has_value()) {
        auto referenced_element_id = this->referenced_element_id();
        if (!referenced_element_id.has_value())
            return {};
        auto* root = as_if<DOM::ParentNode>(this->root());
        if (!root)
            return {};
        return as_if<SVGElement>(root->get_element_by_id(*referenced_element_id).ptr());
    }

    // If the ‘href’ attribute or the deprecated ‘xlink:href’ attribute is not provided, then the target element will
    // be the immediate parent element of the current animation element.
    return as_if<SVGElement>(parent_element().ptr());
}

// https://svgwg.org/specs/animations/#AttributeNameAttribute
Optional<CSS::PropertyID> SVGAnimationElement::target_property() const
{
    auto attribute_name = get_attribute(AttributeNames::attributeName);
    if (!attribute_name.has_value())
        return {};

    // That is, when determining if ‘attributeName’ corresponds to an attribute name or a CSS property name, the
    // implementation must first search through the list of CSS properties for a matching property name.
    // NB: Neither custom properties nor properties that cannot be animated have an animation.
    auto property_id = CSS::property_id_from_string(*attribute_name);
    if (!property_id.has_value() || property_id == CSS::PropertyID::Custom || !CSS::is_animatable_property(*property_id))
        return {};

    // FIXME: If no matching property is found, the implementation must search for a matching attribute on the target
    //        element.
    return property_id;
}

// https://svgwg.org/specs/animations/#DurAttribute
double SVGAnimationElement::simple_duration() const
{
    // If the animation does not have a ‘dur’ attribute, the simple duration is indefinite.
    auto dur = get_attribute(AttributeNames::dur);
    if (!dur.has_value())
        return AK::Infinity<double>;

    // Clock-value
    //     Specifies the length of the simple duration in presentation time. Value must be greater than 0.
    if (auto duration = parse_clock_value_in_milliseconds(*dur); duration.has_value() && *duration > 0)
        return *duration;

    // NB: An "indefinite" value specifies an indefinite simple duration, SVG ignores a "media" value, and SMIL
    //     Animation ignores a value with any error in its syntax. Each leaves the simple duration indefinite.
    return AK::Infinity<double>;
}

// https://svgwg.org/specs/animations/#RepeatCountAttribute
double SVGAnimationElement::repeat_count() const
{
    // Specifies the number of iterations of the animation function. It can have the following attribute values:
    auto repeat_count = get_attribute(AttributeNames::repeatCount);
    if (!repeat_count.has_value())
        return 1;

    // <number>
    //     This is a (base 10) "floating point" numeric value that specifies the number of iterations. It can include
    //     partial iterations expressed as fraction values. A fractional value describes a portion of the simple
    //     duration. Values must be greater than 0.
    if (auto count = parse_number_percentage(*repeat_count); count.has_value() && !count->is_percentage()
        && count->value() > 0 && isfinite(count->value())) {
        return count->value();
    }

    // indefinite
    //     The animation is defined to repeat indefinitely (i.e. until the document ends).
    if (*repeat_count == "indefinite"sv)
        return AK::Infinity<double>;

    // NB: SMIL Animation ignores a value with any error in its syntax, which leaves a single iteration.
    return 1;
}

// https://svgwg.org/specs/animations/#FillAttribute
Bindings::FillMode SVGAnimationElement::fill_mode() const
{
    // freeze
    //     The animation effect F(t) is defined to freeze the effect value at the last value of the active duration.
    if (auto fill = get_attribute(AttributeNames::fill); fill.has_value() && *fill == "freeze"sv)
        return Bindings::FillMode::Forwards;

    // remove
    //     The animation effect is removed (no longer applied) when the active duration of the animation is over.
    return Bindings::FillMode::None;
}

Animations::BaseKeyframe SVGAnimationElement::keyframe(double offset, CSS::PropertyID property_id, CSS::StyleValue const& value)
{
    Animations::BaseKeyframe keyframe;
    keyframe.offset = offset;
    keyframe.properties = Animations::BaseKeyframe::ParsedProperties {};
    set_keyframe_property(keyframe, property_id, value);
    return keyframe;
}

void SVGAnimationElement::set_keyframe_property(Animations::BaseKeyframe& keyframe, CSS::PropertyID property_id, CSS::StyleValue const& value)
{
    keyframe.parsed_properties().set(CSS::PropertyNameAndID::from_id(property_id), CSS::RustStyleValueHandle::retained(value.rust_style_value_data()));
}

void SVGAnimationElement::update_animation()
{
    // The target element of an animation element with an href changes along with the element that has the ID it refers
    // to, so the document lets us know when elements with that ID come and go.
    if (is_connected() && href().has_value())
        document().register_svg_animation_element_with_id_references({}, *this);
    else
        document().unregister_svg_animation_element_with_id_references({}, *this);

    if (m_animation) {
        m_animation->cancel();
        m_animation = nullptr;
    }

    if (!is_connected())
        return;
    auto time_container = this->time_container();
    if (!time_container)
        return;
    auto target = target_element();
    if (!target)
        return;
    auto property_id = target_property();
    if (!property_id.has_value())
        return;
    auto keyframes = this->keyframes(*target, *property_id);
    if (keyframes.is_empty())
        return;

    // FIXME: Resolve the interval to play from the begin and end attributes, and the other timing attributes.
    // FIXME: A running animation requests a rendering update on every frame, even when its output cannot change, as
    //        with an indefinite one. This keeps the rendering loop running for as long as such an animation is active.
    auto effect = MUST(Animations::KeyframeEffect::create_from_processed_keyframes(target, move(keyframes), simple_duration()));
    effect->set_iteration_count(repeat_count());
    effect->set_fill_mode(fill_mode());

    auto animation = SMILAnimation::create(*this, *time_container);
    animation->set_timeline(document().timeline());
    animation->set_effect(effect);
    time_container->add_animation(animation);
    m_animation = animation;
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
void SVGAnimationElement::set_onbegin(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::beginEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onbegin
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onbegin()
{
    return event_handler_attribute(HTML::EventNames::beginEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
void SVGAnimationElement::set_onend(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::endEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onend
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onend()
{
    return event_handler_attribute(HTML::EventNames::endEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
void SVGAnimationElement::set_onrepeat(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::repeatEvent, value);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__onrepeat
GC::Ptr<WebIDL::CallbackType> SVGAnimationElement::onrepeat()
{
    return event_handler_attribute(HTML::EventNames::repeatEvent);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getStartTime
WebIDL::ExceptionOr<float> SVGAnimationElement::get_start_time()
{
    // Returns the begin time, in seconds, for this animation element's current interval, if it exists, regardless of
    // whether the interval has begun yet. If there is no current interval, then a DOMException with code
    // INVALID_STATE_ERR is thrown.
    // NB: An animation element in an SVG document fragment has one interval, which begins when the timeline of the
    //     fragment begins.
    // FIXME: Resolve the begin times of intervals from the begin attribute.
    if (!is_connected() || !time_container())
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(*this), "The animation element has no current interval"_utf16);
    return 0;
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getCurrentTime
float SVGAnimationElement::get_current_time()
{
    // Returns the current time in seconds relative to time zero for the given time container.
    // NB: Like SVGSVGElement.getCurrentTime(), return 0 until the timeline has begun, and for an animation element
    //     outside of an SVG document fragment.
    auto time_container = is_connected() ? this->time_container() : nullptr;
    if (!time_container || !time_container->has_begun())
        return 0;
    return static_cast<float>(time_container->current_time_in_milliseconds() / 1000.);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__getSimpleDuration
WebIDL::ExceptionOr<float> SVGAnimationElement::get_simple_duration()
{
    // Returns the number of seconds for the simple duration for this animation. If the simple duration is undefined
    // (e.g., the end time is indefinite), then an exception is raised.
    // NB: The exception is a DOMException with code NOT_SUPPORTED_ERR.
    auto duration = simple_duration();
    if (isinf(duration))
        return WebIDL::NotSupportedError::create(HTML::relevant_realm(*this), "The simple duration is indefinite"_utf16);
    return static_cast<float>(duration / 1000.);
}

}
