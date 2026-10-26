/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWebView/ProcessLauncher.h>

namespace WebView {

// Launches the given executable as it is, connected to nothing but an in-memory transport, and reports exits only when
// told to.
class TestProcessLauncher final : public ProcessLauncher {
public:
    virtual void watch_for_exit(pid_t) override { }

    void simulate_exit(pid_t pid, Optional<int> exit_status) { on_exit(pid, exit_status); }

private:
    virtual ErrorOr<SpawnedProcess> spawn(ProcessType, Core::ProcessSpawnOptions& options) override
    {
        auto process = TRY(Core::Process::spawn(options));
        auto transports = TRY(IPC::Transport::create_paired());
        m_peer_handles.append(move(transports.remote_handle));
        return SpawnedProcess { move(process), move(transports.local) };
    }

    Vector<IPC::TransportHandle> m_peer_handles;
};

}
