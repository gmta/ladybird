/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/ProcessManager.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <time.h>
#    include <unistd.h>
#endif

namespace WebView {

static constexpr size_t retained_report_count = 20;

ByteString CrashReportStore::default_directory()
{
    return LexicalPath::join(Core::StandardPaths::user_data_directory(), "Ladybird"sv, "CrashReports"sv).string();
}

// The browser's own store lives under the user data directory.
CrashReportStore& CrashReportStore::the()
{
    static NeverDestroyed<CrashReportStore> store { default_directory() };
    return *store;
}

bool CrashReportStore::is_saved_report_name(StringView name)
{
    constexpr auto timestamp_pattern = "####-##-##T##-##-##Z-"sv;
    if (!name.ends_with(".txt"sv))
        return false;
    auto suffix = name;
    if (name.length() > timestamp_pattern.length()) {
        bool has_timestamp = true;
        for (size_t i = 0; i < timestamp_pattern.length(); ++i) {
            if (timestamp_pattern[i] == '#' ? !is_ascii_digit(name[i]) : name[i] != timestamp_pattern[i]) {
                has_timestamp = false;
                break;
            }
        }
        if (has_timestamp)
            suffix = name.substring_view(timestamp_pattern.length());
    }
    for (auto type : { ProcessType::WebContent, ProcessType::WebWorker, ProcessType::RequestServer,
             ProcessType::ImageDecoder, ProcessType::Compositor, ProcessType::WasmCompiler }) {
        auto prefix = ByteString::formatted("{}-", process_name_from_type(type));
        if (suffix.starts_with(prefix) && suffix.length() == prefix.length() + 10)
            return true;
    }
    return false;
}

#if !defined(AK_OS_WINDOWS)

static ErrorOr<Core::Directory> open_report_directory(ByteString const& path)
{
    auto directory = TRY(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes, 0700));
    auto status = TRY(directory.stat());
    if (status.st_uid != getuid())
        return Error::from_string_literal("Crash report directory is not owned by the current user");
    TRY(Core::System::fchmod(directory.fd(), 0700));
    return directory;
}

static bool is_owned_regular_file(struct stat const& status)
{
    return S_ISREG(status.st_mode) && status.st_uid == getuid();
}

static bool is_owned_regular_file_at(Core::Directory const& directory, ByteString const& name, struct stat& status)
{
    auto result = Core::System::fstatat(directory.fd(), name, AT_SYMLINK_NOFOLLOW);
    if (result.is_error())
        return false;
    status = result.release_value();
    return is_owned_regular_file(status);
}

// mkstemps needs a mutable, null-terminated pattern, and reports the name it settled on through it.
static ErrorOr<NonnullOwnPtr<Core::File>> create_temporary_file(ByteString const& pattern, int suffix_length, ByteString& path)
{
    auto pattern_buffer = TRY(ByteBuffer::create_zeroed(pattern.length() + 1));
    pattern.bytes().copy_to(pattern_buffer);
    auto fd = TRY(Core::System::mkstemps({ reinterpret_cast<char*>(pattern_buffer.data()), pattern_buffer.size() },
        suffix_length));
    path = ByteString { pattern_buffer.bytes().trim(pattern.length()) };
    return Core::File::adopt_fd(fd, Core::File::OpenMode::ReadWrite);
}

static timespec modified_time(struct stat const& status)
{
#    if defined(AK_OS_MACOS)
    return status.st_mtimespec;
#    else
    return status.st_mtim;
#    endif
}

// Bound disk use. Unrelated files and symlinks in this directory are ignored.
static void apply_retention(Core::Directory const& directory)
{
    struct RetainedReport {
        ByteString name;
        timespec modified;
    };

    Vector<RetainedReport> reports;
    Core::DirIterator iterator(directory.path().string(), Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!CrashReportStore::is_saved_report_name(name))
            continue;
        struct stat status {};
        if (!is_owned_regular_file_at(directory, name, status))
            continue;
        reports.append({ move(name), modified_time(status) });
    }

    quick_sort(reports, [](auto const& a, auto const& b) {
        if (a.modified.tv_sec != b.modified.tv_sec)
            return a.modified.tv_sec < b.modified.tv_sec;
        return a.modified.tv_nsec < b.modified.tv_nsec;
    });
    for (size_t i = 0; i + retained_report_count < reports.size(); ++i)
        (void)unlinkat(directory.fd(), reports[i].name.characters(), 0);
}

// Writes report text under a name derived from the current time, then drops the oldest reports
// beyond the retention limit.
ErrorOr<void> CrashReportStore::store_report(ProcessType process_type, StringView text) const
{
    auto directory = TRY(open_report_directory(m_directory));

    auto seconds = time(nullptr);
    tm utc_time {};
    if (!gmtime_r(&seconds, &utc_time))
        return Error::from_errno(errno);
    Array<char, 21> timestamp {};
    if (strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H-%M-%SZ", &utc_time) == 0)
        return Error::from_string_literal("Could not format crash report timestamp");

    auto filename_pattern = ByteString::formatted("{}-{}-XXXXXX.txt", timestamp.data(),
        process_name_from_type(process_type));
    auto pattern = LexicalPath::join(m_directory, filename_pattern).string();
    ByteString report_path;
    auto report = TRY(create_temporary_file(pattern, 4, report_path));

    ArmedScopeGuard remove_incomplete_report = [&] { (void)Core::System::unlink(report_path); };
    TRY(report->write_until_depleted(text.bytes()));
    if (fsync(report->fd()) < 0)
        return Error::from_errno(errno);
    remove_incomplete_report.disarm();

    apply_retention(directory);
    return {};
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    TRY(Core::Directory::create(m_directory, Core::Directory::CreateDirectories::Yes, 0700));
    Vector<ByteString> arguments { m_directory };
#    if defined(AK_OS_MACOS)
    TRY(Core::Process::spawn("/usr/bin/open"sv, arguments));
#    else
    TRY(Core::Process::spawn({
        .executable = "xdg-open"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    }));
#    endif
    return {};
}

#else

ErrorOr<void> CrashReportStore::store_report(ProcessType, StringView) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

#endif

}
