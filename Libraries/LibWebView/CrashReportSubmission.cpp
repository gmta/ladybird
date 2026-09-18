/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NumericLimits.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibThreading/ThreadPool.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/CrashReportSubmission.h>

namespace WebView {

static constexpr auto default_endpoint = "https://reports.app.ladybird.org"sv;
static constexpr auto challenge_path = "/api/v1/challenges"sv;
static constexpr auto report_path = "/api/v1/reports"sv;
static constexpr auto challenge_algorithm = "sha256-seed-nonce-le-v1"sv;
static constexpr auto diagnostics_name = "crash-diagnostics.txt"sv;
static constexpr size_t maximum_challenge_token_length = 4096;
static constexpr u64 maximum_expected_work = 100'000'000;
static constexpr int request_timeout_ms = 30'000;
static constexpr int retry_delay_ms = 3'000;
static constexpr int maximum_retry_delay_ms = 60'000;

static String to_string(StringView text)
{
    return String::from_utf8_with_replacement_character(text);
}

static ByteString uuid_v7()
{
    Array<u8, 16> bytes {};
    fill_with_random(bytes);
    auto milliseconds = static_cast<u64>(UnixDateTime::now().milliseconds_since_epoch());
    for (size_t i = 0; i < 6; ++i)
        bytes[5 - i] = static_cast<u8>(milliseconds >> (i * 8));
    bytes[6] = (bytes[6] & 0x0f) | 0x70;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    StringBuilder builder;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            builder.append('-');
        builder.appendff("{:02x}", bytes[i]);
    }
    return builder.to_byte_string();
}

static Optional<JsonObject> parse_json_object(StringView text)
{
    auto parsed = JsonValue::from_string(text);
    if (parsed.is_error() || !parsed.value().is_object())
        return {};
    return parsed.value().as_object();
}

static ByteString sha256_hex(ReadonlyBytes bytes)
{
    return encode_hex(Crypto::Hash::SHA256::hash(bytes).bytes());
}

static ByteString build_manifest(CrashReportSubmission::Options const& options, StringView text)
{
    auto diagnostics = CrashReportDiagnostics::parse(text);

    JsonArray fields;
    auto add = [&fields](StringView key, StringView type, JsonValue value) {
        JsonObject typed;
        typed.set("type"sv, to_string(type));
        typed.set("value"sv, move(value));
        JsonObject field;
        field.set("key"sv, to_string(key));
        field.set("value"sv, move(typed));
        fields.must_append(move(field));
    };
    auto add_text = [&add](StringView key, StringView type, StringView value) {
        if (!value.is_empty())
            add(key, type, to_string(value));
    };

    add_text("stack"sv, "stack_trace"sv, diagnostics.stack);
    add_text("process"sv, "text"sv, diagnostics.header("Process"sv));
    add_text("platform"sv, "text"sv, diagnostics.header("Platform"sv));
    add_text("architecture"sv, "text"sv, diagnostics.header("Architecture"sv));
    add_text("git_commit"sv, "text"sv, diagnostics.header("Git commit"sv));
    add_text("build_configuration"sv, "text"sv, diagnostics.header("Build configuration"sv));
    add_text("cpp_compiler"sv, "text"sv, diagnostics.header("C++ compiler"sv));
    for (auto label : { "Verification failed"sv, "Assertion failed"sv, "Rust panic"sv }) {
        auto value = diagnostics.header(label);
        if (value.is_empty())
            continue;
        add_text("failure_reason"sv, "text"sv, ByteString::formatted("{}: {}", label, value));
        break;
    }
    add_text("signal"sv, "text"sv, diagnostics.signal.name);
    if (diagnostics.signal.number.has_value())
        add("signal_number"sv, "number"sv, *diagnostics.signal.number);
    add_text("description"sv, "multiline"sv, options.description);
    add_text("url"sv, "text"sv, options.url);

    JsonObject attachment;
    attachment.set("id"sv, to_string(uuid_v7()));
    attachment.set("name"sv, to_string(diagnostics_name));
    attachment.set("media_type"sv, "text/plain"_string);
    attachment.set("size"sv, text.length());
    attachment.set("sha256"sv, to_string(sha256_hex(text.bytes())));
    JsonArray attachments;
    attachments.must_append(move(attachment));

    auto version = diagnostics.header("Version"sv);
    auto build = diagnostics.header("Build configuration"sv);

    JsonObject manifest;
    manifest.set("protocol"sv, 1);
    manifest.set("submission_id"sv, to_string(uuid_v7()));
    manifest.set("kind"sv, "crash"_string);
    manifest.set("client_version"sv, version.is_empty() ? "Unknown Ladybird version"_string : to_string(version));
    manifest.set("build"sv, build.is_empty() ? "Unknown build"_string : to_string(build));
    manifest.set("fields"sv, move(fields));
    manifest.set("attachments"sv, move(attachments));
    return manifest.serialized().to_byte_string();
}

