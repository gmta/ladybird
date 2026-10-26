/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Platform/ProcessResourceUsage.h>
#include <LibCore/Platform/ProcessStatistics.h>
#include <LibCore/Process.h>
#include <LibCore/Timer.h>
#include <LibWebView/ChildIdentity.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessLauncher.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

WEBVIEW_API ProcessType process_type_from_name(StringView);
WEBVIEW_API StringView process_name_from_type(ProcessType type);

struct RestartBudget {
    size_t restarts { 0 };
    Optional<AK::Duration> window;
};

template<typename ClientType>
struct SpawnedClient {
    ChildIdentity identity;
    NonnullRefPtr<ClientType> client;
};

struct SpawnedTransport {
    ChildIdentity identity;
    NonnullOwnPtr<IPC::Transport> transport;
};

class WEBVIEW_API ProcessManager {
    AK_MAKE_NONCOPYABLE(ProcessManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ProcessManager(NonnullOwnPtr<ProcessLauncher>);

    void set_lost_handler(ProcessType, Function<void(Process&)>);
    void set_exit_handler(ProcessType, Function<void(Process&)>);
    void set_restart_budget(ProcessType, RestartBudget, Function<void(u64 owner_child_id)> on_exhausted);

    bool may_restart(ProcessType, u64 owner_child_id = 0);

    template<typename ClientType, typename... ClientArguments>
    ErrorOr<SpawnedClient<ClientType>> spawn(ProcessType, Core::ProcessSpawnOptions const&, bool capture_output, ClientArguments&&...);

    // For a helper that serves another process: The caller hands the transport over instead of connecting to it.
    ErrorOr<SpawnedTransport> spawn_with_transport(ProcessType, Core::ProcessSpawnOptions const&, bool capture_output);

    void for_each_process(Function<void(Process&)>);
    void for_each_process_statistics(Function<void(Process&, Core::Platform::ProcessInfo const&)>);
    Optional<Process> remove_process(u64 child_id);
    void remove_all_processes();
    Optional<Process&> find_process(pid_t);
    Optional<Process&> find_child(u64 child_id);
    ErrorOr<void> terminate_child(u64 child_id, Core::Process::TerminationMode);
    void cancel_forced_exit(u64 child_id);
    void force_exit_after_timeout(u64 child_id, int timeout_ms);

#if defined(AK_OS_MACH)
    void set_process_task_name_port(pid_t, Core::MachPort&&);
#endif

    void update_all_process_statistics();
    Optional<Core::Platform::ProcessResourceUsage> resource_usage(pid_t) const;

    Function<void(Process&)> on_process_added; // test-web
    Function<void(Process&&, Optional<int> exit_status)> on_process_exited;

private:
    // Gives the process its child ID and starts watching for its exit.
    ChildIdentity add_process(Process&&);
    void did_lose(Process&);
    void did_exit(pid_t, Optional<int> exit_status);
    void verify_event_loop() const;

    NonnullOwnPtr<ProcessLauncher> m_launcher;
    HashMap<ProcessType, Function<void(Process&)>> m_lost_handlers;
    HashMap<ProcessType, Function<void(Process&)>> m_exit_handlers;

    struct RestartPolicy {
        RestartBudget budget;
        Function<void(u64 owner_child_id)> on_exhausted;
    };
    HashMap<ProcessType, RestartPolicy> m_restart_policies;
    HashMap<ProcessType, HashMap<u64, Vector<MonotonicTime>>> m_restart_times;

    Core::Platform::ProcessStatistics m_statistics;
    HashMap<u64, NonnullOwnPtr<Process>> m_processes;
    // NB: Exit notifications and statistics name processes by PID.
    HashMap<pid_t, u64> m_child_ids_by_pid;
    HashMap<u64, RefPtr<Core::Timer>> m_forced_exit_timers;
    u64 m_next_child_id { 1 };
    Core::EventLoop* m_creation_event_loop { &Core::EventLoop::current() };
};

template<typename ClientType, typename... ClientArguments>
ErrorOr<SpawnedClient<ClientType>> ProcessManager::spawn(ProcessType type, Core::ProcessSpawnOptions const& options, bool capture_output, ClientArguments&&... client_arguments)
{
    auto launched = TRY(m_launcher->launch(type, options, capture_output));
    auto client = adopt_ref(*new ClientType { move(launched.transport), forward<ClientArguments>(client_arguments)... });
    auto identity = add_process(Process { type, move(launched.process), move(launched.output_capture), client.ptr() });
    return SpawnedClient<ClientType> { identity, move(client) };
}

}
