/*
 * Copyright (c) 2025, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Time.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/Notification.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/NotificationsAPI/Notification.h>
#include <LibWeb/NotificationsAPI/NotificationList.h>
#include <LibWeb/NotificationsAPI/PlatformNotification.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/PermissionNames.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::NotificationsAPI {

GC_DEFINE_ALLOCATOR(Notification);

static u64 s_next_notification_id = 0;

// https://notifications.spec.whatwg.org/#get-the-notifications-permission-state
Bindings::NotificationPermission get_the_notifications_permission_state()
{
    // 1. Let permissionState be the result of getting the current permission state with "notifications".
    auto permission_state = PermissionsAPI::get_current_permission_state(PermissionsAPI::PermissionNames::notifications);

    // 2. If permissionState is "prompt", then return "default".
    if (permission_state == Bindings::PermissionState::Prompt)
        return Bindings::NotificationPermission::Default;

    // 3. Return permissionState.
    switch (permission_state) {
    case Bindings::PermissionState::Granted:
        return Bindings::NotificationPermission::Granted;
    case Bindings::PermissionState::Denied:
        return Bindings::NotificationPermission::Denied;
    default:
        VERIFY_NOT_REACHED();
    }
}

Notification::Notification(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_id(s_next_notification_id++)
{
}

Notification::~Notification() = default;

// https://notifications.spec.whatwg.org/#create-a-notification
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification(
    JS::Realm& realm,
    Utf16String const& title,
    Bindings::NotificationOptions const& options,
    URL::Origin origin,
    URL::URL base_url,
    HighResolutionTime::EpochTimeStamp fallback_timestamp)
{
    // 1. Let notification be a new notification.
    ConceptNotification notification;

    // FIXME: 2. If options["silent"] is true and options["vibrate"] exists, then throw a TypeError.

    // 3. If options["renotify"] is true and options["tag"] is the empty string, then throw a TypeError.
    if (options.renotify && options.tag.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "options[\"tag\"] cannot be the empty string when options[\"renotify\"] is set to true."_utf16 };

    // 4. Set notification’s data to StructuredSerializeForStorage(options["data"]).
    notification.data = TRY(HTML::structured_serialize_for_storage(realm.vm(), options.data));

    // 5. Set notification’s title to title.
    notification.title = title;

    // 6. Set notification’s direction to options["dir"].
    notification.direction = options.dir;

    // 7. Set notification’s language to options["lang"].
    notification.language = options.lang;

    // 8. Set notification’s origin to origin.
    notification.origin = move(origin);

    // 9. Set notification’s body to options["body"].
    notification.body = options.body;

    // 10. If options["navigate"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s navigation URL to the return value. (Otherwise notification’s navigation URL remains null.)
    if (options.navigate.has_value()) {
        auto navigate = TRY_OR_THROW_OOM(realm.vm(), options.navigate->utf16_view().to_utf8());
        notification.navigation_url = base_url.complete_url(navigate);
    }

    // 11. Set notification’s tag to options["tag"].
    notification.tag = options.tag;

    // 12. If options["image"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s image URL to the return value. (Otherwise notification’s image URL is not set.)
    if (options.image.has_value()) {
        auto image = TRY_OR_THROW_OOM(realm.vm(), options.image->utf16_view().to_utf8());
        notification.image_url = base_url.complete_url(image);
    }

    // 13. If options["icon"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s icon URL to the return value. (Otherwise notification’s icon URL is not set.)
    if (options.icon.has_value()) {
        auto icon = TRY_OR_THROW_OOM(realm.vm(), options.icon->utf16_view().to_utf8());
        notification.icon_url = base_url.complete_url(icon);
    }

    // 14. If options["badge"] exists, then parse it using baseURL, and if that does not return failure,
    // set notification’s badge URL to the return value. (Otherwise notification’s badge URL is not set.)
    if (options.badge.has_value()) {
        auto badge = TRY_OR_THROW_OOM(realm.vm(), options.badge->utf16_view().to_utf8());
        notification.badge_url = base_url.complete_url(badge);
    }

    // FIXME: 15. If options["vibrate"] exists, then validate and normalize it and
    // set notification’s vibration pattern to the return value.

    // 16. If options["timestamp"] exists, then set notification’s timestamp to the value.
    // Otherwise, set notification’s timestamp to fallbackTimestamp.
    if (options.timestamp.has_value())
        notification.timestamp = options.timestamp.value();
    else
        notification.timestamp = fallback_timestamp;

    // 17. Set notification’s renotify preference to options["renotify"].
    notification.renotify_preference = options.renotify;

    // 18. Set notification’s silent preference to options["silent"].
    notification.silent_preference = options.silent;

    // 19. Set notification’s require interaction preference to options["requireInteraction"].
    notification.require_interaction_preference = options.require_interaction;

    // 20. Set notification’s actions to « ».
    notification.actions = {};

    // 21. For each entry in options["actions"], up to the maximum number of actions supported (skip any excess entries):
    for (auto const& entry : options.actions) {
        // FIXME: stop the loop at the max number of actions supported

        // 1. Let action be a new notification action.
        ConceptNotification::Action action;

        // 2. Set action’s name to entry["action"].
        action.name = entry.action;

        // 3. Set action’s title to entry["title"].
        action.title = entry.title;

        // 4. If entry["navigate"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s navigation URL to the return value. (Otherwise action’s navigation URL remains null.)
        if (entry.navigate.has_value()) {
            auto navigate = TRY_OR_THROW_OOM(realm.vm(), entry.navigate->utf16_view().to_utf8());
            action.navigation_url = base_url.complete_url(navigate);
        }

        // 5. If entry["icon"] exists, then parse it using baseURL, and if that does not return failure,
        // set action’s icon URL to the return value. (Otherwise action’s icon URL remains null.)
        if (entry.icon.has_value()) {
            auto icon = TRY_OR_THROW_OOM(realm.vm(), entry.icon->utf16_view().to_utf8());
            action.icon_url = base_url.complete_url(icon);
        }

        // 6. Append action to notification’s actions.
        notification.actions.append(action);
    }

    // 22. Return notification.
    return notification;
}

// https://notifications.spec.whatwg.org/#create-a-notification-with-a-settings-object
WebIDL::ExceptionOr<ConceptNotification> Notification::create_a_notification_with_a_settings_object(
    JS::Realm& realm,
    Utf16String const& title,
    Bindings::NotificationOptions const& options,
    GC::Ref<HTML::EnvironmentSettingsObject> settings)
{
    // 1. Let origin be settings’s origin.
    URL::Origin origin = settings->origin();

    // 2. Let baseURL be settings’s API base URL.
    URL::URL base_url = settings->api_base_url();

    // 3. Let fallbackTimestamp be the number of milliseconds from the Unix epoch to settings’s current wall time,
    // rounded to the nearest integer.
    auto fallback_timestamp = round_to<HighResolutionTime::EpochTimeStamp>(settings->current_wall_time());

    // 4. Return the result of creating a notification given title, options, origin, baseURL, and fallbackTimestamp.
    return create_a_notification(realm, title, options, origin, base_url, fallback_timestamp);
}

// https://notifications.spec.whatwg.org/#constructors
WebIDL::ExceptionOr<GC::Ref<Notification>> Notification::construct_impl(
    JS::Realm& realm,
    Utf16String const& title,
    Bindings::NotificationOptions const& options)
{
    auto this_notification = realm.create<Notification>(realm);
    auto& relevant_settings_object = HTML::relevant_settings_object(this_notification);
    auto& relevant_global_object = HTML::relevant_global_object(this_notification);

    // 1. If this’s relevant global object is a ServiceWorkerGlobalScope object, then throw a TypeError.
    if (is<ServiceWorker::ServiceWorkerGlobalScope>(relevant_global_object))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "This’s relevant global object is a ServiceWorkerGlobalScope object"_utf16 };

    // 2. If options["actions"] is not empty, then throw a TypeError.
    if (!options.actions.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Options `action` is not empty"_utf16 };

    // 3. Let notification be the result of creating a notification with a settings object given title, options, and this’s relevant settings object.
    ConceptNotification notification = TRY(create_a_notification_with_a_settings_object(realm, title, options, relevant_settings_object));

    // 4. Associate this with notification.
    this_notification->m_notification = notification;

    // 5. Run these steps in parallel:
    // AD-HOC: We have no resources to fetch yet, so the show steps never block. Running them synchronously keeps the
    //         show and close events in the order scripts observe in other engines.
    {
        // 1. If the result of getting the notifications permission state is not "granted",
        //    then queue a task to fire an event named error on this, and abort these steps.
        if (get_the_notifications_permission_state() != Bindings::NotificationPermission::Granted) {
            this_notification->queue_notification_task([this_notification] {
                this_notification->dispatch_event(DOM::Event::create(this_notification->realm(), HTML::EventNames::error));
            });
            return this_notification;
        }

        // 2. Run the notification show steps for notification.
        this_notification->run_notification_show_steps();
    }

    return this_notification;
}

// https://notifications.spec.whatwg.org/#dom-notification-permission
Bindings::NotificationPermission Notification::permission(JS::VM&)
{
    // The static permission getter steps are to return the result of getting the notifications permission state.
    return get_the_notifications_permission_state();
}

// https://notifications.spec.whatwg.org/#dom-notification-requestpermission
GC::Ref<WebIDL::Promise> Notification::request_permission(JS::VM& vm, GC::Ptr<WebIDL::CallbackType> deprecated_callback)
{
    // 1. Let global be the current global object.
    auto& global = HTML::current_global_object();

    // 2. Let promise be a new promise in this's relevant Realm.
    auto& realm = *vm.current_realm();
    auto promise = WebIDL::create_promise(realm);

    // 3. Run these steps in parallel:
    // FIXME: Run these steps in parallel once we can ask the user for permission without blocking.
    {
        // 1. Let permissionState be the result of requesting permission to use "notifications".
        auto permission_state = PermissionsAPI::request_permission({ PermissionsAPI::PermissionNames::notifications.to_utf16_string() });
        auto notification_permission = permission_state == Bindings::PermissionState::Granted
            ? Bindings::NotificationPermission::Granted
            : Bindings::NotificationPermission::Denied;

        // 2. Queue a global task on the DOM manipulation task source given global to run these steps:
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, global, GC::create_function(realm.heap(), [&realm, promise, deprecated_callback, notification_permission] {
            HTML::TemporaryExecutionContext execution_context { realm };

            // 1. If deprecatedCallback is given, then invoke deprecatedCallback with « permissionState » and "report".
            if (deprecated_callback)
                (void)WebIDL::invoke_callback(*deprecated_callback, {}, WebIDL::ExceptionBehavior::Report, { { JS::PrimitiveString::create(realm.vm(), idl_enum_to_string(notification_permission)) } });

            // 2. Resolve promise with permissionState.
            WebIDL::resolve_promise(realm, promise, JS::PrimitiveString::create(realm.vm(), idl_enum_to_string(notification_permission)));
        }));
    }

    // 4. Return promise.
    return promise;
}

// https://notifications.spec.whatwg.org/#show-steps
void Notification::run_notification_show_steps()
{
    // FIXME: 1. Run the fetch steps for notification.
    // FIXME: 2. Wait for any fetches to complete and notification's image resource, icon resource, and badge resource
    //           to be set (if any), as well as the icon resources for the notification's actions (if any).

    // 3. Let shown be false.
    auto shown = false;

    // 4. Let oldNotification be the notification in the list of notifications whose tag is not the empty string and is
    //    notification's tag, and whose origin is same origin with notification's origin, if any, and null otherwise.
    auto old_notification = NotificationList::the().find_by_tag(m_notification.tag, m_notification.origin);

    // 5. If oldNotification is non-null:
    if (old_notification) {
        // 1. Handle close events with oldNotification.
        old_notification->handle_close_events();

        // 2. If the notification platform supports replacement:
        //    1. Replace oldNotification with notification, in the list of notifications.
        //    2. Set shown to true.
        // 3. Otherwise, remove oldNotification from the list of notifications.
        NotificationList::the().replace(*old_notification, *this);
        shown = true;
    }

    // 6. If shown is false:
    if (!shown) {
        // 1. Append notification to the list of notifications.
        NotificationList::the().append(*this);
    }

    // 2. Display notification on the device (e.g., by calling the appropriate notification platform API).
    // AD-HOC: We replace by closing the old platform notification and posting a new one, so this runs unconditionally.
    if (auto page = this->page()) {
        page->did_show_notification(PlatformNotification {
            .id = m_id,
            .title = m_notification.title,
            .body = m_notification.body,
            .language = m_notification.language,
            .icon_url = m_notification.icon_url,
            .image_url = m_notification.image_url,
            .badge_url = m_notification.badge_url,
            .silent = m_notification.silent_preference.value_or(false),
            .require_interaction = m_notification.require_interaction_preference,
            .renotify = m_notification.renotify_preference,
        });
        m_displayed_on_device = true;
    }

    // FIXME: 7. If shown is false or oldNotification is non-null, and notification's renotify preference is true, then
    //           run the alert steps for notification.

    // 8. If notification is a non-persistent notification, then queue a task to fire an event named show on the
    //    Notification object representing notification.
    queue_notification_task([this] {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::show));
    });
}

// https://notifications.spec.whatwg.org/#close-steps
void Notification::run_close_steps()
{
    // 1. If the list of notifications does not contain notification, then abort these steps.
    if (!NotificationList::the().contains(*this))
        return;

    // 2. Handle close events with notification.
    handle_close_events();

    // 3. Remove notification from the list of notifications.
    NotificationList::the().remove(*this);
}

// https://notifications.spec.whatwg.org/#handle-close-events
void Notification::handle_close_events()
{
    // FIXME: 1. If notification is a persistent notification and notification was closed by the end user, then fire a
    //           service worker notification event named "notificationclose" given notification.

    // 2. If notification is a non-persistent notification, then queue a task to fire an event named close on the
    //    Notification object representing notification.
    queue_notification_task([this] {
        dispatch_event(DOM::Event::create(realm(), HTML::EventNames::close));
    });

    // AD-HOC: Withdraw the platform notification. Both callers of these steps - closing and replacing a notification -
    //         want it to disappear from the device.
    if (m_displayed_on_device) {
        if (auto page = this->page())
            page->did_close_notification(m_id);
        m_displayed_on_device = false;
    }
}

// https://notifications.spec.whatwg.org/#activating-a-notification
void Notification::activate()
{
    // 1. Let action be null.
    // FIXME: 2. If one of notification's actions was activated by the end user, then set action to that notification action.

    // 3. Let navigationURL be notification's navigation URL.
    auto navigation_url = m_notification.navigation_url;

    // FIXME: 4. If action is non-null, then set navigationURL to action's navigation URL.

    // FIXME: 5. If navigationURL is non-null, navigate a top-level traversable to it and return.

    // FIXME: 6. If notification is a persistent notification, fire a service worker notification event named
    //           "notificationclick" given notification and actionName.

    // 7. Otherwise, queue a task to run these steps:
    queue_notification_task([this] {
        // 1. Let intoFocus be the result of firing an event named click on the Notification object representing
        //    notification, with its cancelable attribute initialized to true.
        Bindings::EventInit event_init {};
        event_init.cancelable = true;
        auto into_focus = dispatch_event(DOM::Event::create(realm(), HTML::EventNames::click, event_init));

        // FIXME: 2. If intoFocus is true, then the user agent should bring the notification's related browsing
        //           context's viewport into focus.
        (void)into_focus;
    });
}

// https://notifications.spec.whatwg.org/#dom-notification-close
void Notification::close()
{
    // The close() method steps are to run the close steps for this's notification.
    run_close_steps();
}

void Notification::queue_notification_task(Function<void()> steps)
{
    auto& realm = this->realm();
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*this), GC::create_function(realm.heap(), [&realm, steps = move(steps)] {
        HTML::TemporaryExecutionContext execution_context { realm };
        steps();
    }));
}

GC::Ptr<Page> Notification::page()
{
    // FIXME: Notifications created in a worker have no page to display them on.
    auto* window = as_if<HTML::Window>(HTML::relevant_global_object(*this));
    if (!window)
        return {};
    return window->page();
}

void Notification::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Notification);
    Base::initialize(realm);
}

// https://notifications.spec.whatwg.org/#handler-notification-onclick
void Notification::set_onclick(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::click, value);
}

// https://notifications.spec.whatwg.org/#handler-notification-onclick
GC::Ptr<WebIDL::CallbackType> Notification::onclick()
{
    return event_handler_attribute(HTML::EventNames::click);
}

// https://notifications.spec.whatwg.org/#handler-notification-onshow
void Notification::set_onshow(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::show, value);
}

// https://notifications.spec.whatwg.org/#handler-notification-onshow
GC::Ptr<WebIDL::CallbackType> Notification::onshow()
{
    return event_handler_attribute(HTML::EventNames::show);
}

// https://notifications.spec.whatwg.org/#handler-notification-onerror
void Notification::set_onerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::error, value);
}

// https://notifications.spec.whatwg.org/#handler-notification-onerror
GC::Ptr<WebIDL::CallbackType> Notification::onerror()
{
    return event_handler_attribute(HTML::EventNames::error);
}

// https://notifications.spec.whatwg.org/#handler-notification-onclose
void Notification::set_onclose(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::close, value);
}

// https://notifications.spec.whatwg.org/#handler-notification-onclose
GC::Ptr<WebIDL::CallbackType> Notification::onclose()
{
    return event_handler_attribute(HTML::EventNames::close);
}

// https://notifications.spec.whatwg.org/#dom-notification-actions
Vector<NotificationAction> Notification::actions() const
{
    // 1. Let frozenActions be an empty list of type NotificationAction.
    Vector<NotificationAction> frozen_actions;
    frozen_actions.ensure_capacity(m_notification.actions.capacity());

    // 2. For each entry of this’s notification’s actions:
    for (auto const& entry : m_notification.actions) {
        // 1. Let action be a new NotificationAction.
        NotificationAction action;

        // 2. Set action["action"] to entry’s name.
        action.action = entry.name;

        // 3. Set action["title"] to entry’s title.
        action.title = entry.title;

        // 4. If entry’s navigation URL is non-null, then set action["navigate"] to entry’s navigation URL, serialized.
        if (entry.navigation_url.has_value())
            action.navigate = serialize_url_for_bindings(entry.navigation_url);

        // 5. If entry’s icon URL is non-null, then set action["icon"] to entry’s icon URL, serialized.
        if (entry.icon_url.has_value())
            action.icon = serialize_url_for_bindings(entry.icon_url);

        // FIXME: 6. Call Object.freeze on action, to prevent accidental mutation by scripts.

        // 7. Append action to frozenActions.
        frozen_actions.append(action);
    }

    // FIXME: 3. Return the result of create a frozen array from frozenActions.
    return frozen_actions;
}

// https://notifications.spec.whatwg.org/#dom-notification-data
JS::Value Notification::data() const
{
    // The data getter steps are to return StructuredDeserialize(this’s notification’s data, this’s relevant Realm).
    // If this throws an exception, then return null.
    auto deserialized_data = HTML::structured_deserialize(vm(), m_notification.data, realm());
    if (!deserialized_data.is_exception())
        return deserialized_data.release_value();
    return JS::js_null();
}

Utf16String Notification::serialize_url_for_bindings(Optional<URL::URL> const& url)
{
    if (!url.has_value())
        return {};
    return Utf16String::from_utf8(url->serialize());
}

}
