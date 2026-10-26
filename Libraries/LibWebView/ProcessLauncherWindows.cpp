/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibWebView/ProcessLauncherWindows.h>

namespace WebView {

ProcessLauncherWindows::ProcessLauncherWindows()
    : m_handle_broker_server(IPC::HandleBrokerServer::create())
{
}

ProcessLauncherWindows::~ProcessLauncherWindows()
{
    for (pid_t pid : m_watched_processes)
        Core::EventLoop::unregister_process(pid);
}

void ProcessLauncherWindows::watch_for_exit(pid_t pid)
{
    if (m_watched_processes.set(pid, AK::HashSetExistingEntryBehavior::Keep) != AK::HashSetResult::InsertedNewEntry)
        return;

    Core::EventLoop::register_process(pid, [this](pid_t pid) {
        m_watched_processes.remove(pid);
        on_exit(pid, {});
    });
}

ErrorOr<ProcessLauncher::SpawnedProcess> ProcessLauncherWindows::spawn(ProcessType, Core::ProcessSpawnOptions& options)
{
    // A Windows pipe cannot carry a handle, so the helper also gets a line to the Browser's handle broker.
    auto channel = TRY(Core::System::duplex_pipe());
    ArmedScopeGuard guard_channel_0 { [&] { MUST(Core::System::close(channel[0])); } };
    ScopeGuard guard_channel_1 { [&] { MUST(Core::System::close(channel[1])); } };
    auto broker_channel = TRY(Core::System::duplex_pipe());
    ArmedScopeGuard guard_broker_channel_0 { [&] { MUST(Core::System::close(broker_channel[0])); } };
    ScopeGuard guard_broker_channel_1 { [&] { MUST(Core::System::close(broker_channel[1])); } };

    // Only the helper's ends reach the helper, and only this helper.
    options.file_actions.append(Core::FileAction::DupFd { .write_fd = channel[1], .fd = channel[1] });
    options.file_actions.append(Core::FileAction::DupFd { .write_fd = broker_channel[1], .fd = broker_channel[1] });
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, ByteString::formatted("ipc:{};handle-broker:{}", channel[1], broker_channel[1]), Core::Environment::Overwrite::Yes));

    auto process = TRY(Core::Process::spawn(options));
    ArmedScopeGuard terminate_process_on_failure { [&] {
        (void)Core::Process::terminate_process(process.pid(), Core::Process::TerminationMode::Forceful);
    } };

    auto transport = TRY(IPC::Transport::adopt_fd(channel[0]));
    guard_channel_0.disarm();

    guard_broker_channel_0.disarm();
    m_handle_broker_server->add_child(process.pid(), broker_channel[0]);

    terminate_process_on_failure.disarm();
    return SpawnedProcess { move(process), move(transport) };
}

}
