/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NeverDestroyed.h>
#include <AK/String.h>
#include <LibURL/URL.h>

namespace URL {

#define ENUMERATE_INTERNAL_URLS                          \
    __URL_ENUMERATE(bookmarks, "bookmarks"_string)       \
    __URL_ENUMERATE(crash_report, "crash-report"_string) \
    __URL_ENUMERATE(downloads, "downloads"_string)       \
    __URL_ENUMERATE(history, "history"_string)           \
    __URL_ENUMERATE(newtab, "newtab"_string)             \
    __URL_ENUMERATE(settings, "settings"_string)         \
    __URL_ENUMERATE(version, "version"_string)

#define __URL_ENUMERATE(name, value)                        \
    inline URL const& about_##name()                        \
    {                                                       \
        static NeverDestroyed<URL> url = URL::about(value); \
        return *url;                                        \
    }
ENUMERATE_INTERNAL_URLS
#undef __URL_ENUMERATE

inline bool is_webui_url(URL const& url)
{
#define __URL_ENUMERATE(name, value) \
    if (url == about_##name())       \
        return true;
    ENUMERATE_INTERNAL_URLS
#undef __URL_ENUMERATE

    return false;
}

}
