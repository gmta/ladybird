/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/ProcessManager.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <stdlib.h>
#    include <sys/file.h>
#    include <time.h>
#    include <unistd.h>
#endif

namespace WebView {

using namespace Core::CrashReportData;

static constexpr auto prepared_suffix = ".prepared"sv;
static constexpr auto ignored_suffix = ".ignored"sv;
static constexpr auto pending_prefix = "Browser-"sv;
static constexpr auto pending_suffix = ".pending"sv;
static constexpr off_t maximum_report_size = 1048576;
static constexpr size_t retained_report_count = 20;

ByteString CrashReportStore::default_directory()
{
    return LexicalPath::join(Core::StandardPaths::user_data_directory(), "Ladybird"sv, "CrashReports"sv).string();
}

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
    for (auto type : { ProcessType::Browser, ProcessType::WebContent, ProcessType::WebWorker,
             ProcessType::RequestServer, ProcessType::ImageDecoder, ProcessType::Compositor,
             ProcessType::WasmCompiler }) {
        auto prefix = ByteString::formatted("{}-", process_name_from_type(type));
        if (suffix.starts_with(prefix) && suffix.length() == prefix.length() + 10)
            return true;
    }
    return false;
}

#if !defined(AK_OS_WINDOWS)

static NeverDestroyed<ByteString> s_browser_pending_path;
static NeverDestroyed<OwnPtr<CrashReport>> s_browser_crash_report;

// The one way in. A directory that another user owns is never touched.
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
    return fstatat(directory.fd(), name.characters(), &status, AT_SYMLINK_NOFOLLOW) == 0
        && is_owned_regular_file(status);
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

static ErrorOr<ByteString> read_owned_file(Core::Directory const& directory, ByteString const& name)
{
    auto fd = openat(directory.fd(), name.characters(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return Error::from_errno(errno);
    auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::Read));
    struct stat status {};
    if (fstat(fd, &status) != 0 || !is_owned_regular_file(status) || status.st_size > maximum_report_size)
        return Error::from_string_literal("Invalid crash report file");
    return ByteString::copy(TRY(file->read_until_eof()));
}

static bool marker_exists(Core::Directory const& directory, ByteString const& name, StringView suffix)
{
    auto marker = ByteString::formatted("{}{}", name, suffix);
    struct stat status {};
    return fstatat(directory.fd(), marker.characters(), &status, AT_SYMLINK_NOFOLLOW) == 0;
}

static ErrorOr<void> create_marker(Core::Directory const& directory, ByteString const& name, StringView suffix)
{
    auto marker = ByteString::formatted("{}{}", name, suffix);
    auto fd = openat(directory.fd(), marker.characters(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0 && errno != EEXIST)
        return Error::from_errno(errno);
    if (fd >= 0)
        close(fd);
    return {};
}

static ErrorOr<void> remove_report_and_markers(Core::Directory const& directory, ByteString const& name)
{
    for (auto suffix : { ""sv, prepared_suffix, ignored_suffix }) {
        auto path = ByteString::formatted("{}{}", name, suffix);
        if (unlinkat(directory.fd(), path.characters(), 0) < 0 && errno != ENOENT)
            return Error::from_errno(errno);
    }
    return {};
}

// An empty result means no manifest was prepared. An error means one exists but cannot be trusted.
static ErrorOr<ByteString> read_prepared_manifest(Core::Directory const& directory, ByteString const& name)
{
    if (!marker_exists(directory, name, prepared_suffix))
        return ByteString {};
    auto text = TRY(read_owned_file(directory, ByteString::formatted("{}{}", name, prepared_suffix)));
    auto parsed = JsonValue::from_string(text);
    if (parsed.is_error() || !parsed.value().is_object())
        return Error::from_string_literal("Invalid prepared crash report");
    return text;
}

static ErrorOr<CrashReportStore::SavedReport> read_saved_report(Core::Directory const& directory, ByteString name)
{
    auto text = TRY(read_owned_file(directory, name));
    auto prepared = read_prepared_manifest(directory, name);
    return CrashReportStore::SavedReport {
        move(name),
        move(text),
        prepared.is_error() ? ByteString {} : prepared.release_value(),
    };
}

ErrorOr<CrashReportStore::SavedReport> CrashReportStore::saved_report(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    return read_saved_report(directory, name);
}

ErrorOr<Vector<ByteString>> CrashReportStore::pending_report_names() const
{
    // Runs on every launch, so this only stats the markers instead of reading every report.
    auto directory = TRY(open_report_directory(m_directory));

    Vector<ByteString> names;
    Core::DirIterator iterator(m_directory, Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!is_saved_report_name(name) || marker_exists(directory, name, ignored_suffix))
            continue;
        names.append(move(name));
    }
    // Names begin with the crash time, so this orders the most recent crash first.
    quick_sort(names, [](auto const& a, auto const& b) { return a > b; });
    return names;
}

