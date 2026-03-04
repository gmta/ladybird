/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashFunctions.h>
#include <AK/ScopeGuard.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cache/CacheEntry.h>
#include <LibHTTP/Cache/CacheIndex.h>
#include <LibHTTP/Cache/DiskCache.h>
#include <LibHTTP/Cache/Utilities.h>

namespace HTTP {

ErrorOr<CacheFileHeader> CacheFileHeader::read_from_stream(Stream& stream)
{
    CacheFileHeader header;
    header.magic = TRY(stream.read_value<u32>());
    header.version = TRY(stream.read_value<u32>());
    header.cache_key = TRY(stream.read_value<u64>());
    header.vary_key = TRY(stream.read_value<u64>());
    TRY(stream.read_until_filled({ header.body_hash.data, header.body_hash.Size }));
    header.header_hash = TRY(stream.read_value<u32>());
    return header;
}

ErrorOr<void> CacheFileHeader::write_to_stream(Stream& stream) const
{
    TRY(stream.write_value(magic));
    TRY(stream.write_value(version));
    TRY(stream.write_value(cache_key));
    TRY(stream.write_value(vary_key));
    TRY(stream.write_until_depleted(body_hash.bytes()));
    TRY(stream.write_value(header_hash));
    return {};
}

u32 CacheFileHeader::hash() const
{
    u32 hash = 0;
    hash = pair_int_hash(hash, magic);
    hash = pair_int_hash(hash, version);
    hash = pair_int_hash(hash, static_cast<u32>(cache_key >> 32));
    hash = pair_int_hash(hash, static_cast<u32>(cache_key));
    hash = pair_int_hash(hash, static_cast<u32>(vary_key >> 32));
    hash = pair_int_hash(hash, static_cast<u32>(vary_key));

    using DigestType = decltype(body_hash);
    static_assert(DigestType::Size % sizeof(u32) == 0);
    auto const* body_words = reinterpret_cast<u32 const*>(body_hash.data);
    for (size_t i = 0; i < DigestType::Size / sizeof(u32); ++i)
        hash = pair_int_hash(hash, body_words[i]);

    return hash;
}

CacheFileStatus CacheFileHeader::validate() const
{
    if (magic != CACHE_MAGIC || header_hash != hash())
        return CacheFileStatus::Corrupted;
    if (version < CACHE_VERSION)
        return CacheFileStatus::VersionOlder;
    if (version > CACHE_VERSION)
        return CacheFileStatus::VersionNewer;
    return CacheFileStatus::Valid;
}

CacheEntry::CacheEntry(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, String url, Optional<LexicalPath> path)
    : m_disk_cache(disk_cache)
    , m_index(index)
    , m_cache_key(cache_key)
    , m_vary_key(vary_key)
    , m_url(move(url))
    , m_path(move(path))
{
}

void CacheEntry::remove()
{
    if (!m_path.has_value())
        return;

    (void)FileSystem::remove(m_path->string(), FileSystem::RecursionMode::Disallowed);
    m_index.remove_entry(m_cache_key, m_vary_key);
}

void CacheEntry::close_and_destroy_cache_entry()
{
    m_disk_cache.cache_entry_closed({}, *this);
}

ErrorOr<NonnullOwnPtr<CacheEntryWriter>> CacheEntryWriter::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, UnixDateTime request_time, AK::Duration current_time_offset_for_testing)
{
    return adopt_own(*new CacheEntryWriter { disk_cache, index, cache_key, move(url), request_time, current_time_offset_for_testing });
}

CacheEntryWriter::CacheEntryWriter(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, String url, UnixDateTime request_time, AK::Duration current_time_offset_for_testing)
    : CacheEntry(disk_cache, index, cache_key, 0, move(url), {})
    , m_request_time(request_time)
    , m_current_time_offset_for_testing(current_time_offset_for_testing)
{
}

