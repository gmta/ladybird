/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/NotificationProviderQt.h>
#include <UI/Qt/StringUtils.h>

#include <QDBusConnection>
#include <QDBusReply>
#include <QStringList>
#include <QVariantMap>

namespace Ladybird {

static QString notifications_service()
{
    return QStringLiteral("org.freedesktop.Notifications");
}

static QString notifications_path()
{
    return QStringLiteral("/org/freedesktop/Notifications");
}

// The action key that notification servers invoke when the end user clicks the notification body itself.
static QString default_action_key()
{
    return QStringLiteral("default");
}

ErrorOr<NonnullOwnPtr<NotificationProviderQt>> NotificationProviderQt::create()
{
    auto interface = make<QDBusInterface>(notifications_service(), notifications_path(), notifications_service(), QDBusConnection::sessionBus());
    if (!interface->isValid())
        return Error::from_string_literal("No org.freedesktop.Notifications service is available on the session bus");

    return adopt_own(*new NotificationProviderQt(move(interface)));
}

NotificationProviderQt::NotificationProviderQt(NonnullOwnPtr<QDBusInterface> interface)
    : m_interface(move(interface))
{
    QDBusConnection::sessionBus().connect(notifications_service(), notifications_path(), notifications_service(),
        QStringLiteral("NotificationClosed"), this, SLOT(notification_closed(uint, uint)));
    QDBusConnection::sessionBus().connect(notifications_service(), notifications_path(), notifications_service(),
        QStringLiteral("ActionInvoked"), this, SLOT(action_invoked(uint, QString)));
}

ErrorOr<void> NotificationProviderQt::show(NotificationId id, Core::Notification const& notification)
{
    QVariantMap hints;
    if (notification.silent)
        hints[QStringLiteral("suppress-sound")] = true;
    hints[QStringLiteral("desktop-entry")] = QStringLiteral("org.ladybird.Ladybird");

    // A notification that requires interaction must never expire on its own.
    auto expire_timeout = notification.require_interaction ? 0 : -1;

    QDBusReply<uint> reply = m_interface->call(QStringLiteral("Notify"),
        QStringLiteral("Ladybird"),
        0u,
        QString {},
        qstring_from_ak_string(notification.title),
        qstring_from_ak_string(notification.body),
        QStringList { default_action_key(), QStringLiteral("Open") },
        hints,
        expire_timeout);

    if (!reply.isValid()) {
        dbgln("Notification server rejected the notification: {} ({})",
            ak_string_from_qstring(reply.error().message()), ak_string_from_qstring(reply.error().name()));
        return Error::from_string_literal("The notification server rejected the notification");
    }

    m_server_ids.set(id, reply.value());
    return {};
}

void NotificationProviderQt::close(NotificationId id)
{
    auto server_id = m_server_ids.take(id);
    if (!server_id.has_value())
        return;

    m_interface->call(QStringLiteral("CloseNotification"), *server_id);
}

void NotificationProviderQt::notification_closed(uint server_id, uint)
{
    auto id = notification_id_for_server_id(server_id);
    if (!id.has_value())
        return;

    m_server_ids.remove(*id);
    if (on_closed)
        on_closed(*id);
}

void NotificationProviderQt::action_invoked(uint server_id, QString const& action_key)
{
    if (action_key != default_action_key())
        return;

    auto id = notification_id_for_server_id(server_id);
    if (!id.has_value())
        return;

    if (on_activated)
        on_activated(*id);
}

Optional<Core::NotificationProvider::NotificationId> NotificationProviderQt::notification_id_for_server_id(uint server_id) const
{
    for (auto const& entry : m_server_ids) {
        if (entry.value == server_id)
            return entry.key;
    }
    return {};
}

static ErrorOr<NonnullOwnPtr<Core::NotificationProvider>> create_notification_provider_qt()
{
    return NotificationProviderQt::create();
}

static bool is_qt_notifications_available()
{
    return QDBusConnection::sessionBus().isConnected();
}

void install_qt_notification_provider()
{
    Core::NotificationProvider::set_provider_functions(create_notification_provider_qt, is_qt_notifications_available);
}

}
