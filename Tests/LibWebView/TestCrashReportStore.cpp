/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportStore.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static ByteString test_directory()
{
    return ByteString::formatted("{}/test-crash-report-store-{}", Core::StandardPaths::tempfile_directory(), getpid());
}

static WebView::CrashReportStore test_store()
{
    return WebView::CrashReportStore { test_directory() };
}

static void cleanup()
{
    if (FileSystem::exists(test_directory()))
        MUST(FileSystem::remove(test_directory(), FileSystem::RecursionMode::Allowed));
}

static Vector<ByteString> directory_entries()
{
    Vector<ByteString> names;
    Core::DirIterator iterator(test_directory(), Core::DirIterator::SkipDots);
    while (iterator.has_next())
        names.append(iterator.next_path());
    return names;
}

TEST_CASE(a_stored_report_is_named_after_the_time_of_the_crash)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto crashed_at = UnixDateTime::from_seconds_since_epoch(1772000767);
    auto name = MUST(test_store().store_report(WebView::ProcessType::WebContent, "A report\n"sv, crashed_at));
    EXPECT(name.starts_with("2026-02-25T06-26-07Z-WebContent-"sv));
    EXPECT(name.ends_with(".txt"sv));
    EXPECT(WebView::CrashReportStore::is_saved_report_name(name));

    auto path = ByteString::formatted("{}/{}", test_directory(), name);
    EXPECT_EQ(ByteString::copy(MUST(MUST(Core::File::open(path, Core::File::OpenMode::Read))->read_until_eof())),
        "A report\n"sv);
}

TEST_CASE(unrelated_files_are_not_reports)
{
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("notes.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-Firefox-abc123.txt"sv));
    EXPECT(!WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-abc123.log"sv));
    EXPECT(WebView::CrashReportStore::is_saved_report_name("2026-03-04T05-06-07Z-WebContent-abc123.txt"sv));
}

TEST_CASE(retention_drops_the_oldest_reports)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto store = test_store();
    Vector<ByteString> names;
    for (u32 i = 0; i < 20; ++i) {
        auto crashed_at = UnixDateTime::from_seconds_since_epoch(1772000000 + i);
        auto name = MUST(store.store_report(WebView::ProcessType::WebContent, "A report\n"sv, crashed_at));
        timespec times[2] { { 1000 + i, 0 }, { 1000 + i, 0 } };
        auto path = ByteString::formatted("{}/{}", test_directory(), name);
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
        VERIFY(futimens(file->fd(), times) == 0);
        names.append(move(name));
    }

    // The directory is now at the retention limit, so storing one more has to evict the oldest.
    auto crashed_at = UnixDateTime::from_seconds_since_epoch(1772000100);
    MUST(store.store_report(WebView::ProcessType::WebContent, "One more\n"sv, crashed_at));

    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 20u);
    EXPECT(!entries.contains_slow(names[0]));
    EXPECT(entries.contains_slow(names[1]));
}

static ByteString write_pending_report(int signal, time_t modified)
{
    Core::CrashReportData::ReportHeader header {};
    header.magic = Core::CrashReportData::report_magic;
    header.signal = signal;

    auto path = ByteString::formatted("{}/Browser-abc123.pending", test_directory());
    MUST(Core::Directory::create(test_directory(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted({ &header, sizeof(header) }));

    timespec times[2] { { modified, 0 }, { modified, 0 } };
    VERIFY(futimens(file->fd(), times) == 0);
    return path;
}

TEST_CASE(a_recovered_browser_crash_keeps_the_time_it_crashed)
{
    cleanup();
    ScopeGuard guard = cleanup;

    write_pending_report(SIGSEGV, 1772000767);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);

    auto entries = directory_entries();
    EXPECT_EQ(entries.size(), 1u);
    // Not the time of this launch, which is what a report named after "now" would record.
    EXPECT(entries[0].starts_with("2026-02-25T06-26-07Z-Browser-"sv));

    auto path = ByteString::formatted("{}/{}", test_directory(), entries[0]);
    auto text = ByteString::copy(MUST(MUST(Core::File::open(path, Core::File::OpenMode::Read))->read_until_eof()));
    EXPECT(text.contains("Termination signal name: SIGSEGV"sv));
}

TEST_CASE(a_pending_report_held_by_a_running_browser_is_left_alone)
{
    cleanup();
    ScopeGuard guard = cleanup;

    auto path = write_pending_report(SIGSEGV, 1772000767);
    auto held = MUST(Core::File::open(path, Core::File::OpenMode::Read));

    // A live browser holds this lock for its lifetime. Process IDs get recycled, so they could not
    // tell a running browser from an unrelated process that inherited its number.
    VERIFY(flock(held->fd(), LOCK_EX | LOCK_NB) == 0);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 0u);

    held->close();
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 1u);
}

TEST_CASE(a_pending_report_without_a_captured_signal_is_discarded)
{
    cleanup();
    ScopeGuard guard = cleanup;

    // A clean exit leaves no signal behind, and must not be reported as a crash.
    write_pending_report(0, 1772000767);
    EXPECT_EQ(MUST(test_store().recover_pending_reports()), 0u);
    EXPECT(directory_entries().is_empty());
}
