/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportStore.h>
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