bool CrashReportStore::has_pending_reports() const
{
    auto names = pending_report_names();
    return !names.is_error() && !names.value().is_empty();
}

ErrorOr<void> CrashReportStore::mark_ignored(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    return create_marker(directory, name, ignored_suffix);
}

ErrorOr<void> CrashReportStore::remove_sent_report(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    return remove_report_and_markers(directory, name);
}

ErrorOr<ByteString> CrashReportStore::prepare_submission(ByteString const& name, ByteString const& manifest) const
{
    if (!is_saved_report_name(name) || manifest.is_empty() || manifest.length() > static_cast<size_t>(maximum_report_size))
        return Error::from_string_literal("Invalid crash report submission");
    auto parsed = JsonValue::from_string(manifest);
    if (parsed.is_error() || !parsed.value().is_object())
        return Error::from_string_literal("Invalid crash report manifest");

    auto directory = TRY(open_report_directory(m_directory));
    struct stat report_status {};
    if (!is_owned_regular_file_at(directory, name, report_status))
        return Error::from_string_literal("Crash report no longer exists");

    auto existing = read_prepared_manifest(directory, name);
    if (!existing.is_error() && !existing.value().is_empty())
        return existing.release_value();

    auto path = ByteString::formatted("{}{}", name, prepared_suffix);
    if (existing.is_error()) {
        // An interrupted write cannot have been submitted: the browser only requests a challenge
        // after this file has been fully synchronized.
        (void)unlinkat(directory.fd(), path.characters(), 0);
    }

    auto fd = openat(directory.fd(), path.characters(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return Error::from_errno(errno);
    auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::Write));
    ArmedScopeGuard remove_incomplete = [&] { (void)unlinkat(directory.fd(), path.characters(), 0); };
    TRY(file->write_until_depleted(manifest.bytes()));
    if (fsync(fd) < 0)
        return Error::from_errno(errno);
    remove_incomplete.disarm();
    return manifest;
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
static void apply_retention(Core::Directory const& directory, ByteString const& path)
{
    struct RetainedReport {
        ByteString name;
        timespec modified;
    };

    Vector<RetainedReport> reports;
    Core::DirIterator iterator(path, Core::DirIterator::SkipDots);
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
        (void)remove_report_and_markers(directory, reports[i].name);
}

ErrorOr<ByteString> CrashReportStore::store_report(ProcessType process_type, StringView text, UnixDateTime crashed_at) const
{
    auto directory = TRY(open_report_directory(m_directory));

    auto seconds = static_cast<time_t>(crashed_at.truncated_seconds_since_epoch());
    tm utc_time {};
    if (!gmtime_r(&seconds, &utc_time))
        return Error::from_errno(errno);
    Array<char, 21> timestamp {};
    if (strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H-%M-%SZ", &utc_time) == 0)
        return Error::from_string_literal("Could not format crash report timestamp");

    auto pattern = ByteString::formatted("{}/{}-{}-XXXXXX.txt", m_directory, timestamp.data(),
        process_name_from_type(process_type));
    ByteString report_path;
    auto report = TRY(create_temporary_file(pattern, 4, report_path));

    ArmedScopeGuard remove_incomplete_report = [&] { (void)Core::System::unlink(report_path); };
    TRY(report->write_until_depleted(text.bytes()));
    if (fsync(report->fd()) < 0)
        return Error::from_errno(errno);
    remove_incomplete_report.disarm();

    apply_retention(directory, m_directory);
    return LexicalPath::basename(report_path);
}

ErrorOr<size_t> CrashReportStore::recover_pending_reports() const
{
    auto directory = TRY(open_report_directory(m_directory));
    size_t recovered_count = 0;

    Core::DirIterator iterator(m_directory, Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!name.starts_with(pending_prefix) || !name.ends_with(pending_suffix))
            continue;

        auto fd = openat(directory.fd(), name.characters(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0)
            continue;
        auto file = TRY(Core::File::adopt_fd(fd, Core::File::OpenMode::ReadWrite));

        // A running browser holds this lock for its lifetime, so taking it means the owner is gone.
        // Process IDs are recycled, which makes them an unreliable way to tell.
        if (flock(fd, LOCK_EX | LOCK_NB) != 0)
            continue;

        struct stat status {};
        if (fstat(fd, &status) != 0 || !is_owned_regular_file(status))
            continue;

        ReportHeader header {};
        if (pread(fd, &header, sizeof(header), 0) != sizeof(header) || header.magic != report_magic
            || !header.signal) {
            (void)unlinkat(directory.fd(), name.characters(), 0);
            continue;
        }

        // The file was last written as the browser died, so this is when the crash happened, which
        // can be much earlier than the launch that recovers it.
        auto crashed_at = UnixDateTime::from_unix_timespec(modified_time(status));

        CrashReport recovered(move(file), ProcessType::Browser);
        if (auto result = recovered.save(header.signal, m_directory, crashed_at); result.is_error()) {
            warnln("Could not recover Browser crash report: {}", result.error());
            continue;
        }
        (void)unlinkat(directory.fd(), name.characters(), 0);
        ++recovered_count;
    }
    return recovered_count;
}

static void remove_clean_browser_report()
{
    if (!s_browser_pending_path->is_empty())
        (void)unlink(s_browser_pending_path->characters());
}

ErrorOr<void> CrashReportStore::initialize_browser_crash_handler()
{
    TRY(open_report_directory(m_directory));

    // A browser cannot format its own fatal signal, so earlier ones are recovered before this
    // process installs a handler of its own. One unreadable file must not prevent that.
    if (auto result = recover_pending_reports(); result.is_error())
        warnln("Could not recover browser crash reports: {}", result.error());

    auto pattern = ByteString::formatted("{}/{}XXXXXX{}", m_directory, pending_prefix, pending_suffix);
    auto file = TRY(create_temporary_file(pattern, static_cast<int>(pending_suffix.length()),
        *s_browser_pending_path));

    // Held until this process exits. Recovery takes it to tell a crashed browser from a live one.
    auto fd = file->fd();
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        return Error::from_errno(errno);

    *s_browser_crash_report = make<CrashReport>(move(file), ProcessType::Browser);
    TRY(Core::CrashHandler::initialize(fd));
    if (atexit(remove_clean_browser_report) != 0)
        return Error::from_string_literal("Could not register browser crash report cleanup");
    return {};
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    TRY(open_report_directory(m_directory));
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

ErrorOr<CrashReportStore::SavedReport> CrashReportStore::saved_report(ByteString const&) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<Vector<ByteString>> CrashReportStore::pending_report_names() const { return Vector<ByteString> {}; }
bool CrashReportStore::has_pending_reports() const { return false; }
ErrorOr<void> CrashReportStore::mark_ignored(ByteString const&) const { return {}; }
ErrorOr<void> CrashReportStore::remove_sent_report(ByteString const&) const { return {}; }

ErrorOr<ByteString> CrashReportStore::prepare_submission(ByteString const&, ByteString const& manifest) const
{
    return manifest;
}

ErrorOr<ByteString> CrashReportStore::store_report(ProcessType, StringView, UnixDateTime) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<size_t> CrashReportStore::recover_pending_reports() const { return 0; }

ErrorOr<void> CrashReportStore::initialize_browser_crash_handler()
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

#endif

}