ErrorOr<void> CacheEntryWriter::write_status_and_reason(u32 status_code, Optional<String> reason_phrase, HeaderList const& request_headers, HeaderList const& response_headers)
{
    if (m_marked_for_deletion) {
        close_and_destroy_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    m_response_time = UnixDateTime::now() + m_current_time_offset_for_testing;
    m_status_code = status_code;
    m_reason_phrase = move(reason_phrase);

    auto result = [&]() -> ErrorOr<void> {
        if (!is_cacheable(status_code, response_headers))
            return Error::from_string_literal("Response is not cacheable");

        m_vary_key = create_vary_key(request_headers, response_headers);
        m_path = path_for_cache_entry(m_disk_cache.cache_directory(), m_cache_key, m_vary_key);

        auto freshness_lifetime = calculate_freshness_lifetime(status_code, response_headers, m_current_time_offset_for_testing);
        auto current_age = calculate_age(response_headers, m_request_time, m_response_time, m_current_time_offset_for_testing);

        // We can cache already-expired responses if there are other cache directives that allow us to revalidate the
        // response on subsequent requests. For example, `Cache-Control: max-age=0, must-revalidate`.
        if (cache_lifetime_status(request_headers, response_headers, freshness_lifetime, current_age) == CacheLifetimeStatus::Expired)
            return Error::from_string_literal("Response has already expired");

        auto unbuffered_file = TRY(Core::File::open(m_path->string(), Core::File::OpenMode::ReadWrite));
        m_file = TRY(Core::OutputBufferedFile::create(move(unbuffered_file)));

        CacheFileHeader placeholder;
        TRY(placeholder.write_to_stream(*m_file));

        m_body_hasher = Crypto::Hash::SHA1::create();

        return {};
    }();

    if (result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to write status/reason to cache entry for\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destroy_cache_entry();

        return result.release_error();
    }

    return {};
}

ErrorOr<void> CacheEntryWriter::write_data(ReadonlyBytes data)
{
    if (m_marked_for_deletion) {
        close_and_destroy_cache_entry();
        return Error::from_string_literal("Cache entry has been deleted");
    }

    if (auto result = m_file->write_until_depleted(data); result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to write data to cache entry for\033[0m {}: {}", m_url, result.error());

        remove();
        close_and_destroy_cache_entry();

        return result.release_error();
    }

    m_body_hasher->update(data);
    m_data_size += data.size();
    return {};
}

ErrorOr<void> CacheEntryWriter::flush(NonnullRefPtr<HeaderList> request_headers, NonnullRefPtr<HeaderList> response_headers)
{
    ScopeGuard guard { [&] { close_and_destroy_cache_entry(); } };

    if (m_marked_for_deletion)
        return Error::from_string_literal("Cache entry has been deleted");

    auto result = [&]() -> ErrorOr<void> {
        CacheFileHeader header;
        header.cache_key = m_cache_key;
        header.vary_key = m_vary_key;
        header.body_hash = m_body_hasher->digest();
        header.header_hash = header.hash();

        TRY(m_file->seek(0, SeekMode::SetPosition));
        TRY(header.write_to_stream(*m_file));
        TRY(m_index.create_entry(m_cache_key, m_vary_key, m_url, move(request_headers), m_status_code, m_reason_phrase, move(response_headers), m_data_size, m_request_time, m_response_time));

        return {};
    }();

    if (result.is_error()) {
        dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mUnable to flush cache entry for\033[0m {} ({} bytes): {}", m_url, m_data_size, result.error());
        remove();
        return result.release_error();
    }

    m_disk_cache.remove_entries_exceeding_cache_limit();

    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[34;1mFinished caching\033[0m {} ({} bytes)", m_url, m_data_size);
    return {};
}

void CacheEntryWriter::remove_incomplete_entry()
{
    remove();
    close_and_destroy_cache_entry();
}

NonnullOwnPtr<CacheEntryReader> CacheEntryReader::create(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, String url, u32 status_code, Optional<String> reason_phrase, NonnullRefPtr<HeaderList> response_headers, u64 data_size)
{
    return adopt_own(*new CacheEntryReader { disk_cache, index, cache_key, vary_key, move(url), status_code, move(reason_phrase), move(response_headers), data_size });
}

CacheEntryReader::CacheEntryReader(DiskCache& disk_cache, CacheIndex& index, u64 cache_key, u64 vary_key, String url, u32 status_code, Optional<String> reason_phrase, NonnullRefPtr<HeaderList> response_headers, u64 data_size)
    : CacheEntry(disk_cache, index, cache_key, vary_key, move(url), path_for_cache_entry(disk_cache.cache_directory(), cache_key, vary_key))
    , m_status_code(status_code)
    , m_reason_phrase(move(reason_phrase))
    , m_response_headers(move(response_headers))
    , m_data_size(data_size)
{
}

ErrorOr<void> CacheEntryReader::open_file()
{
    if (m_file)
        return {};

    auto file = TRY(Core::File::open(m_path->string(), Core::File::OpenMode::Read));

    auto header = TRY(CacheFileHeader::read_from_stream(*file));

    switch (header.validate()) {
    case CacheFileStatus::Corrupted:
        return Error::from_string_literal("Cache file header is corrupted");
    case CacheFileStatus::VersionOlder:
    case CacheFileStatus::VersionNewer:
        return Error::from_string_literal("Version mismatch");
    case CacheFileStatus::Valid:
        break;
    }

    if (header.cache_key != m_cache_key)
        return Error::from_string_literal("Cache key mismatch");
    if (header.vary_key != m_vary_key)
        return Error::from_string_literal("Vary key mismatch");

    m_fd = file->fd();
    m_file = move(file);

    return {};
}

void CacheEntryReader::revalidation_succeeded(HeaderList const& response_headers)
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[34;1mCache revalidation succeeded for\033[0m {}", m_url);

    update_header_fields(m_response_headers, response_headers);
    m_index.update_response_headers(m_cache_key, m_vary_key, m_response_headers);

    if (m_revalidation_type != RevalidationType::MustRevalidate)
        close_and_destroy_cache_entry();
}

void CacheEntryReader::revalidation_failed()
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[33;1mCache revalidation failed for\033[0m {}", m_url);

    remove();
    close_and_destroy_cache_entry();
}

void CacheEntryReader::send_to(int socket_fd, Function<void(u64)> on_complete, Function<void(u64)> on_error)
{
    VERIFY(m_socket_fd == -1);
    m_socket_fd = socket_fd;

    m_on_send_complete = move(on_complete);
    m_on_send_error = move(on_error);

    if (m_marked_for_deletion) {
        send_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    if (auto result = open_file(); result.is_error()) {
        send_error(result.release_error());
        return;
    }

    m_socket_write_notifier = Core::Notifier::construct(m_socket_fd, Core::NotificationType::Write);
    m_socket_write_notifier->set_enabled(false);

    m_socket_write_notifier->on_activation = [this]() {
        m_socket_write_notifier->set_enabled(false);
        send_without_blocking();
    };

    send_without_blocking();
}

void CacheEntryReader::send_without_blocking()
{
    if (m_marked_for_deletion) {
        send_error(Error::from_string_literal("Cache entry has been deleted"));
        return;
    }

    auto result = Core::System::transfer_file_through_socket(m_fd, m_socket_fd, CacheEntry::DATA_OFFSET + m_bytes_sent, m_data_size - m_bytes_sent);

    if (result.is_error()) {
        if (result.error().code() != EAGAIN && result.error().code() != EWOULDBLOCK)
            send_error(result.release_error());
        else
            m_socket_write_notifier->set_enabled(true);

        return;
    }

    m_bytes_sent += result.value();

    if (m_bytes_sent == m_data_size) {
        send_complete();
        return;
    }

    send_without_blocking();
}

void CacheEntryReader::send_complete()
{
    m_index.update_last_access_time(m_cache_key, m_vary_key);

    if (m_on_send_complete)
        m_on_send_complete(m_bytes_sent);

    close_and_destroy_cache_entry();
}

void CacheEntryReader::send_error(Error error)
{
    dbgln_if(HTTP_DISK_CACHE_DEBUG, "\033[36m[disk]\033[0m \033[31;1mError transferring cache to socket for\033[0m {}: {}", m_url, error);

    // FIXME: We may not want to actually remove the cache file for all errors. For now, let's assume the file is not
    //        useable at this point and remove it.
    remove();

    if (m_on_send_error)
        m_on_send_error(m_bytes_sent);

    close_and_destroy_cache_entry();
}

}
