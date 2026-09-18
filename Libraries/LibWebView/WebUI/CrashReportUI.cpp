/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibWebView/Application.h>
#include <LibWebView/CrashReportDiagnostics.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebUI/CrashReportUI.h>

namespace WebView {

static constexpr size_t maximum_description_length = 16384;
static constexpr size_t maximum_url_length = 1024;

static ByteString sha256_hex(ReadonlyBytes bytes)
{
    return encode_hex(Crypto::Hash::SHA256::hash(bytes).bytes());
}

void CrashReportUI::register_interfaces()
{
    register_interface("loadCrashReport"sv, [this](auto const& data) { load_report(data); });
    register_interface("ignoreCrashReport"sv, [this](auto const& data) { ignore_report(data); });
    register_interface("submitCrashReport"sv, [this](auto const& data) { submit_report(data); });
    register_interface("closeCrashReportTab"sv, [this](auto const&) { close_tab(); });
}

// The page reviews one report. Which one is named in the URL when a crash screen or a launch after a
// browser crash opens the page; otherwise the newest report nobody has answered yet is taken.
void CrashReportUI::load_report(JsonValue const& requested_name)
{
    auto pending = CrashReportStore::the().pending_report_names();
    if (pending.is_error()) {
        async_send_message("crashReportError"sv, "Could not read saved crash reports."_string);
        return;
    }

    auto name = requested_name.is_string() ? requested_name.as_string().to_byte_string() : ByteString {};
    if (name.is_empty()) {
        if (pending.value().is_empty()) {
            async_send_message("crashReportUnavailable"sv, JsonValue {});
            return;
        }
        name = pending.value().first();
    }

    auto report = CrashReportStore::the().saved_report(name);
    if (report.is_error()) {
        async_send_message("crashReportUnavailable"sv, JsonValue {});
        return;
    }

    // Putting the report in front of the user is the prompt for it, however the page was reached.
    // Without this, a report closed rather than answered would come back on every launch.
    if (auto result = CrashReportStore::the().mark_ignored(name); result.is_error())
        warnln("Could not mark crash report as ignored: {}", result.error());

    // The page renders what this parse produces, and a submission sends what the same parse
    // produces, so a reviewer cannot be shown one reading of a report and send another.
    auto diagnostics = CrashReportDiagnostics::parse(report.value().text);

    JsonArray headers;
    for (auto const& header : diagnostics.headers) {
        JsonObject entry;
        entry.set("name"sv, String::from_utf8_with_replacement_character(header.name));
        entry.set("value"sv, String::from_utf8_with_replacement_character(header.value));
        headers.must_append(move(entry));
    }

    JsonArray frames;
    for (auto const& frame : diagnostics.frames) {
        JsonObject entry;
        entry.set("binary"sv, String::from_utf8_with_replacement_character(frame.binary));
        entry.set("address"sv, String::from_utf8_with_replacement_character(frame.address));
        entry.set("symbol"sv, String::from_utf8_with_replacement_character(frame.symbol));
        frames.must_append(move(entry));
    }

    JsonObject item;
    item.set("name"sv, String::from_utf8_with_replacement_character(report.value().name));
    item.set("text"sv, String::from_utf8_with_replacement_character(report.value().text));
    item.set("process"sv, String::from_utf8_with_replacement_character(diagnostics.header("Process"sv)));
    item.set("signalName"sv, String::from_utf8_with_replacement_character(diagnostics.signal.name));
    item.set("headers"sv, move(headers));
    item.set("frames"sv, move(frames));
    item.set("stack"sv, String::from_utf8_with_replacement_character(diagnostics.stack));
    item.set("textDigest"sv, String::from_utf8_with_replacement_character(sha256_hex(report.value().text.bytes())));
    item.set("manifestDigest"sv,
        String::from_utf8_with_replacement_character(sha256_hex(report.value().prepared_manifest.bytes())));
    if (!report.value().prepared_manifest.is_empty()) {
        auto prepared = CrashReportSubmission::prepared_choices(report.value().prepared_manifest).value_or({});
        item.set("preparedDescription"sv, String::from_utf8_with_replacement_character(prepared.description));
        item.set("preparedUrl"sv, String::from_utf8_with_replacement_character(prepared.url));
        item.set("preparedManifest"sv,
            String::from_utf8_with_replacement_character(report.value().prepared_manifest));
    }
    // Answering this report leaves the others untouched, so what comes next is already known.
    item.set("hasNextReport"sv, any_of(pending.value(), [&](auto const& pending_name) { return pending_name != name; }));
    async_send_message("crashReportLoaded"sv, move(item));
}

// Loading the report already marked it, so this only records the answer the user actually gave.
void CrashReportUI::ignore_report(JsonValue const& data)
{
    if (!data.is_string())
        return;
    if (auto result = CrashReportStore::the().mark_ignored(data.as_string().to_byte_string()); result.is_error())
        warnln("Could not mark crash report as ignored: {}", result.error());
    async_send_message("crashReportIgnored"sv, JsonValue {});
}

// Closing the only tab would take the window with it, so the review page gives way to the new tab
// page instead of leaving the user with nothing.
void CrashReportUI::close_tab()
{
    auto review_tab = view();
    if (!review_tab.has_value())
        return;

    auto is_only_tab = true;
    ViewImplementation::for_each_view([&](auto& other) {
        if (&other == &review_tab.value())
            return IterationDecision::Continue;
        is_only_tab = false;
        return IterationDecision::Break;
    });

    if (!is_only_tab && review_tab->on_close) {
        review_tab->on_close();
        return;
    }
    review_tab->load(Application::settings().new_tab_page_url());
}

void CrashReportUI::submit_report(JsonValue const& data)
{
    if (m_submission) {
        async_send_message("crashReportFailed"sv, "Another report is already being sent."_string);
        return;
    }

    auto name = data.is_object() ? data.as_object().get_string("name"sv) : Optional<String const&> {};
    if (!name.has_value()) {
        async_send_message("crashReportFailed"sv, "Invalid report data."_string);
        return;
    }

    auto description = data.as_object().get_string("description"sv).value_or({});
    auto url = data.as_object().get_string("url"sv).value_or({});
    auto text_digest = data.as_object().get_string("textDigest"sv).value_or({});
    auto manifest_digest = data.as_object().get_string("manifestDigest"sv).value_or({});
    if (description.bytes().size() > maximum_description_length || url.bytes().size() > maximum_url_length) {
        async_send_message("crashReportFailed"sv, "This report is too large to send."_string);
        return;
    }

    auto submission = CrashReportSubmission::create({
        .report_name = name->to_byte_string(),
        .description = description.to_byte_string(),
        .url = url.to_byte_string(),
        .reviewed_text_digest = text_digest.to_byte_string(),
        .reviewed_manifest_digest = manifest_digest.to_byte_string(),
    });

    submission->on_progress = [this](auto stage) {
        JsonObject progress;
        progress.set("stage"sv, String::from_utf8_with_replacement_character(CrashReportSubmission::stage_name(stage)));
        async_send_message("crashReportProgress"sv, move(progress));
    };
    submission->on_prepared = [this](ByteString manifest_digest) {
        async_send_message("crashReportPrepared"sv, String::from_utf8_with_replacement_character(manifest_digest));
    };
    submission->on_retry = [this](String reason, u32 retry_number) {
        JsonObject retry;
        retry.set("reason"sv, move(reason));
        retry.set("retry"sv, retry_number);
        retry.set("maximumRetries"sv, CrashReportSubmission::maximum_attempts - 1);
        async_send_message("crashReportRetrying"sv, move(retry));
    };
    submission->on_sent = [this] {
        m_submission = nullptr;
        async_send_message("crashReportSent"sv, JsonValue {});
    };
    submission->on_failed = [this](String reason) {
        m_submission = nullptr;
        async_send_message("crashReportFailed"sv, move(reason));
    };

    m_submission = submission;
    submission->start();
}

}
