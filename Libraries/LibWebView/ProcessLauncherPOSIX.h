/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibWebView/ProcessLauncher.h>

namespace WebView {

class WEBVIEW_API ProcessLauncherPOSIX : public ProcessLauncher {
public:
    ProcessLauncherPOSIX();
    virtual ~ProcessLauncherPOSIX() override;

    virtual void watch_for_exit(pid_t) override;

protected:
    static void restrict_helper_options(ProcessType, Core::ProcessSpawnOptions&);

private:
    HashTable<pid_t> m_watched_processes;
    int m_signal_handle { -1 };
};

}
