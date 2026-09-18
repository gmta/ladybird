/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/CrashReportSubmission.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class CrashReportUI final : public WebUI {
    WEB_UI(CrashReportUI);

private:
    virtual void register_interfaces() override;

    void load_report(JsonValue const& requested_name);
    void ignore_report(JsonValue const&);
    void close_tab();
    void submit_report(JsonValue const&);

    RefPtr<CrashReportSubmission> m_submission;
};

}
