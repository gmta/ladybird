/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/PageSnapshot.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::PageSnapshotDump const& dump)
{
    TRY(encoder.encode(dump.name));
    TRY(encoder.encode(dump.data));
    return {};
}

template<>
ErrorOr<WebView::PageSnapshotDump> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto data = TRY(decoder.decode<ByteBuffer>());
    return WebView::PageSnapshotDump { move(name), move(data) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::PageSnapshotFrame const& frame)
{
    TRY(encoder.encode(frame.index));
    TRY(encoder.encode(frame.parent_index));
    TRY(encoder.encode(frame.url));
    TRY(encoder.encode(frame.base_url));
    TRY(encoder.encode(frame.title));
    TRY(encoder.encode(frame.dumps));
    return {};
}

template<>
ErrorOr<WebView::PageSnapshotFrame> IPC::decode(Decoder& decoder)
{
    auto index = TRY(decoder.decode<u64>());
    auto parent_index = TRY(decoder.decode<Optional<u64>>());
    auto url = TRY(decoder.decode<String>());
    auto base_url = TRY(decoder.decode<String>());
    auto title = TRY(decoder.decode<String>());
    auto dumps = TRY(decoder.decode<Vector<WebView::PageSnapshotDump>>());
    return WebView::PageSnapshotFrame { index, parent_index, move(url), move(base_url), move(title), move(dumps) };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::PageSnapshot const& snapshot)
{
    TRY(encoder.encode(snapshot.frames));
    return {};
}

template<>
ErrorOr<WebView::PageSnapshot> IPC::decode(Decoder& decoder)
{
    auto frames = TRY(decoder.decode<Vector<WebView::PageSnapshotFrame>>());
    return WebView::PageSnapshot { move(frames) };
}
