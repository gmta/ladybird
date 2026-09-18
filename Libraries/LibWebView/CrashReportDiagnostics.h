/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The parsed form of a saved crash report's text.
//
// One parser feeds both the review page and the submission manifest, so the report a user reviews is
// the report the server receives.
struct WEBVIEW_API CrashReportDiagnostics {
    struct Header {
        ByteString name;
        ByteString value;
    };

    struct Frame {
        // Empty when the frame could not be split, in which case the whole line is the symbol.
        ByteString binary;
        ByteString address;
        ByteString symbol;
    };

    struct Signal {
        ByteString name;
        Optional<u64> number;
    };

    static CrashReportDiagnostics parse(StringView text);

    // The first value under this name, or an empty string when the report has no such line.
    ByteString header(StringView name) const;

    // Named lines above the native stack, in the order the report lists them. A signal that an older
    // build wrote as one "SIGSEGV (11)" line appears here as separate name and number entries.
    Vector<Header> headers;

    // Frames of the native stack, and the section they came from with its heading kept and its
    // trailing note about partial stacks removed.
    Vector<Frame> frames;
    ByteString stack;

    Signal signal;
};

}
