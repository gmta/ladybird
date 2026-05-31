/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API PageSnapshotDump {
    String name;
    ByteBuffer data;
};

struct WEBVIEW_API PageSnapshotFrame {
    u64 index { 0 };
    Optional<u64> parent_index;
    String url;
    String base_url;
    String title;
    Vector<PageSnapshotDump> dumps;
};

struct WEBVIEW_API PageSnapshot {
    Vector<PageSnapshotFrame> frames;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::PageSnapshotDump const&);

template<>
WEBVIEW_API ErrorOr<WebView::PageSnapshotDump> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::PageSnapshotFrame const&);

template<>
WEBVIEW_API ErrorOr<WebView::PageSnapshotFrame> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::PageSnapshot const&);

template<>
WEBVIEW_API ErrorOr<WebView::PageSnapshot> decode(Decoder&);

}