static ByteString diagnostics_attachment_id(StringView manifest)
{
    auto object = parse_json_object(manifest);
    if (!object.has_value())
        return {};
    auto attachments = object->get_array("attachments"sv);
    if (!attachments.has_value())
        return {};
    for (auto const& value : attachments->values()) {
        if (!value.is_object())
            continue;
        auto name = value.as_object().get_string("name"sv);
        auto id = value.as_object().get_string("id"sv);
        if (name.has_value() && *name == diagnostics_name && id.has_value())
            return id->to_byte_string();
    }
    return {};
}

static ByteString build_multipart_body(StringView manifest, StringView report_text, StringView boundary)
{
    StringBuilder builder;
    auto append_part = [&](StringView disposition, StringView content_type, StringView content) {
        builder.appendff("--{}\r\nContent-Disposition: form-data; {}\r\nContent-Type: {}\r\n\r\n{}\r\n",
            boundary, disposition, content_type, content);
    };

    append_part("name=\"manifest\""sv, "application/json"sv, manifest);
    if (auto id = diagnostics_attachment_id(manifest); !id.is_empty()) {
        append_part(ByteString::formatted("name=\"{}\"; filename=\"{}\"", id, diagnostics_name),
            "text/plain; charset=utf-8"sv, report_text);
    }

    builder.appendff("--{}--\r\n", boundary);
    return builder.to_byte_string();
}

static Optional<u64> solve_challenge(ByteString const& token, u64 expected_work)
{
    auto seed = Crypto::Hash::SHA256::hash(token.bytes());
    Array<u8, 40> candidate {};
    seed.bytes().copy_to(candidate.span().slice(0, 32));
    auto threshold = NumericLimits<u64>::max() / expected_work;

    for (u64 nonce = 0; nonce < NumericLimits<u64>::max(); ++nonce) {
        for (size_t i = 0; i < 8; ++i)
            candidate[32 + i] = static_cast<u8>(nonce >> (i * 8));
        auto digest = Crypto::Hash::SHA256::hash(candidate.span());
        u64 prefix = 0;
        for (size_t i = 0; i < 8; ++i)
            prefix = prefix << 8 | digest.bytes()[i];
        if (prefix <= threshold)
            return nonce;
    }
    return {};
}

// The server would reject anything else again, so only these are worth another attempt. A conflict means the challenge
// expired or was already used, which the fresh challenge of the next attempt resolves.
static bool is_transient_status(u32 status)
{
    return status == 408 || status == 409 || status == 429 || (status >= 500 && status < 600);
}

// Only a delay in seconds is honored. A date falls back to the regular delay between attempts.
static Optional<int> retry_after_ms(HTTP::HeaderList const& headers)
{
    auto value = headers.get("Retry-After"sv);
    if (!value.has_value())
        return {};
    auto seconds = value->view().trim_whitespace().to_number<u32>();
    if (!seconds.has_value())
        return {};
    return static_cast<int>(min<u64>(*seconds * 1000ull, maximum_retry_delay_ms));
}

