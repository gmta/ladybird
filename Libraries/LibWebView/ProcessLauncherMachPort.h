/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/TransportBootstrapMach.h>
#include <LibWebView/ProcessLauncherPOSIX.h>
#include <LibWebView/ProcessReaper.h>

namespace WebView {

class WEBVIEW_API ProcessLauncherMachPort final : public ProcessLauncherPOSIX {
public:
    explicit ProcessLauncherMachPort(IPC::TransportBootstrapMachServer&);

    virtual void forget(pid_t) override;

private:
    virtual ErrorOr<SpawnedProcess> spawn(ProcessType, Core::ProcessSpawnOptions&) override;

    void watch_with_process_reaper(pid_t);

    IPC::TransportBootstrapMachServer& m_bootstrap_server;
    ProcessReaper* m_process_reaper { nullptr };
    bool m_tried_to_start_process_reaper { false };
};

}
