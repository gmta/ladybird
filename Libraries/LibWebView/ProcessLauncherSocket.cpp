/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibWebView/ProcessLauncherSocket.h>

#include <sys/socket.h>

namespace WebView {

ErrorOr<ProcessLauncher::SpawnedProcess> ProcessLauncherSocket::spawn(ProcessType type, Core::ProcessSpawnOptions& options)
{
    restrict_helper_options(type, options);

    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_fds));

    ArmedScopeGuard guard_fd_0 { [&] { MUST(Core::System::close(socket_fds[0])); } };
    ScopeGuard guard_fd_1 { [&] { MUST(Core::System::close(socket_fds[1])); } };

    // Only the helper's end reaches the helper, and only this helper.
    options.file_actions.append(Core::FileAction::DupFd { .write_fd = socket_fds[1], .fd = socket_fds[1] });

    // NB: Only the helper gets to see this, if it has an environment of its own.
    auto takeover_string = ByteString::formatted("ipc:{}", socket_fds[1]);
    if (options.environment.has_value())
        options.environment->append(ByteString::formatted("SOCKET_TAKEOVER={}", takeover_string));
    else
        TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    auto process = TRY(Core::Process::spawn(options));
    ArmedScopeGuard terminate_process_on_failure { [&] {
        (void)Core::Process::terminate_process(process.pid(), Core::Process::TerminationMode::Forceful);
    } };

    auto ipc_socket = TRY(Core::LocalSocket::adopt_fd(socket_fds[0]));
    guard_fd_0.disarm();
    TRY(ipc_socket->set_blocking(true));

    terminate_process_on_failure.disarm();
    return SpawnedProcess { move(process), make<IPC::Transport>(move(ipc_socket)) };
}

}