StringView CrashReportSubmission::stage_name(Stage stage)
{
    switch (stage) {
    case Stage::Preparing:
        return "preparing"sv;
    case Stage::RequestingChallenge:
        return "challenge"sv;
    case Stage::SolvingProof:
        return "proof"sv;
    case Stage::Sending:
        return "sending"sv;
    }
    VERIFY_NOT_REACHED();
}

// Return what an earlier attempt committed to sending so the page can display it without allowing
// the choices to change between retries.
Optional<CrashReportSubmission::PreparedChoices> CrashReportSubmission::prepared_choices(StringView manifest)
{
    auto object = parse_json_object(manifest);
    if (!object.has_value())
        return {};
    auto fields = object->get_array("fields"sv);
    if (!fields.has_value())
        return {};

    PreparedChoices choices;
    for (auto const& entry : fields->values()) {
        if (!entry.is_object())
            continue;
        auto key = entry.as_object().get_string("key"sv);
        auto value = entry.as_object().get_object("value"sv);
        if (!key.has_value() || !value.has_value())
            continue;
        auto text = value->get_string("value"sv);
        if (!text.has_value())
            continue;
        if (*key == "description"sv)
            choices.description = text->to_byte_string();
        else if (*key == "url"sv)
            choices.url = text->to_byte_string();
    }
    return choices;
}

NonnullRefPtr<CrashReportSubmission> CrashReportSubmission::create(Options options)
{
    return adopt_ref(*new CrashReportSubmission(move(options)));
}

// A submission builds and persists its manifest, solves the proof-of-work challenge off the main
// thread, uploads the report, and removes the local files after the server acknowledges it.
CrashReportSubmission::CrashReportSubmission(Options options)
    : m_options(move(options))
{
}

CrashReportSubmission::~CrashReportSubmission() = default;

void CrashReportSubmission::start()
{
    begin_attempt();
}

void CrashReportSubmission::begin_attempt()
{
    if (on_progress)
        on_progress(Stage::Preparing);

    auto manifest = prepare_manifest();
    if (manifest.is_error()) {
        warnln("Could not prepare the crash report for sending: {}", manifest.error());
        NonnullRefPtr protector = *this;
        if (on_failed)
            on_failed("Could not prepare this report. Reload the page to review it again."_string);
        return;
    }
    if (on_prepared)
        on_prepared(sha256_hex(manifest.value().bytes()));
    request_challenge(manifest.release_value());
}

ErrorOr<ByteString> CrashReportSubmission::prepare_manifest()
{
    if (!m_manifest.is_empty())
        return m_manifest;

    auto report = TRY(CrashReportStore::the().saved_report(m_options.report_name));
    if (sha256_hex(report.text.bytes()) != m_options.reviewed_text_digest
        || sha256_hex(report.prepared_manifest.bytes()) != m_options.reviewed_manifest_digest) {
        return Error::from_string_literal("Crash report changed after review");
    }

    m_report_text = move(report.text);
    auto manifest = report.prepared_manifest.is_empty()
        ? build_manifest(m_options, m_report_text)
        : move(report.prepared_manifest);

    // A manifest already on disk wins, so every attempt uploads identical bytes under one submission ID.
    auto prepared_manifest = TRY(CrashReportStore::the().prepare_submission(m_options.report_name, manifest));
    if (prepared_manifest != manifest)
        return Error::from_string_literal("Crash report submission changed after review");
    m_options.reviewed_manifest_digest = sha256_hex(prepared_manifest.bytes());
    m_manifest = move(prepared_manifest);
    return m_manifest;
}

