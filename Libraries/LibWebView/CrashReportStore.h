/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Time.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

// The directory of saved crash reports and the browser's own pending capture file. CrashReport
// captures a crashing process; this owns everything that lives on disk.
//
// Every entry point opens the directory through one helper that refuses a directory owned by another
// user, and reads only regular, bounded files that the current user owns.
class WEBVIEW_API CrashReportStore {
public:
    // The browser's own store, under the user data directory.
    static CrashReportStore& the();
    static ByteString default_directory();

    explicit CrashReportStore(ByteString directory)
        : m_directory(move(directory))
    {
    }

    ByteString const& directory() const { return m_directory; }

    // Writes report text under a name derived from the time of the crash and returns that name,
    // then drops the oldest reports beyond the retention limit.
    ErrorOr<ByteString> store_report(ProcessType, StringView text, UnixDateTime crashed_at) const;

    // Formats the signal-safe records left by browsers that are no longer running, and returns how
    // many were recovered. A report keeps the time of the crash, not the time it was recovered.
    ErrorOr<size_t> recover_pending_reports() const;

    // Recovers earlier crashes, then installs this process's handler. Browser processes only.
    ErrorOr<void> initialize_browser_crash_handler();

    ErrorOr<void> show_directory() const;

    static bool is_saved_report_name(StringView);

private:
    ByteString m_directory;
};

}
