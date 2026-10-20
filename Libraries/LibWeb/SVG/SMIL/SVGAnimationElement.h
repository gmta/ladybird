/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <LibJS/Forward.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Export.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

class SVGAnimationElement : public SVGElement {
    WEB_WRAPPABLE(SVGAnimationElement, SVGElement);

public:
    GC::Ptr<SVGElement> target_element();

    void set_onbegin(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onbegin();

    void set_onend(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onend();

    void set_onrepeat(GC::Ptr<WebIDL::CallbackType>);
    GC::Ptr<WebIDL::CallbackType> onrepeat();

    WebIDL::ExceptionOr<float> get_start_time();
    float get_current_time();
    WebIDL::ExceptionOr<float> get_simple_duration();

    void begin_element();
    void begin_element_at(float offset);
    void end_element();
    void end_element_at(float offset);

    void element_with_id_changed(Badge<DOM::Document>, Utf16FlyString const& id);

    void update_interval(Badge<SMILTimeContainer>, double time);
    void reset_intervals(Badge<SMILTimeContainer>);
    Optional<double> next_interval_time(double time) const;

protected:
    SVGAnimationElement(DOM::Document&, DOM::QualifiedName);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void inserted() override;
    virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root) override;
    virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor) override;
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;

    virtual Optional<CSS::PropertyID> target_property() const;
    virtual Vector<Animations::BaseKeyframe> keyframes(SVGElement const& target, CSS::PropertyID) const = 0;
    static Animations::BaseKeyframe keyframe(double offset, CSS::PropertyID, CSS::StyleValue const&);
    static void set_keyframe_property(Animations::BaseKeyframe&, CSS::PropertyID, CSS::StyleValue const&);

    void update_animation();

private:
    struct Interval {
        double begin { 0 };
        double end { 0 };

        bool contains(double time) const { return begin <= time && time < end; }
        bool operator==(Interval const&) const = default;
    };

    enum class Restart : u8 {
        Always,
        WhenNotActive,
        Never,
    };

    enum class BeginOrEnd : u8 {
        Begin,
        End,
    };

    enum class InstanceTimeOrigin : u8 {
        Attribute,
        Event,
        Script,
    };

    struct InstanceTime {
        double time { 0 };
        InstanceTimeOrigin origin { InstanceTimeOrigin::Attribute };
    };

    struct EventCondition {
        BeginOrEnd begin_or_end { BeginOrEnd::Begin };
        Optional<Utf16FlyString> event_base_id;
        Utf16FlyString event_type;
        double offset { 0 };
        GC::Ptr<DOM::Element> event_base;
    };

    struct EventBaseListener {
        GC::Ref<DOM::Element> event_base;
        Utf16FlyString event_type;
        GC::Ref<DOM::IDLEventListener> listener;
    };

    GC::Ptr<SMILTimeContainer> time_container();
    Optional<Utf16String> href() const;
    Optional<Utf16String> referenced_element_id() const;
    double simple_duration() const;
    double repeat_count() const;
    Bindings::FillMode fill_mode() const;
    Restart restart() const;

    void parse_begin_and_end_attributes();
    void update_event_base_listeners();
    GC::Ptr<DOM::Element> resolve_event_base(EventCondition const&);
    void handle_event_condition(DOM::Element& event_base, Utf16FlyString const& event_type);
    bool is_active_at(double time) const;
    bool add_instance_time(BeginOrEnd, double time, InstanceTimeOrigin);
    void add_instance_time_for_script(BeginOrEnd, float offset);
    void instance_times_changed();
    Optional<double> resolve_active_end(double begin) const;
    Optional<Interval> resolve_interval(double begin_after, double end_after);
    void discard_or_revalidate_current_interval(double time);
    bool handle_interval_restart(double time);
    double last_interval_end_time() const;
    Optional<Interval> active_interval(double time) const;
    void update_effect_timing(double time);
    void reset_intervals();

    GC::Ptr<SMILAnimation> m_animation;

    Vector<InstanceTime> m_begin_instance_times;
    Vector<InstanceTime> m_end_instance_times;
    bool m_end_allows_open_ended_intervals { true };
    Vector<EventCondition> m_event_conditions;
    Vector<EventBaseListener> m_event_base_listeners;

    Optional<Interval> m_interval;
    Optional<Interval> m_previous_interval;
    Optional<Interval> m_effect_interval;
    bool m_is_waiting_for_first_interval { true };
    bool m_interval_needs_revalidation { false };
};

}