void CrashReportSubmission::request_challenge(ByteString manifest)
{
    if (on_progress)
        on_progress(Stage::RequestingChallenge);

    JsonObject body;
    body.set("manifest_digest"sv, to_string(sha256_hex(manifest.bytes())));

    post(challenge_path, "requesting a challenge"sv, "application/json"sv, body.serialized().to_byte_string(), {},
        [this, manifest = move(manifest)](ByteString response) mutable {
            auto challenge = parse_json_object(response);
            if (!challenge.has_value()) {
                fail("The report server sent an unreadable challenge."_string, ShouldRetry::No);
                return;
            }

            auto algorithm = challenge->get_string("algorithm"sv);
            if (!algorithm.has_value() || *algorithm != challenge_algorithm) {
                fail("Unsupported report challenge."_string, ShouldRetry::No);
                return;
            }

            auto token = challenge->get_string("token"sv);
            auto expected_work = challenge->get_integer<u64>("expected_work"sv);
            if (!token.has_value() || token->bytes().size() > maximum_challenge_token_length
                || !expected_work.has_value() || *expected_work == 0 || *expected_work > maximum_expected_work) {
                fail("The report server supplied an invalid challenge."_string, ShouldRetry::No);
                return;
            }

            solve_proof(move(manifest), token->to_byte_string(), *expected_work);
        });
}

void CrashReportSubmission::solve_proof(ByteString manifest, ByteString token, u64 expected_work)
{
    if (on_progress)
        on_progress(Stage::SolvingProof);

    // The server chooses the difficulty, so the search never runs on the thread that draws the browser.
    auto& main_thread_event_loop = Core::EventLoop::current();
    Threading::ThreadPool::the().submit(
        [weak_this = make_weak_ptr<CrashReportSubmission>(), &main_thread_event_loop, attempt = m_attempt,
            manifest = move(manifest), token = move(token), expected_work]() mutable {
            auto nonce = solve_challenge(token, expected_work);
            main_thread_event_loop.deferred_invoke(
                [weak_this = move(weak_this), attempt, manifest = move(manifest), token = move(token),
                    nonce]() mutable {
                    auto self = weak_this.strong_ref();
                    if (!self || self->m_attempt != attempt)
                        return;
                    if (!nonce.has_value()) {
                        self->fail("Could not compute the report's proof of work."_string, ShouldRetry::No);
                        return;
                    }
                    self->send_report(move(manifest), ProofOfWork { move(token), *nonce });
                });
        });
}

void CrashReportSubmission::send_report(ByteString manifest, ProofOfWork proof)
{
    if (on_progress)
        on_progress(Stage::Sending);

    auto boundary = ByteString::formatted("ladybird-{}", uuid_v7());
    auto body = build_multipart_body(manifest, m_report_text, boundary);
    auto content_type = ByteString::formatted("multipart/form-data; boundary={}", boundary);

    post(report_path, "sending the report"sv, content_type, move(body), move(proof), [this](ByteString response) {
        auto body = parse_json_object(response);
        auto receipt = body.has_value() ? body->get_string("receipt"sv) : Optional<String const&> {};
        if (!receipt.has_value() || receipt->is_empty()) {
            fail("Report server did not provide a receipt."_string, ShouldRetry::No);
            return;
        }
        finish();
    });
}

