/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Mutex.h>
#include <LibCore/MachPort.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/TransportMachPort.h>
#include <LibWebView/ProcessLauncherMachPort.h>
#include <LibWebView/Utilities.h>

namespace WebView {

ProcessLauncherMachPort::ProcessLauncherMachPort(IPC::TransportBootstrapMachServer& bootstrap_server)
    : m_bootstrap_server(bootstrap_server)
{
}

void ProcessLauncherMachPort::forget(pid_t pid)
{
    m_bootstrap_server.unregister_child_transport(pid);
}

ErrorOr<ProcessLauncher::SpawnedProcess> ProcessLauncherMachPort::spawn(ProcessType type, Core::ProcessSpawnOptions& options)
{
    restrict_helper_options(type, options);

    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));
    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    // The child may receive startup messages before it has constructed its transport.
    IPC::TransportMachPort::raise_receive_queue_limit(port_b_recv);

    MutexLocker child_registration_locker(m_bootstrap_server.child_registration_lock());
    auto process = TRY(Core::Process::spawn(options));
    watch_with_process_reaper(process.pid());

    m_bootstrap_server.register_child_transport(process.pid(), IPC::TransportBootstrapMachPorts { move(port_b_recv), move(port_a_send) });

    return SpawnedProcess { move(process), make<IPC::Transport>(move(port_a_recv), move(port_b_send)) };
}

// The kernel does not kill our helpers when we exit or die on macOS, so the ProcessReaper helper does it for us.
void ProcessLauncherMachPort::watch_with_process_reaper(pid_t pid)
{
    if (!m_tried_to_start_process_reaper) {
        m_tried_to_start_process_reaper = true;
        auto candidate_paths = get_paths_for_helper_process("ProcessReaper"sv);
        if (!candidate_paths.is_error()) {
            for (auto const& path : candidate_paths.value()) {
                if (!FileSystem::exists(path))
                    continue;
                // Never destroyed: the pipe to the reaper has to stay open until this process is gone.
                if (auto process_reaper = ProcessReaper::start(path); !process_reaper.is_error()) {
                    m_process_reaper = process_reaper.release_value().leak_ptr();
                    break;
                }
            }
        }
        if (!m_process_reaper)
            warnln("Could not start the ProcessReaper, so helper processes may outlive this process");
    }

    if (m_process_reaper) {
        if (auto result = m_process_reaper->watch(pid); result.is_error())
            warnln("Could not ask the ProcessReaper to watch process {}: {}", pid, result.error());
    }
}

}
