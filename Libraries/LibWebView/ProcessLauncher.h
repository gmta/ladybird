/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/Process.h>
#include <LibIPC/Transport.h>
#include <LibWebView/Export.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

struct LaunchedProcess {
    Core::Process process;
    NonnullOwnPtr<IPC::Transport> transport;
    ProcessOutputCapture output_capture;
};

class WEBVIEW_API ProcessLauncher {
    AK_MAKE_NONCOPYABLE(ProcessLauncher);
    AK_MAKE_NONMOVABLE(ProcessLauncher);

public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~ProcessLauncher() = default;

    ErrorOr<LaunchedProcess> launch(ProcessType, Core::ProcessSpawnOptions const&, bool capture_output);

    virtual void watch_for_exit(pid_t) = 0;
    virtual void forget(pid_t) { }

    Function<void(pid_t, Optional<int> exit_status)> on_exit;

protected:
    ProcessLauncher() = default;

    struct SpawnedProcess {
        Core::Process process;
        NonnullOwnPtr<IPC::Transport> transport;
    };
    virtual ErrorOr<SpawnedProcess> spawn(ProcessType, Core::ProcessSpawnOptions&) = 0;
};

}
