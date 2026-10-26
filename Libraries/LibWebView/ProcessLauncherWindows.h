/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <LibIPC/HandleBrokerServer.h>
#include <LibWebView/ProcessLauncher.h>

namespace WebView {

class WEBVIEW_API ProcessLauncherWindows final : public ProcessLauncher {
public:
    ProcessLauncherWindows();
    virtual ~ProcessLauncherWindows() override;

    virtual void watch_for_exit(pid_t) override;

private:
    virtual ErrorOr<SpawnedProcess> spawn(ProcessType, Core::ProcessSpawnOptions&) override;

    NonnullOwnPtr<IPC::HandleBrokerServer> m_handle_broker_server;
    HashTable<pid_t> m_watched_processes;
};

}
