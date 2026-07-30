/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/NotificationsAPI/Notification.h>
#include <LibWeb/NotificationsAPI/NotificationList.h>

namespace Web::NotificationsAPI {

NotificationList& NotificationList::the()
{
    static auto& s_the = *new NotificationList;
    return s_the;
}

void NotificationList::append(Notification& notification)
{
    m_notifications.append(GC::Root<Notification> { notification });
}

void NotificationList::remove(Notification& notification)
{
    m_notifications.remove_all_matching([&](auto const& entry) { return entry.ptr() == &notification; });
}

void NotificationList::replace(Notification& old_notification, Notification& new_notification)
{
    for (auto& entry : m_notifications) {
        if (entry.ptr() == &old_notification) {
            entry = GC::Root<Notification> { new_notification };
            return;
        }
    }
}

bool NotificationList::contains(Notification const& notification) const
{
    return any_of(m_notifications, [&](auto const& entry) { return entry.ptr() == &notification; });
}

GC::Ptr<Notification> NotificationList::find_by_tag(Utf16View tag, URL::Origin const& origin) const
{
    if (tag.is_empty())
        return {};

    for (auto const& entry : m_notifications) {
        if (entry->tag() == tag && entry->origin().is_same_origin(origin))
            return *entry;
    }
    return {};
}

GC::Ptr<Notification> NotificationList::find_by_id(u64 id) const
{
    for (auto const& entry : m_notifications) {
        if (entry->id() == id)
            return *entry;
    }
    return {};
}

}
