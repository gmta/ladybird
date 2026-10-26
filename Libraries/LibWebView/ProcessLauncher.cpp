/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/StatsLog.h>
#include <LibCore/System.h>
#include <LibWebView/ProcessLauncher.h>

#include <fcntl.h>

namespace WebView {

ErrorOr<LaunchedProcess> ProcessLauncher::launch(ProcessType type, Core::ProcessSpawnOptions const& options, bool capture_output)
{
    auto spawn_options = options;
    spawn_options.die_with_parent = true;

    Vector<ByteString> arguments_with_stats;
    if (auto* stats_log = Core::StatsLog::the()) {
        arguments_with_stats = Vector<ByteString> { options.arguments };
        arguments_with_stats.append(ByteString::formatted("--stats-log-dir={}", stats_log->directory()));
        spawn_options.arguments = arguments_with_stats;
    }

    Array<int, 2> stdout_pipe {};
    Array<int, 2> stderr_pipe {};
    if (capture_output) {
        stdout_pipe = TRY(Core::System::pipe2(O_CLOEXEC));
        stderr_pipe = TRY(Core::System::pipe2(O_CLOEXEC));

        // The write ends stay close-on-exec, so no other process that is spawned meanwhile inherits them. The child
        // gets its copies from these file actions.
        spawn_options.file_actions.append(Core::FileAction::DupFd { .write_fd = stdout_pipe[1], .fd = STDOUT_FILENO });
        spawn_options.file_actions.append(Core::FileAction::DupFd { .write_fd = stderr_pipe[1], .fd = STDERR_FILENO });
    }

    auto [process, transport] = TRY(spawn(type, spawn_options));

    // NB: A helper that we fail to connect to is ended rather than left running untracked.
    ArmedScopeGuard terminate_process_on_failure { [&, pid = process.pid()] {
        (void)Core::Process::terminate_process(pid, Core::Process::TerminationMode::Forceful);
        forget(pid);
    } };

    ProcessOutputCapture output_capture;
    if (capture_output) {
        MUST(Core::System::close(stdout_pipe[1]));
        MUST(Core::System::close(stderr_pipe[1]));
        output_capture.stdout_file = TRY(Core::File::adopt_fd(stdout_pipe[0], Core::File::OpenMode::Read));
        output_capture.stderr_file = TRY(Core::File::adopt_fd(stderr_pipe[0], Core::File::OpenMode::Read));
    }

    terminate_process_on_failure.disarm();
    return LaunchedProcess { move(process), move(transport), move(output_capture) };
}

}
