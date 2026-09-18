/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibRequests/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Owns one crash report upload from start to finish: building the manifest, persisting it so a retry
// reuses the same submission ID, solving the server's proof-of-work challenge off the main thread,
// uploading, and removing the local files once the server acknowledges a receipt. The review page
// chooses what to send and renders progress; it never sees the wire format.
class WEBVIEW_API CrashReportSubmission
    : public RefCounted<CrashReportSubmission>
    , public Weakable<CrashReportSubmission> {
public:
    enum class Stage : u8 {
        Preparing,
        RequestingChallenge,
        SolvingProof,
        Sending,
    };
    static StringView stage_name(Stage);

    // The first attempt plus the retries the page announces while it counts down.
    static constexpr u32 maximum_attempts = 6;

    struct Options {
        ByteString report_name;
        ByteString description;
        ByteString url;
        ByteString reviewed_text_digest;
        ByteString reviewed_manifest_digest;
    };

    // What an earlier attempt already committed to sending, so the page can show it and lock it.
    struct PreparedChoices {
        ByteString description;
        ByteString url;
    };
    static Optional<PreparedChoices> prepared_choices(StringView manifest);

    static NonnullRefPtr<CrashReportSubmission> create(Options);
    ~CrashReportSubmission();

    void start();

    Function<void(Stage)> on_progress;
    Function<void(ByteString manifest_digest)> on_prepared;
    Function<void(String reason, u32 retry_number)> on_retry;
    Function<void()> on_sent;
    Function<void(String reason)> on_failed;

private:
    struct ProofOfWork {
        ByteString token;
        u64 nonce { 0 };
    };

    explicit CrashReportSubmission(Options);

    void begin_attempt();
    ErrorOr<ByteString> prepare_manifest();
    void request_challenge(ByteString manifest);
    void solve_proof(ByteString manifest, ByteString token, u64 expected_work);
    void send_report(ByteString manifest, ProofOfWork);
    void finish();
    void fail(String reason);

    void post(StringView path, StringView content_type, ByteString body, Optional<ProofOfWork>,
        Function<void(u32 status, ByteString body)> on_response);

    Options m_options;
    ByteString m_report_text;
    ByteString m_manifest;
    u32 m_attempt { 1 };

    RefPtr<Requests::Request> m_request;
    RefPtr<Core::Timer> m_request_timeout;
    RefPtr<Core::Timer> m_retry_timer;
};

}
