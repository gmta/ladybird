/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Array.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>

#include <fcntl.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace WebView {

Process::Process(ProcessType type, Core::Process process, ProcessOutputCapture output_capture, IPC::ConnectionBase* connection)
    : m_pid(process.pid())
    , m_process(move(process))
    , m_type(type)
    , m_connection(connection)
    , m_output_capture(move(output_capture))
{
}

Process::Process(ProcessType type, pid_t pid)
    : m_pid(pid)
    , m_type(type)
{
}

Process Process::for_browser()
{
    return Process { ProcessType::Browser, Core::System::getpid() };
}

void Process::did_register(Badge<ProcessManager>, u64 child_id)
{
    m_child_id = child_id;
}

void Process::save_crash_report(Optional<int> exit_status)
{
    if (m_crash_report && exit_status.has_value()) {
        if (auto result = m_crash_report->save(*exit_status); result.is_error())
            warnln("Could not save {} crash report: {}", process_name_from_type(m_type), result.error());
        else
            m_saved_crash_report_name = m_crash_report->saved_name();
    }
    m_crash_report = nullptr;
}

Process::~Process()
{
    // shutdown() can release the connection's last owner while dispatching die().
    if (auto connection = m_connection.strong_ref())
        connection->shutdown();
}

Vector<ByteString> Process::helper_process_environment(ProcessType type)
{
    static constexpr Array allowed_names {
        "HOME"sv,
        "LANG"sv,
        "LANGUAGE"sv,
        "LLVM_PROFILE_FILE"sv,
        "LOGNAME"sv,
        "PATH"sv,
        "TMPDIR"sv,
        "TZ"sv,
        "USER"sv,
        "XDG_CACHE_HOME"sv,
        "XDG_CONFIG_DIRS"sv,
        "XDG_CONFIG_HOME"sv,
        "XDG_DATA_DIRS"sv,
        "XDG_DATA_HOME"sv,
        "XDG_RUNTIME_DIR"sv,
        "FONTCONFIG_FILE"sv,
        "FONTCONFIG_PATH"sv,
        "__CF_USER_TEXT_ENCODING"sv,
    };
    static constexpr Array allowed_prefixes {
        "ASAN_"sv,
        "LADYBIRD_"sv,
        "LC_"sv,
        "LIBGC_"sv,
        "LIBJS_"sv,
        "LIBWASM_"sv,
        "LIBWEB_"sv,
        "LSAN_"sv,
        "RUST_"sv,
        "TSAN_"sv,
        "UBSAN_"sv,
    };
    // OpenSSL reads these to find the system's certificates. OPENSSL_CONF is not here on purpose: a configuration file
    // can load providers and engines into the process.
    static constexpr Array certificate_names {
        "SSL_CERT_DIR"sv,
        "SSL_CERT_FILE"sv,
    };
    // libpulse reads these to find the audio server, and the Browser uses them to decide which server the audio
    // clients may connect to.
    static constexpr Array audio_names {
        "PULSE_RUNTIME_PATH"sv,
        "PULSE_SERVER"sv,
    };
    // Mesa and the Vulkan loader read these to select and configure the GPU driver.
    static constexpr Array gpu_prefixes {
        "DRI_PRIME"sv,
        "MESA_"sv,
        "VK_"sv,
        "__EGL_"sv,
        "__GLX_"sv,
    };

    Vector<ByteString> environment;
    for (auto** variable = Core::Environment::raw_environ(); *variable; ++variable) {
        auto entry = Core::Environment::Entry::from_chars(*variable);
        auto is_allowed = allowed_names.contains_slow(entry.name) || any_of(allowed_prefixes, [&](auto prefix) { return entry.name.starts_with(prefix); });
        if (type == ProcessType::RequestServer) {
            is_allowed |= certificate_names.contains_slow(entry.name);
        }
        if (type == ProcessType::AudioServer)
            is_allowed |= audio_names.contains_slow(entry.name);
        if (type == ProcessType::Compositor || type == ProcessType::VideoDecoderServer)
            is_allowed |= any_of(gpu_prefixes, [&](auto prefix) { return entry.name.starts_with(prefix); });
        if (is_allowed)
            environment.append(entry.full_entry);
    }
    return environment;
}

