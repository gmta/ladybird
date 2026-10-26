/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <LibWebView/ProcessManager.h>
#include <Tests/LibWebView/TestProcessLauncher.h>

struct ManagerWithLauncher {
    WebView::TestProcessLauncher& launcher;
    NonnullOwnPtr<WebView::ProcessManager> manager;
};

static ManagerWithLauncher create_manager()
{
    auto launcher = make<WebView::TestProcessLauncher>();
    auto& launcher_reference = *launcher;
    return { launcher_reference, make<WebView::ProcessManager>(move(launcher)) };
}

static WebView::SpawnedTransport spawn_helper(WebView::ProcessManager& manager, WebView::ProcessType type)
{
    auto spawned = MUST(manager.spawn_with_transport(type, { .executable = "/usr/bin/true" }, false));
    (void)Core::System::waitpid(spawned.identity.pid);
    return spawned;
}

TEST_CASE(spawning_registers_the_helper)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();

    auto spawned = spawn_helper(*manager, WebView::ProcessType::ImageDecoder);
    EXPECT_NE(spawned.identity.child_id, 0u);

    auto process = manager->find_child(spawned.identity.child_id);
    EXPECT(process.has_value());
    EXPECT_EQ(process->pid(), spawned.identity.pid);
    EXPECT_EQ(process->type(), WebView::ProcessType::ImageDecoder);
    EXPECT_EQ(manager->find_process(spawned.identity.pid)->child_id(), spawned.identity.child_id);
}

TEST_CASE(an_exit_removes_the_helper_and_reports_it)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    auto spawned = spawn_helper(*manager, WebView::ProcessType::WebContent);

    Optional<pid_t> exited_pid;
    Optional<int> exited_status;
    manager->on_process_exited = [&](WebView::Process&& process, Optional<int> exit_status) {
        exited_pid = process.pid();
        exited_status = exit_status;
    };
    launcher.simulate_exit(spawned.identity.pid, 0);

    EXPECT_EQ(exited_pid, spawned.identity.pid);
    EXPECT_EQ(exited_status, 0);
    EXPECT(!manager->find_child(spawned.identity.child_id).has_value());
    EXPECT(!manager->find_process(spawned.identity.pid).has_value());
}
