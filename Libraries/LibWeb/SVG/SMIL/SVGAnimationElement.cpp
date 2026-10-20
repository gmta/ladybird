/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/SMIL/SVGAnimationElement.h>

#include <AK/CharacterTypes.h>
#include <AK/Math.h>
#include <AK/Utf16StringBuilder.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibURL/URL.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/IDLEventListener.h>
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
    parse_begin_and_end_attributes();
}

void SVGAnimationElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation);
    for (auto& condition : m_event_conditions)
        visitor.visit(condition.event_base);
    for (auto& event_base_listener : m_event_base_listeners) {
        visitor.visit(event_base_listener.event_base);
        visitor.visit(event_base_listener.listener);
    }
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
    if (first_is_one_of(name, AttributeNames::attributeName, AttributeNames::dur, AttributeNames::fill, AttributeNames::repeatCount) && !namespace_.has_value()) {
        update_animation();
    } else if (first_is_one_of(name, AttributeNames::begin, AttributeNames::end) && !namespace_.has_value()) {
        parse_begin_and_end_attributes();
        update_event_base_listeners();
        instance_times_changed();
    } else if (name == AttributeNames::href && (!namespace_.has_value() || namespace_ == Namespace::XLink)) {
        update_animation();
    }
}

void SVGAnimationElement::element_with_id_changed(Badge<DOM::Document>, Utf16FlyString const& id)
{
    // Which element has the ID that the href refers to may have changed, and with it the target element. The same goes
    // for the event base elements of event values.
    if (auto referenced_element_id = this->referenced_element_id(); referenced_element_id.has_value() && *referenced_element_id == id)
        update_animation();
    else if (any_of(m_event_conditions, [&](auto const& condition) { return condition.event_base_id == id; }))
        update_event_base_listeners();
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

// https://svgwg.org/specs/animations/#RestartAttribute
SVGAnimationElement::Restart SVGAnimationElement::restart() const
{
    auto restart = get_attribute(AttributeNames::restart);

    // whenNotActive
    //     The animation can only be restarted when it is not active (i.e. after the active end). Attempts to restart
    //     the animation during its active duration are ignored.
    if (restart == "whenNotActive"sv)
        return Restart::WhenNotActive;

    // never
    //     The element cannot be restarted for the remainder of the current simple duration of the parent time
    //     container. (In the case of SVG, since the parent time container is the SVG document fragment, then the
    //     animation cannot be restarted for the remainder of the document duration.)
    if (restart == "never"sv)
        return Restart::Never;

    // always
    //     The animation can be restarted at any time. This is the default value.
    return Restart::Always;
}

// https://svgwg.org/specs/animations/#OffsetValueSyntax
static Optional<double> parse_offset_value_in_milliseconds(Utf16View value)
{
    // offset-value ::= ( S? "+" | "-" S? )? ( Clock-value )
    // NB: White space may precede a plus sign and follow a minus sign, so allow it around either.
    value = value.trim_ascii_whitespace();
    double sign = 1;
    if (value.starts_with('+') || value.starts_with('-')) {
        sign = value.starts_with('-') ? -1 : 1;
        value = value.substring_view(1);
    }
    auto offset = parse_clock_value_in_milliseconds(value);
    if (!offset.has_value())
        return {};
    return sign * *offset;
}

namespace {

struct TimingSpecifier {
    enum class Type : u8 {
        Offset,
        Indefinite,
        Wallclock,
        Event,
        Syncbase,
        Repeat,
        AccessKey,
    };

    Type type { Type::Offset };
    double offset { 0 };
    Optional<Utf16FlyString> element_id;
    Utf16FlyString symbol;
};

}

// https://www.w3.org/TR/2001/REC-smil-animation-20010904/#Timing-ParsingTimeSpecifiers
static Optional<TimingSpecifier> parse_timing_specifier(Utf16View value)
{
    // Several of the timing specification values have a similar syntax. In addition, XML ID attributes are allowed to
    // contain the dot '.' separator character. The backslash character '\' can be used to escape the dot separator
    // within identifier and event-name references. To parse an individual item in a value-list, the following
    // approach defines the correct interpretation.
    auto find_unescaped = [](Utf16View string, auto predicate) -> Optional<size_t> {
        for (size_t index = 0; index < string.length_in_code_units(); ++index) {
            auto code_unit = string.code_unit_at(index);
            if (code_unit == '\\')
                ++index;
            else if (predicate(code_unit))
                return index;
        }
        return {};
    };
    auto remove_escape_characters = [](Utf16View string) {
        Utf16StringBuilder builder;
        for (size_t index = 0; index < string.length_in_code_units(); ++index) {
            if (string.code_unit_at(index) == '\\' && index + 1 < string.length_in_code_units())
                ++index;
            builder.append_code_unit(string.code_unit_at(index));
        }
        return Utf16FlyString { builder.to_string() };
    };
    auto is_sign_indicator = [](char16_t code_unit) { return code_unit == '+' || code_unit == '-'; };

    // 1. Strip any leading, trailing, or intervening white space characters.
    // NB: Like other engines, keep intervening white space, which neither a clock value nor the reference to an
    //     event allows. The white space around the sign of an offset is ignored when the offset is parsed.
    value = value.trim_ascii_whitespace();

    // 2. If the value begins with a number or numeric sign indicator (i.e. '+' or '-'), the value should be parsed as
    //    an offset value.
    if (!value.is_empty() && (is_ascii_digit(value.code_unit_at(0)) || is_sign_indicator(value.code_unit_at(0)))) {
        auto offset = parse_offset_value_in_milliseconds(value);
        if (!offset.has_value())
            return {};
        return TimingSpecifier { .type = TimingSpecifier::Type::Offset, .offset = *offset, .element_id = {}, .symbol = {} };
    }

    // 3. Else if the value begins with the token "wallclock", it should be parsed as a wallclock-sync-value.
    if (value.starts_with("wallclock"sv))
        return TimingSpecifier { .type = TimingSpecifier::Type::Wallclock, .offset = 0, .element_id = {}, .symbol = {} };

    // 4. Else if the value is the token "indefinite", it should be parsed as the value "indefinite".
    if (value == "indefinite"sv)
        return TimingSpecifier { .type = TimingSpecifier::Type::Indefinite, .offset = 0, .element_id = {}, .symbol = {} };

    // 5. Else: Build a token substring up to but not including any sign indicator (i.e. strip off any offset). In the
    //    following, ignore any '.' separator characters preceded by a backslash '\' escape character. In addition,
    //    strip any leading backslash '\' escape character.
    // NB: Like Gecko, also ignore a sign indicator preceded by a backslash, so that an ID can contain one.
    TimingSpecifier specifier;
    auto sign_index = find_unescaped(value, is_sign_indicator);
    if (sign_index.has_value()) {
        auto offset = parse_offset_value_in_milliseconds(value.substring_view(*sign_index));
        if (!offset.has_value())
            return {};
        specifier.offset = *offset;
    }
    auto token = value.substring_view(0, sign_index.value_or(value.length_in_code_units())).trim_ascii_whitespace(TrimMode::Right);
    if (token.starts_with('\\'))
        token = token.substring_view(1);

    //    - If the token contains no '.' separator character, then the value should be parsed as an event-value with an
    //      unspecified (i.e. default) eventbase-element.
    // NB: So are a repeat-value without an Id-value and an accessKey-value.
    auto separator_index = find_unescaped(token, [](char16_t code_unit) { return code_unit == '.'; });
    if (!separator_index.has_value()) {
        specifier.type = TimingSpecifier::Type::Event;
        if (token.starts_with("repeat("sv))
            specifier.type = TimingSpecifier::Type::Repeat;
        else if (token.starts_with("accessKey("sv))
            specifier.type = TimingSpecifier::Type::AccessKey;
        specifier.symbol = remove_escape_characters(token);
    } else {
        auto element_id = token.substring_view(0, *separator_index);
        auto symbol = token.substring_view(*separator_index + 1);
        specifier.element_id = remove_escape_characters(element_id);

        //    - Else if the token ends with the string ".begin" or ".end", then the value should be parsed as a
        //      syncbase-value.
        if (first_is_one_of(symbol, "begin"sv, "end"sv)) {
            specifier.type = TimingSpecifier::Type::Syncbase;
            specifier.symbol = Utf16FlyString::from_utf16(symbol);
        }
        //    - Else, the value should be parsed as an event-value (with a specified eventbase-element). Before parsing
        //      the event value, any backslash '\' escape character after the '.' separator character should be
        //      removed.
        // NB: So is a repeat-value with an Id-value.
        else {
            specifier.type = symbol.starts_with("repeat("sv) ? TimingSpecifier::Type::Repeat : TimingSpecifier::Type::Event;
            specifier.symbol = remove_escape_characters(symbol);
        }
    }

    // NB: An event-value refers to an element and an event by name, so neither can be empty.
    if (specifier.symbol.is_empty() || (specifier.element_id.has_value() && specifier.element_id->is_empty()))
        return {};
    return specifier;
}

// https://svgwg.org/specs/animations/#BeginAttribute
// https://svgwg.org/specs/animations/#EndAttribute
void SVGAnimationElement::parse_begin_and_end_attributes()
{
    // NB: Instance times that events and DOM method calls added stay, as they do not come from the attributes.
    m_begin_instance_times.remove_all_matching([](auto const& instance_time) { return instance_time.origin == InstanceTimeOrigin::Attribute; });
    m_end_instance_times.remove_all_matching([](auto const& instance_time) { return instance_time.origin == InstanceTimeOrigin::Attribute; });
    m_event_conditions.clear();

    // NB: The instance times of an indefinite value are infinite, which sort after every other instance time.
    auto parse_value_list = [&](Optional<Utf16String> const& attribute, BeginOrEnd begin_or_end, Vector<InstanceTime>& instance_times) {
        bool has_event_conditions = false;
        if (!attribute.has_value())
            return has_event_conditions;
        for (auto value : attribute->utf16_view().split_view(';', SplitBehavior::KeepEmpty)) {
            auto specifier = parse_timing_specifier(value);
            if (!specifier.has_value())
                continue;
            switch (specifier->type) {
            case TimingSpecifier::Type::Offset:
                instance_times.append({ specifier->offset, InstanceTimeOrigin::Attribute });
                break;
            case TimingSpecifier::Type::Indefinite:
                instance_times.append({ AK::Infinity<double>, InstanceTimeOrigin::Attribute });
                break;
            case TimingSpecifier::Type::Event:
                m_event_conditions.append({
                    .begin_or_end = begin_or_end,
                    .event_base_id = move(specifier->element_id),
                    .event_type = move(specifier->symbol),
                    .offset = specifier->offset,
                    .event_base = {},
                });
                has_event_conditions = true;
                break;
            case TimingSpecifier::Type::Repeat:
            case TimingSpecifier::Type::AccessKey:
                // FIXME: Support repeat and accessKey values.
                has_event_conditions = true;
                break;
            case TimingSpecifier::Type::Syncbase:
            case TimingSpecifier::Type::Wallclock:
                // FIXME: Support syncbase and wallclock values.
                break;
            }
        }
        return has_event_conditions;
    };

    // begin-value-list ::= begin-value (S? ";" S? begin-value-list )?
    //     A semicolon separated list of begin values. The interpretation of a list of begin times is detailed in SMIL
    //     Animation's section on "Evaluation of begin and end time lists".
    // NB: The initial value of begin is 0s.
    auto begin = get_attribute(AttributeNames::begin);
    if (begin.has_value())
        parse_value_list(begin, BeginOrEnd::Begin, m_begin_instance_times);
    else
        m_begin_instance_times.append({ 0, InstanceTimeOrigin::Attribute });

    // end-value-list ::= end-value (S? ";" S? end-value-list )?
    //     A semicolon separated list of end values. The interpretation of a list of end times is detailed below.
    // NB: An interval may be left without an end when there is no end attribute, or when one of its values may yield
    //     an instance time later on.
    auto end = get_attribute(AttributeNames::end);
    auto end_has_event_conditions = parse_value_list(end, BeginOrEnd::End, m_end_instance_times);
    m_end_allows_open_ended_intervals = !end.has_value() || end_has_event_conditions;

    auto by_time = [](auto const& a, auto const& b) { return a.time < b.time; };
    insertion_sort(m_begin_instance_times, by_time);
    insertion_sort(m_end_instance_times, by_time);
}

// Listens for the events of the event values, at their event base elements.
void SVGAnimationElement::update_event_base_listeners()
{
    for (auto& event_base_listener : m_event_base_listeners)
        event_base_listener.event_base->remove_event_listener_without_options(event_base_listener.event_type, event_base_listener.listener);
    m_event_base_listeners.clear();
    for (auto& condition : m_event_conditions)
        condition.event_base = nullptr;
    if (!is_connected())
        return;

    for (auto& condition : m_event_conditions) {
        condition.event_base = resolve_event_base(condition);
        if (!condition.event_base)
            continue;

        // One listener per event at an event base handles all the event values for it, since an event can begin or
        // end an animation element but not both.
        if (any_of(m_event_base_listeners, [&](auto const& listener) { return listener.event_base == condition.event_base && listener.event_type == condition.event_type; }))
            continue;
        auto& realm = HTML::relevant_realm(*this);
        auto function = JS::NativeFunction::create(
            realm, [element = GC::Weak<SVGAnimationElement> { *this }, event_base = GC::Weak<DOM::Element> { *condition.event_base }, event_type = condition.event_type](JS::VM&) {
                if (element && event_base)
                    element->handle_event_condition(*event_base, event_type);
                return JS::js_undefined();
            },
            1, Utf16FlyString {}, &realm);
        auto listener = DOM::IDLEventListener::create(GC::Heap::the().allocate<WebIDL::CallbackType>(*function, realm));
        condition.event_base->add_event_listener_without_options(condition.event_type, listener);
        m_event_base_listeners.append({ *condition.event_base, condition.event_type, listener });
    }
}

// https://www.w3.org/TR/2001/REC-smil-animation-20010904/#Timing-EventValueSyntax
GC::Ptr<DOM::Element> SVGAnimationElement::resolve_event_base(EventCondition const& condition)
{
    // If the Eventbase-element term is missing, the event-base element is defined to be the target element of the
    // animation,
    if (!condition.event_base_id.has_value())
        return target_element();

    // The eventbase-element must be another element contained in the host document.
    // NB: Like other engines, only take an element in the same tree as this one, as with the href.
    auto* root = as_if<DOM::ParentNode>(this->root());
    if (!root)
        return {};
    return root->get_element_by_id(*condition.event_base_id);
}

// https://www.w3.org/TR/2001/REC-smil-animation-20010904/#Timing-EventSensitivity
void SVGAnimationElement::handle_event_condition(DOM::Element& event_base, Utf16FlyString const& event_type)
{
    auto time_container = this->time_container();
    if (!time_container)
        return;
    auto time = time_container->time_of_event_in_milliseconds();

    // The semantics of element sensitivity to events are described by the following set of rules:
    // - If an element is not active, then events are only handled for begin specifications. Thus if an event is raised
    //   and begin specifies the event, the element begins. While the element is not active, any end specification of
    //   the event is ignored.
    // - If an element is (already) active when an event is raised, and begin specifies the event, then the behavior
    //   depends upon the value of restart:
    //   - If restart="always", then a new begin time is resolved for the element based on the event time. Any
    //     specification of the event in end is ignored for this event instance.
    //   - If restart="never" or restart="whenNotActive", then any begin specification of the event is ignored for
    //     this instance of the event. If end specifies the event, an end value is resolved based upon the event time,
    //     and the active duration is re-evaluated (according to the rules in Computing the active duration).
    // It is important to notice that in no case is a single event occurrence used to resolve both a begin and end time
    // on the same element.
    // NB: add_instance_time() ignores the instance times that the rules ignore.
    auto handles_event = [&](EventCondition const& condition, BeginOrEnd begin_or_end) {
        return condition.begin_or_end == begin_or_end && condition.event_base.ptr() == &event_base && condition.event_type == event_type;
    };
    bool began = false;
    for (auto const& condition : m_event_conditions) {
        if (handles_event(condition, BeginOrEnd::Begin) && add_instance_time(BeginOrEnd::Begin, time + condition.offset, InstanceTimeOrigin::Event))
            began = true;
    }
    bool ended = false;
    if (!began) {
        for (auto const& condition : m_event_conditions) {
            if (handles_event(condition, BeginOrEnd::End) && add_instance_time(BeginOrEnd::End, time + condition.offset, InstanceTimeOrigin::Event))
                ended = true;
        }
    }
    if (began || ended)
        instance_times_changed();
}

bool SVGAnimationElement::is_active_at(double time) const
{
    return m_interval.has_value() && m_interval->contains(time);
}

// Adds an instance time for an event or a DOM method call, unless the element is not sensitive to it.
bool SVGAnimationElement::add_instance_time(BeginOrEnd begin_or_end, double time, InstanceTimeOrigin origin)
{
    auto time_container = this->time_container();
    if (!time_container)
        return false;
    auto is_active = is_active_at(time_container->time_of_event_in_milliseconds());
    if (begin_or_end == BeginOrEnd::Begin && is_active && restart() != Restart::Always)
        return false;
    if (begin_or_end == BeginOrEnd::End && !is_active)
        return false;

    auto& instance_times = begin_or_end == BeginOrEnd::Begin ? m_begin_instance_times : m_end_instance_times;
    auto index = instance_times.find_first_index_if([&](auto const& instance_time) { return instance_time.time > time; }).value_or(instance_times.size());
    instance_times.insert(index, { time, origin });
    return true;
}

void SVGAnimationElement::instance_times_changed()
{
    m_interval_needs_revalidation = true;
    if (m_animation)
        m_animation->time_container().update_intervals();
}

// Resolves the end of an interval that begins at the given time, which is unresolved if there is no such interval.
Optional<double> SVGAnimationElement::resolve_active_end(double begin) const
{
    // The end of an interval is the first end instance time after its begin. Without end instance times, the active
    // duration is not constrained by an end.
    auto end = AK::Infinity<double>;
    if (!m_end_instance_times.is_empty()) {
        auto next_end = m_end_instance_times.first_matching([&](auto const& instance_time) { return instance_time.time > begin; });
        if (next_end.has_value())
            end = next_end->time;
        else if (!m_end_allows_open_ended_intervals)
            return {};
    }

    // The end can constrain but not extend the active duration that is otherwise defined by the simple duration and
    // the repeat behavior.
    // FIXME: Support repeatDur, min and max.
    return min(begin + simple_duration() * repeat_count(), end);
}

// Resolves the first interval that begins at or after begin_after and ends after end_after, if any.
Optional<SVGAnimationElement::Interval> SVGAnimationElement::resolve_interval(double begin_after, double end_after)
{
    auto const is_first_interval = m_is_waiting_for_first_interval;
    size_t search_start = 0;
    while (true) {
        auto begin_index = search_start;
        while (begin_index < m_begin_instance_times.size() && m_begin_instance_times[begin_index].time < begin_after)
            ++begin_index;

        // No interval begins without a further begin instance time. An indefinite one only yields an instance time
        // once beginElement() is called.
        if (begin_index == m_begin_instance_times.size() || isinf(m_begin_instance_times[begin_index].time))
            return {};
        auto begin = m_begin_instance_times[begin_index].time;
        auto end = resolve_active_end(begin);
        if (!end.has_value())
            return {};
        if (*end > end_after)
            return Interval { begin, *end };

        // An interval that ended in the past is still the current interval while waiting for the first one, so that it
        // can be frozen. One that ended before the timeline began is skipped.
        if (is_first_interval && *end > 0) {
            m_interval = Interval { begin, *end };
            m_is_waiting_for_first_interval = false;
        }
        search_start = begin_index + 1;
        begin_after = *end;
    }
}

// Instance times that changed may move the end of the current interval, or replace an interval that has not begun.
void SVGAnimationElement::discard_or_revalidate_current_interval(double time)
{
    if (!m_interval.has_value())
        return;
    if (time < m_interval->begin) {
        m_interval.clear();
        return;
    }
    if (time < m_interval->end) {
        auto end = resolve_active_end(m_interval->begin);
        if (!end.has_value()) {
            m_interval.clear();
            if (!m_previous_interval.has_value())
                m_is_waiting_for_first_interval = true;
            return;
        }
        m_interval->end = *end;
    }
}

// Whether the next interval should be resolved, after restarting the current one if it should be.
bool SVGAnimationElement::handle_interval_restart(double time)
{
    auto restart = this->restart();
    if (!m_is_waiting_for_first_interval && restart == Restart::Never)
        return false;
    if (!m_interval.has_value() || m_interval->end <= time)
        return true;

    // A begin instance time within the current interval ends it, so that the next one begins at that time.
    if (restart == Restart::Always) {
        auto next_begin = m_begin_instance_times.first_matching([&](auto const& instance_time) { return instance_time.time > m_interval->begin; });
        if (next_begin.has_value() && m_interval->end > next_begin->time) {
            m_interval->end = next_begin->time;
            return m_interval->end <= time;
        }
    }
    return false;
}

double SVGAnimationElement::last_interval_end_time() const
{
    if (!m_is_waiting_for_first_interval) {
        if (m_interval.has_value())
            return m_interval->end;
        if (m_previous_interval.has_value())
            return m_previous_interval->end;
    }
    return -AK::Infinity<double>;
}

// The interval that the animation plays at the given time: the previous one keeps playing until the current one begins,
// so that it stays frozen until then.
Optional<SVGAnimationElement::Interval> SVGAnimationElement::active_interval(double time) const
{
    if (!m_interval.has_value() || (m_previous_interval.has_value() && time < m_interval->begin))
        return m_previous_interval;
    return m_interval;
}

void SVGAnimationElement::update_interval(Badge<SMILTimeContainer>, double time)
{
    if (exchange(m_interval_needs_revalidation, false))
        discard_or_revalidate_current_interval(time);

    if (handle_interval_restart(time)) {
        auto interval = resolve_interval(last_interval_end_time(), time);
        if (interval != m_interval) {
            if (m_interval.has_value())
                m_previous_interval = m_interval;
            m_interval = interval;
        }
    }
    if (m_interval.has_value() && m_interval->contains(time))
        m_is_waiting_for_first_interval = false;

    update_effect_timing(time);
}

void SVGAnimationElement::reset_intervals(Badge<SMILTimeContainer>)
{
    reset_intervals();
}

void SVGAnimationElement::reset_intervals()
{
    m_interval.clear();
    m_previous_interval.clear();
    m_is_waiting_for_first_interval = true;
}

// The next time after the given one at which the interval to play may change.
Optional<double> SVGAnimationElement::next_interval_time(double time) const
{
    Optional<double> next_time;
    if (m_interval.has_value()) {
        if (time < m_interval->begin)
            next_time = m_interval->begin;
        else if (time < m_interval->end && isfinite(m_interval->end))
            next_time = m_interval->end;
    }
    if (auto next_begin = m_begin_instance_times.first_matching([&](auto const& instance_time) { return instance_time.time > time; }); next_begin.has_value() && isfinite(next_begin->time))
        next_time = min(next_time.value_or(next_begin->time), next_begin->time);
    return next_time;
}

// Times the effect of the animation to play the interval that is active at the given time.
void SVGAnimationElement::update_effect_timing(double time)
{
    auto interval = active_interval(time);
    if (!m_animation || interval == m_effect_interval)
        return;
    m_effect_interval = interval;

    // Without an interval, the effect has no active duration and leaves its target alone.
    Bindings::OptionalEffectTiming timing {
        .delay = 0,
        .duration = 0.,
        .easing = "linear"_utf16,
        .fill = Bindings::FillMode::None,
        .iterations = 0,
    };
    if (interval.has_value()) {
        auto active_duration = interval->end - interval->begin;
        auto simple_duration = this->simple_duration();
        timing.delay = interval->begin;
        timing.fill = fill_mode();
        if (isinf(simple_duration)) {
            // There is nothing to interpolate over without a simple duration, so the animation function keeps its value
            // at the start of the simple duration throughout the active duration.
            timing.duration = active_duration;
            timing.iterations = 1;
            timing.easing = "linear(0, 0)"_utf16;
        } else {
            // An end that constrains the active duration cuts the last iteration short.
            auto repeat_count = this->repeat_count();
            timing.duration = simple_duration;
            timing.iterations = active_duration < simple_duration * repeat_count ? active_duration / simple_duration : repeat_count;
        }
    }
    MUST(m_animation->effect()->update_timing(timing));
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
    // to, so the document lets us know when elements with that ID come and go. The same goes for the event base elements
    // of event values.
    auto refers_to_ids = href().has_value() || any_of(m_event_conditions, [](auto const& condition) { return condition.event_base_id.has_value(); });
    if (is_connected() && refers_to_ids)
        document().register_svg_animation_element_with_id_references({}, *this);
    else
        document().unregister_svg_animation_element_with_id_references({}, *this);
    update_event_base_listeners();

    GC::Ptr<SMILTimeContainer> previous_time_container;
    if (m_animation) {
        previous_time_container = m_animation->time_container();
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

    // FIXME: A running animation requests a rendering update on every frame, even when its output cannot change, as
    //        with an indefinite one. This keeps the rendering loop running for as long as such an animation is active.
    auto effect = MUST(Animations::KeyframeEffect::create_from_processed_keyframes(target, move(keyframes), 0.));
    effect->set_iteration_count(0);
    effect->set_fill_mode(Bindings::FillMode::None);
    m_effect_interval.clear();

    // The intervals of an animation element are times on the timeline of its time container, so they are resolved
    // again on another one. On the same one, the timing attributes that changed may have moved the end of the current
    // interval.
    if (time_container != previous_time_container)
        reset_intervals();
    else
        m_interval_needs_revalidation = true;

    m_animation = SMILAnimation::create(*this, *time_container);
    m_animation->set_timeline(document().timeline());
    m_animation->set_effect(effect);
    time_container->add_animation(*m_animation);
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
    // NB: Only an animation element that animates a target element has intervals.
    if (!m_animation || !m_interval.has_value())
        return WebIDL::InvalidStateError::create(HTML::relevant_realm(*this), "The animation element has no current interval"_utf16);
    return static_cast<float>(m_interval->begin / 1000.);
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

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__beginElement
void SVGAnimationElement::begin_element()
{
    // Creates a begin instance time for the current time. The new instance time is added to the begin instance times
    // list. The behavior of this method is equivalent to beginElementAt(0).
    begin_element_at(0);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__beginElementAt
void SVGAnimationElement::begin_element_at(float offset)
{
    // Creates a begin instance time for the current time plus the specified offset. The new instance time is added to
    // the begin instance times list.
    add_instance_time_for_script(BeginOrEnd::Begin, offset);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__endElement
void SVGAnimationElement::end_element()
{
    // Creates an end instance time for the current time. The new instance time is added to the end instance times
    // list. The behavior of this method is equivalent to endElementAt(0).
    end_element_at(0);
}

// https://svgwg.org/specs/animations/#__svg__SVGAnimationElement__endElementAt
void SVGAnimationElement::end_element_at(float offset)
{
    // Creates a end instance time for the current time plus the specified offset. The new instance time is added to
    // the end instance times list.
    add_instance_time_for_script(BeginOrEnd::End, offset);
}

// https://svgwg.org/specs/animations/#InterfaceSVGAnimationElement
void SVGAnimationElement::add_instance_time_for_script(BeginOrEnd begin_or_end, float offset)
{
    // The instance time associated with a beginElementAt() or endElementAt() call is the current presentation time at
    // the time of the DOM method call, plus or minus the specified offset.
    // Note that beginElement() is subject to the ‘restart’ attribute in the same manner that event-based begin timing
    // is.
    // NB: Like other engines, an element that is not active also ignores endElement() as it does end events.
    auto time_container = this->time_container();
    if (!time_container || !is_connected())
        return;
    if (add_instance_time(begin_or_end, time_container->time_of_event_in_milliseconds() + offset * 1000., InstanceTimeOrigin::Script))
        instance_times_changed();
}

}