ErrorOr<Optional<pid_t>> Process::get_process_pid(StringView process_name, StringView pid_path)
{
    if (!FileSystem::exists(pid_path))
        return OptionalNone {};

    Optional<pid_t> pid;
    {
        auto pid_file = Core::File::open(pid_path, Core::File::OpenMode::Read);
        if (pid_file.is_error()) {
            warnln("Could not open {} PID file '{}': {}", process_name, pid_path, pid_file.error());
            return pid_file.release_error();
        }

        auto contents = pid_file.value()->read_until_eof();
        if (contents.is_error()) {
            warnln("Could not read {} PID file '{}': {}", process_name, pid_path, contents.error());
            return contents.release_error();
        }

        pid = StringView { contents.value() }.to_number<pid_t>();
    }

    if (!pid.has_value()) {
        warnln("{} PID file '{}' exists, but with an invalid PID", process_name, pid_path);
        TRY(FileSystem::remove(pid_path, FileSystem::RecursionMode::Disallowed));
        return OptionalNone {};
    }

    bool const process_not_found = [&pid]() {
#if defined(AK_OS_WINDOWS)
        HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
        if (process_handle == nullptr)
            return true;

        // FIXME: We should create an RAII wrapper around HANDLE objects.
        ScopeGuard handle_guard = [&process_handle] { CloseHandle(process_handle); };
        DWORD exit_code = 0;

        if (GetExitCodeProcess(process_handle, &exit_code) == 0)
            return true;

        return exit_code != STILL_ACTIVE;
#else
        return kill(*pid, 0) < 0;
#endif
    }();

    if (process_not_found) {
        warnln("{} PID file '{}' exists with PID {}, but process cannot be found", process_name, pid_path, *pid);
        TRY(FileSystem::remove(pid_path, FileSystem::RecursionMode::Disallowed));
        return OptionalNone {};
    }

    return pid;
}

// This is heavily based on how SystemServer's Service creates its socket.
ErrorOr<int> Process::create_ipc_socket(ByteString const& socket_path)
{
    auto socket_address_un = Core::SocketAddress::local(socket_path).to_sockaddr_un();
    if (!socket_address_un.has_value())
        return Error::from_string_literal("IPC socket path does not fit in sockaddr_un; the runtime directory path is too long");

    if (FileSystem::exists(socket_path))
        TRY(FileSystem::remove(socket_path, FileSystem::RecursionMode::Disallowed));

#if defined(AK_OS_WINDOWS)
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    TRY(Core::System::set_socket_blocking(socket_fd, false));
    if (SetHandleInformation(to_handle(socket_fd), HANDLE_FLAG_INHERIT, 0) == 0)
        return Error::from_windows_error();
#else
#    ifdef SOCK_NONBLOCK
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#    else
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));

    int option = 1;
    TRY(Core::System::ioctl(socket_fd, FIONBIO, &option));
    TRY(Core::System::fcntl(socket_fd, F_SETFD, FD_CLOEXEC));
#    endif

#    if !defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_GNU_HURD)
    TRY(Core::System::fchmod(socket_fd, 0600));
#    endif
#endif

    TRY(Core::System::bind(socket_fd, reinterpret_cast<sockaddr*>(&*socket_address_un), sizeof(*socket_address_un)));
    TRY(Core::System::listen(socket_fd, 16));

    return socket_fd;
}

ErrorOr<Process::ProcessPaths> Process::paths_for_process(StringView process_name, StringView runtime_directory)
{
    auto socket_path = ByteString::formatted("{}/{}.socket", runtime_directory, process_name);
    auto pid_path = ByteString::formatted("{}/{}.pid", runtime_directory, process_name);

    return ProcessPaths { move(socket_path), move(pid_path) };
}

}
