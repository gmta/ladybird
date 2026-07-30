/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/String.h>
#include <LibCore/NotificationProviderImplementation.h>

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#if !__has_feature(objc_arc)
#    error "This file requires ARC"
#endif

static NSString* const ladybird_notification_category = @"org.ladybird.WebNotification";

namespace Core {

class NotificationProviderUserNotifications;

static NSString* to_nsstring(String const& string)
{
    return [[NSString alloc] initWithBytes:string.bytes().data()
                                    length:string.bytes().size()
                                  encoding:NSUTF8StringEncoding];
}

}

@interface LadybirdNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>

@property (nonatomic, assign) Core::NotificationProviderUserNotifications* provider;

@end

namespace Core {

class NotificationProviderUserNotifications final : public NotificationProvider {
public:
    static NonnullOwnPtr<NotificationProviderUserNotifications> create()
    {
        return adopt_own(*new NotificationProviderUserNotifications());
    }

    virtual ~NotificationProviderUserNotifications() override
    {
        m_delegate.provider = nullptr;
        if ([UNUserNotificationCenter currentNotificationCenter].delegate == m_delegate)
            [UNUserNotificationCenter currentNotificationCenter].delegate = nil;
    }

    virtual ErrorOr<void> show(NotificationId, Notification const&) override;
    virtual void close(NotificationId) override;

    void did_activate(NotificationId id)
    {
        if (on_activated)
            on_activated(id);
    }

    void did_close(NotificationId id)
    {
        if (on_closed)
            on_closed(id);
    }

private:
    NotificationProviderUserNotifications()
    {
        m_delegate = [[LadybirdNotificationDelegate alloc] init];
        m_delegate.provider = this;

        auto* center = [UNUserNotificationCenter currentNotificationCenter];
        center.delegate = m_delegate;

        // The dismiss action is only reported back to us for notifications in a category that asks for it.
        auto* category = [UNNotificationCategory categoryWithIdentifier:ladybird_notification_category
                                                                actions:@[]
                                                      intentIdentifiers:@[]
                                                                options:UNNotificationCategoryOptionCustomDismissAction];
        [center setNotificationCategories:[NSSet setWithObject:category]];

        // macOS delivers notifications for several authorization states that report an error or no grant here, so this
        // is informational only: we always post the notification and let the platform decide whether to show it.
        [center requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                              completionHandler:^(BOOL granted, NSError* error) {
                                  if (error)
                                      dbgln("Notifications were not explicitly authorized: {}", error.localizedDescription.UTF8String);
                                  else if (!granted)
                                      dbgln("Notifications were not explicitly authorized");
                              }];
    }

    LadybirdNotificationDelegate* m_delegate;
};

}

@implementation LadybirdNotificationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*) [[maybe_unused]] center
       willPresentNotification:(UNNotification*) [[maybe_unused]] notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    // Without this, notifications posted while Ladybird is the frontmost application are not shown at all.
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList | UNNotificationPresentationOptionSound);
}

- (void)userNotificationCenter:(UNUserNotificationCenter*) [[maybe_unused]] center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    if (self.provider) {
        auto id = static_cast<Core::NotificationProvider::NotificationId>(response.notification.request.identifier.longLongValue);

        if ([response.actionIdentifier isEqualToString:UNNotificationDismissActionIdentifier])
            self.provider->did_close(id);
        else if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier])
            self.provider->did_activate(id);
    }

    completionHandler();
}

@end

namespace Core {

ErrorOr<void> NotificationProviderUserNotifications::show(NotificationId id, Notification const& notification)
{
    auto* content = [[UNMutableNotificationContent alloc] init];
    content.title = to_nsstring(notification.title);
    content.body = to_nsstring(notification.body);
    content.categoryIdentifier = ladybird_notification_category;
    content.sound = notification.silent ? nil : [UNNotificationSound defaultSound];
    content.interruptionLevel = notification.silent ? UNNotificationInterruptionLevelPassive : UNNotificationInterruptionLevelActive;

    // FIXME: Attach the notification's icon and image once the browser process fetches them.
    // FIXME: macOS decides on its own whether a notification stays on screen until dismissed, so we cannot honor the
    //        notification's require interaction preference.

    auto* request = [UNNotificationRequest requestWithIdentifier:[NSString stringWithFormat:@"%llu", id]
                                                         content:content
                                                         trigger:nil];

    [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:request
                                                           withCompletionHandler:^(NSError* error) {
                                                               if (error)
                                                                   dbgln("Failed to show notification: {}", error.localizedDescription.UTF8String);
                                                           }];
    return {};
}

void NotificationProviderUserNotifications::close(NotificationId id)
{
    auto* identifiers = @[ [NSString stringWithFormat:@"%llu", id] ];
    auto* center = [UNUserNotificationCenter currentNotificationCenter];

    [center removePendingNotificationRequestsWithIdentifiers:identifiers];
    [center removeDeliveredNotificationsWithIdentifiers:identifiers];
}

ErrorOr<NonnullOwnPtr<NotificationProvider>> create_platform_notification_provider()
{
    if (!platform_notification_provider_is_available())
        return Error::from_string_literal("Notifications require the application to be run from a bundle");
    return NotificationProviderUserNotifications::create();
}

bool platform_notification_provider_is_available()
{
    // UNUserNotificationCenter raises an exception when the running process is not part of an application bundle.
    return [NSBundle mainBundle].bundleIdentifier != nil;
}

}
