/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibRequests/RequestControlClient.h>
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

struct LossCounts {
    size_t lost { 0 };
    size_t exited { 0 };
};

static void count_losses(WebView::ProcessManager& manager, WebView::ProcessType type, LossCounts& counts)
{
    manager.set_lost_handler(type, [&](WebView::Process&) { ++counts.lost; });
    manager.set_exit_handler(type, [&](WebView::Process&) { ++counts.exited; });
}

TEST_CASE(a_helper_whose_connection_closes_first_is_lost_once)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    LossCounts counts;
    count_losses(*manager, WebView::ProcessType::RequestServer, counts);

    auto spawned = MUST(manager->spawn<Requests::RequestControlClient>(WebView::ProcessType::RequestServer, { .executable = "/usr/bin/true" }, false));
    (void)Core::System::waitpid(spawned.identity.pid);

    spawned.client->shutdown();
    EXPECT_EQ(counts.lost, 1u);
    EXPECT_EQ(counts.exited, 0u);
    EXPECT(manager->find_child(spawned.identity.child_id)->was_lost());

    launcher.simulate_exit(spawned.identity.pid, 0);
    EXPECT_EQ(counts.lost, 1u);
    EXPECT_EQ(counts.exited, 1u);
}

TEST_CASE(a_helper_that_exits_first_is_lost_on_exit)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    LossCounts counts;
    count_losses(*manager, WebView::ProcessType::RequestServer, counts);

    auto spawned = MUST(manager->spawn<Requests::RequestControlClient>(WebView::ProcessType::RequestServer, { .executable = "/usr/bin/true" }, false));
    (void)Core::System::waitpid(spawned.identity.pid);

    launcher.simulate_exit(spawned.identity.pid, 0);
    EXPECT_EQ(counts.lost, 1u);
    EXPECT_EQ(counts.exited, 1u);

    spawned.client->shutdown();
    EXPECT_EQ(counts.lost, 1u);
}

TEST_CASE(exits_while_shutting_down_are_not_handled)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    LossCounts counts;
    count_losses(*manager, WebView::ProcessType::ImageDecoder, counts);
    auto spawned = spawn_helper(*manager, WebView::ProcessType::ImageDecoder);

    bool reported = false;
    manager->on_process_exited = [&](WebView::Process&&, Optional<int>) { reported = true; };
    event_loop.quit(0);
    launcher.simulate_exit(spawned.identity.pid, 0);

    EXPECT_EQ(counts.lost, 0u);
    EXPECT_EQ(counts.exited, 0u);
    EXPECT(reported);
    EXPECT(!manager->find_child(spawned.identity.child_id).has_value());
}

TEST_CASE(a_used_up_budget_refuses_restarts_and_reports_it)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    Vector<u64> exhausted_owners;
    manager->set_restart_budget(WebView::ProcessType::Compositor, { .restarts = 2, .window = {} }, [&](u64 owner_child_id) {
        exhausted_owners.append(owner_child_id);
    });

    EXPECT(manager->may_restart(WebView::ProcessType::Compositor));
    EXPECT(manager->may_restart(WebView::ProcessType::Compositor));
    EXPECT(exhausted_owners.is_empty());

    EXPECT(!manager->may_restart(WebView::ProcessType::Compositor));
    EXPECT_EQ(exhausted_owners, Vector<u64> { 0 });
}

TEST_CASE(owners_have_budgets_of_their_own)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    manager->set_restart_budget(WebView::ProcessType::ImageDecoder, { .restarts = 1, .window = {} }, [](u64) { });

    EXPECT(manager->may_restart(WebView::ProcessType::ImageDecoder, 1));
    EXPECT(!manager->may_restart(WebView::ProcessType::ImageDecoder, 1));
    EXPECT(manager->may_restart(WebView::ProcessType::ImageDecoder, 2));
}

TEST_CASE(restarts_outside_the_window_do_not_count)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    manager->set_restart_budget(WebView::ProcessType::MediaServer, { .restarts = 1, .window = AK::Duration::zero() }, [](u64) { });

    EXPECT(manager->may_restart(WebView::ProcessType::MediaServer, 1));
    EXPECT(manager->may_restart(WebView::ProcessType::MediaServer, 1));
}

TEST_CASE(an_owner_that_exits_leaves_the_restarts_of_others)
{
    Core::EventLoop event_loop;
    auto [launcher, manager] = create_manager();
    manager->set_restart_budget(WebView::ProcessType::ImageDecoder, { .restarts = 1, .window = {} }, [](u64) { });

    Vector<WebView::SpawnedTransport> owners;
    for (size_t i = 0; i < 32; ++i) {
        owners.append(spawn_helper(*manager, WebView::ProcessType::WebContent));
        EXPECT(manager->may_restart(WebView::ProcessType::ImageDecoder, owners.last().identity.child_id));
    }

    while (!owners.is_empty()) {
        auto owner = owners.take_first();
        launcher.simulate_exit(owner.identity.pid, 0);
        for (auto const& other : owners)
            EXPECT(!manager->may_restart(WebView::ProcessType::ImageDecoder, other.identity.child_id));
    }
}
