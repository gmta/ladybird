/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibCore/NotificationProvider.h>
#include <QDBusInterface>
#include <QObject>

namespace Ladybird {

// Displays notifications using the org.freedesktop.Notifications D-Bus interface, which is what desktop environments
// on Linux and the other BSDs implement.
class NotificationProviderQt final
    : public QObject
    , public Core::NotificationProvider {
    Q_OBJECT

public:
    static ErrorOr<NonnullOwnPtr<NotificationProviderQt>> create();
    virtual ~NotificationProviderQt() override = default;

    virtual ErrorOr<void> show(NotificationId, Core::Notification const&) override;
    virtual void close(NotificationId) override;

private slots:
    void notification_closed(uint server_id, uint reason);
    void action_invoked(uint server_id, QString const& action_key);

private:
    explicit NotificationProviderQt(NonnullOwnPtr<QDBusInterface>);

    Optional<NotificationId> notification_id_for_server_id(uint server_id) const;

    NonnullOwnPtr<QDBusInterface> m_interface;
    HashMap<NotificationId, uint> m_server_ids;
};

void install_qt_notification_provider();

}
