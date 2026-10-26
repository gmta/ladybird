/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessLauncherPOSIX.h>

#include <sys/wait.h>

namespace WebView {

ProcessLauncherPOSIX::ProcessLauncherPOSIX()
{
    m_signal_handle = Core::EventLoop::register_signal(SIGCHLD, [this](int) {
        auto result = Core::System::waitpid(-1, WNOHANG);
        while (!result.is_error() && result.value().pid > 0) {
            auto& [pid, status] = result.value();
            if (m_watched_processes.contains(pid) && (WIFEXITED(status) || WIFSIGNALED(status))) {
                m_watched_processes.remove(pid);
                on_exit(pid, status);
            }
            result = Core::System::waitpid(-1, WNOHANG);
        }
    });
}

ProcessLauncherPOSIX::~ProcessLauncherPOSIX()
{
    Core::EventLoop::unregister_signal(m_signal_handle);
}

void ProcessLauncherPOSIX::watch_for_exit(pid_t pid)
{
    m_watched_processes.set(pid);
}

void ProcessLauncherPOSIX::restrict_helper_options(ProcessType type, Core::ProcessSpawnOptions& options)
{
#if defined(AK_OS_MACOS) || defined(AK_OS_LINUX)
    options.environment = Process::helper_process_environment(type);
#else
    (void)type;
#endif

    // Helpers must not read the terminal or pipe that the Browser was started from.
    options.file_actions.append(Core::FileAction::OpenFile { .path = "/dev/null", .mode = Core::File::OpenMode::Read, .fd = STDIN_FILENO });
}

}
