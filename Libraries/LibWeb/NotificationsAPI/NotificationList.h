/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::NotificationsAPI {

// https://notifications.spec.whatwg.org/#list-of-notifications
// The user agent must keep a list of notifications, which is a list of zero or more notifications. Entries are rooted
// because a Notification object must not be garbage collected while the list contains its notification.
// FIXME: The spec says user agents should run the close steps for a non-persistent notification a couple of seconds
//        after it was created. Until we do, a notification that neither the page nor the notification platform ever
//        closes stays in this list, and stays alive, for the lifetime of the process.
class WEB_API NotificationList {
public:
    static NotificationList& the();

    void append(Notification&);
    void remove(Notification&);
    void replace(Notification& old_notification, Notification& new_notification);
    bool contains(Notification const&) const;

    GC::Ptr<Notification> find_by_tag(Utf16View tag, URL::Origin const&) const;
    GC::Ptr<Notification> find_by_id(u64 id) const;

private:
    NotificationList() = default;

    Vector<GC::Root<Notification>> m_notifications;
};

}