// A response outside the 2xx range fails this attempt, and the activity names what was going on in that message.
void CrashReportSubmission::post(StringView path, StringView activity, StringView content_type, ByteString body,
    Optional<ProofOfWork> proof, Function<void(ByteString)> on_success)
{
    auto endpoint = Core::Environment::get("LADYBIRD_REPORTS_ENDPOINT"sv).value_or(default_endpoint);
    auto url = URL::Parser::basic_parse(MUST(String::formatted("{}{}{}", endpoint,
        endpoint.ends_with('/') ? ""sv : "/"sv, path.substring_view(1))));
    if (!url.has_value() || (url->scheme() != "https"sv && url->scheme() != "http"sv)) {
        fail("Invalid report server configuration."_string, ShouldRetry::No);
        return;
    }

    auto headers = HTTP::HeaderList::create();
    headers->set({ "Content-Type"sv, ByteString { content_type } });
    if (proof.has_value()) {
        headers->set({ "X-Ladybird-Challenge"sv, proof->token });
        headers->set({ "X-Ladybird-Nonce"sv, ByteString::number(proof->nonce) });
    }

    auto& request_client = Application::request_server_client(IsPrivate::No);
    m_request = request_client.start_request("POST"sv, *url, *headers, body.bytes(),
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!m_request) {
        fail("Could not connect to the report server."_string, ShouldRetry::Yes);
        return;
    }

    m_request_timeout = Core::Timer::create_single_shot(request_timeout_ms,
        [weak_this = make_weak_ptr<CrashReportSubmission>()] {
            auto self = weak_this.strong_ref();
            if (!self)
                return;
            auto request = move(self->m_request);
            self->m_request_timeout = nullptr;
            if (request)
                request->stop();
            self->fail("The report server did not respond in time."_string, ShouldRetry::Yes);
        });
    m_request_timeout->start();

    m_request->set_buffered_request_finished_callback(
        [weak_this = make_weak_ptr<CrashReportSubmission>(), activity = ByteString { activity },
            on_success = move(on_success)](u64, Requests::RequestTimingInfo const&,
            Optional<Requests::NetworkError> const& network_error, HTTP::HeaderList const& response_headers,
            Optional<u32> response_code, Optional<String> const&,
            Optional<Core::ImmutableBytes>, Optional<u64>, Requests::CameFromCache,
            Core::ImmutableBytes payload) {
            auto self = weak_this.strong_ref();
            if (!self || !self->m_request)
                return;
            if (self->m_request_timeout)
                self->m_request_timeout->stop();
            self->m_request_timeout = nullptr;

            // A request cannot be destroyed from inside its own callback.
            Core::deferred_invoke([weak_this] {
                if (auto self = weak_this.strong_ref())
                    self->m_request = nullptr;
            });

            if (network_error.has_value() || !response_code.has_value()) {
                self->fail("Network error while contacting the report server."_string, ShouldRetry::Yes);
                return;
            }
            if (*response_code < 200 || *response_code >= 300) {
                self->fail(MUST(String::formatted("Report server returned {} while {}.", *response_code, activity)),
                    is_transient_status(*response_code) ? ShouldRetry::Yes : ShouldRetry::No,
                    retry_after_ms(response_headers));
                return;
            }
            on_success(ByteString::copy(payload.bytes()));
        });
}

void CrashReportSubmission::finish()
{
    // Reporting success can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }
    if (auto result = CrashReportStore::the().remove_sent_report(m_options.report_name); result.is_error())
        warnln("Could not remove the sent crash report: {}", result.error());
    if (on_sent)
        on_sent();
}

void CrashReportSubmission::fail(String reason, ShouldRetry should_retry, Optional<int> retry_after_ms)
{
    // Reporting the final failure can release the last reference to this submission.
    NonnullRefPtr protector = *this;

    if (m_request_timeout) {
        m_request_timeout->stop();
        m_request_timeout = nullptr;
    }

    if (should_retry == ShouldRetry::No || m_attempt >= maximum_attempts) {
        if (on_failed)
            on_failed(move(reason));
        return;
    }

    // A server that asks for more time gets it, but it cannot hold the page in its retry state for long.
    auto delay_ms = clamp(retry_after_ms.value_or(retry_delay_ms), retry_delay_ms, maximum_retry_delay_ms);
    auto retry_number = m_attempt++;
    if (on_retry)
        on_retry(move(reason), retry_number, static_cast<u32>(delay_ms / 1000));

    m_retry_timer = Core::Timer::create_single_shot(delay_ms,
        [weak_this = make_weak_ptr<CrashReportSubmission>()] {
            if (auto self = weak_this.strong_ref())
                self->begin_attempt();
        });
    m_retry_timer->start();
}

}
