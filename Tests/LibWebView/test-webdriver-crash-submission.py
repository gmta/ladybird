#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import hashlib
import http.server
import importlib
import json
import os
import re
import secrets
import subprocess
import tempfile
import threading
import time
import uuid

from pathlib import Path
from typing import Optional
from urllib.parse import urlencode

webdriver = importlib.import_module("test-webdriver-delete-session")


def generated_uuid_v7():
    data = bytearray(secrets.token_bytes(16))
    data[:6] = int(time.time() * 1000).to_bytes(6, "big")
    data[6] = (data[6] & 0x0F) | 0x70
    data[8] = (data[8] & 0x3F) | 0x80
    return str(uuid.UUID(bytes=bytes(data)))


class ReportsHandler(http.server.BaseHTTPRequestHandler):
    token = secrets.token_urlsafe(32)
    manifest_digest = None
    attempts = 0
    failures_remaining = 1
    rate_limits_remaining = 0
    rejections_remaining = 0
    submitted_manifest: Optional[bytes] = None

    def log_message(self, format, *args):
        pass

    def reply(self, status, content, headers=None):
        body = json.dumps(content).encode()
        self.send_response(status)
        for name, value in (headers or {}).items():
            self.send_header(name, value)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.path == "/api/v1/challenges":
            ReportsHandler.attempts += 1
            if ReportsHandler.failures_remaining:
                ReportsHandler.failures_remaining -= 1
                time.sleep(0.3)
                self.reply(503, {"error": "temporarily unavailable"})
                return
            if ReportsHandler.rate_limits_remaining:
                ReportsHandler.rate_limits_remaining -= 1
                self.reply(429, {"error": "Rate limit exceeded"}, {"Retry-After": "4"})
                return
            ReportsHandler.manifest_digest = json.loads(body)["manifest_digest"]
            self.reply(
                200,
                {
                    "algorithm": "sha256-seed-nonce-le-v1",
                    "token": self.token,
                    "expected_work": 1,
                    "expires_at_unix": int(time.time()) + 300,
                },
            )
            return

        if self.path != "/api/v1/reports":
            self.reply(404, {"error": "unknown path"})
            return

        if ReportsHandler.rejections_remaining:
            ReportsHandler.rejections_remaining -= 1
            self.reply(400, {"error": "Invalid report payload"})
            return

        assert self.headers["X-Ladybird-Challenge"] == self.token
        nonce = int(self.headers["X-Ladybird-Nonce"])
        assert nonce >= 0
        match = re.search(rb'name="manifest"\r\nContent-Type: application/json\r\n\r\n(.*?)\r\n--', body, re.DOTALL)
        assert match is not None
        manifest_bytes = match.group(1)
        assert hashlib.sha256(manifest_bytes).hexdigest() == ReportsHandler.manifest_digest
        manifest = json.loads(manifest_bytes)
        assert manifest["protocol"] == 1
        assert uuid.UUID(manifest["submission_id"]).version == 7
        assert manifest["kind"] == "crash"
        assert len(manifest["attachments"]) == 1
        attachment = manifest["attachments"][0]
        assert uuid.UUID(attachment["id"]).version == 7
        assert attachment["name"] == "crash-diagnostics.txt"
        assert attachment["media_type"] == "text/plain"
        boundary = self.headers["Content-Type"].split("boundary=", 1)[1]
        attachment_header = (
            f"--{boundary}\r\n"
            f'Content-Disposition: form-data; name="{attachment["id"]}"; filename="crash-diagnostics.txt"\r\n'
            "Content-Type: text/plain; charset=utf-8\r\n\r\n"
        ).encode()
        assert body.count(attachment_header) == 1
        attachment_with_closing = body.split(attachment_header, 1)[1]
        closing = f"\r\n--{boundary}--\r\n".encode()
        assert attachment_with_closing.endswith(closing)
        attachment_bytes = attachment_with_closing[: -len(closing)]
        assert attachment_bytes.decode("utf-8").startswith("Ladybird crash report")
        assert attachment["size"] == len(attachment_bytes)
        assert attachment["sha256"] == hashlib.sha256(attachment_bytes).hexdigest()
        fields = {field["key"]: field["value"] for field in manifest["fields"]}
        assert fields["stack"]["type"] == "stack_trace"
        assert "#0 " in fields["stack"]["value"]
        assert manifest["build"] == fields["build_configuration"]["value"]
        assert fields["git_commit"]["type"] == "text"
        assert fields["cpp_compiler"]["type"] == "text"
        failure_reason = fields["failure_reason"]
        assert failure_reason["type"] == "text"
        assert failure_reason["value"].startswith("Verification failed: false at Services/WebContent/")
        assert "/Users/" not in failure_reason["value"]
        assert failure_reason["value"] in attachment_bytes.decode("utf-8")
        assert "diagnostics" not in fields
        assert "Native stack" in attachment_bytes.decode("utf-8")
        signal = re.search(r"Termination signal: (\w+) \((\d+)\)", attachment_bytes.decode("utf-8"))
        assert signal is not None
        assert fields["signal"] == {"type": "text", "value": signal.group(1)}
        assert fields["signal_number"] == {"type": "number", "value": int(signal.group(2))}
        if ReportsHandler.submitted_manifest is None:
            assert "url" not in fields and "hostname" not in fields
        else:
            assert fields["url"] == {"type": "text", "value": "https://private.example/second-page"}
        ReportsHandler.submitted_manifest = manifest_bytes
        self.reply(200, {"receipt": generated_uuid_v7()})


def command(port, session, path, body):
    status, payload, response = webdriver.request(port, "POST", f"/session/{session}{path}", body)
    assert status == 200, response
    return payload["value"]


def wait_for(port, session, script, predicate):
    deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
    value = None
    while time.monotonic() < deadline:
        value = command(port, session, "/execute/sync", {"script": script, "args": []})
        if predicate(value):
            return value
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for browser state: {value!r}")


def run_test(webdriver_binary):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ReportsHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-crash-submission-") as temporary:
            environment = os.environ.copy()
            environment.pop("LADYBIRD_SOURCE_DIR", None)
            for variable, directory in (
                ("XDG_DATA_HOME", "data"),
                ("XDG_CONFIG_HOME", "config"),
                ("XDG_CACHE_HOME", "cache"),
            ):
                environment[variable] = str(Path(temporary) / directory)
            environment["LADYBIRD_REPORTS_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
            port = webdriver.unused_port()
            process = subprocess.Popen(
                [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port)],
                env=environment,
            )
            try:
                webdriver.wait_for_port(port)
                status, payload, response = webdriver.request(
                    port,
                    "POST",
                    "/session",
                    {
                        "capabilities": {
                            "alwaysMatch": {
                                "ladybird:headless": True,
                                "ladybird:enableTestHooks": True,
                            }
                        }
                    },
                )
                assert status == 200, response
                session = payload["value"]["sessionId"]

                command(port, session, "/url", {"url": "data:text/html,<title>PRIVATE_CRASH_PAGE</title>"})
                command(port, session, "/ladybird/crash-current-page", {})
                reports_directory = Path(temporary) / "data/Ladybird/CrashReports"
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while time.monotonic() < deadline:
                    reports = list(reports_directory.glob("*-WebContent-*.txt"))
                    if reports and "Stacks may be partial." in reports[0].read_text():
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("Ladybird did not save a crash report")
                reports[0].write_text(
                    reports[0]
                    .read_text()
                    .replace("Process: WebContent", "Encoding check: café\nProcess: WebContent", 1)
                )

                review_url = "about:crash-report?" + urlencode(
                    {
                        "report": reports[0].name,
                        "website": "https://private.example/page?token=not-shared",
                    }
                )
                command(port, session, "/url", {"url": review_url})
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                context = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return [document.querySelector('.website-value').value, "
                            "document.querySelector('.website-field input[type=checkbox]').checked, "
                            "document.querySelector('.website-value').tagName]"
                        ),
                        "args": [],
                    },
                )
                assert context == ["https://private.example/page?token=not-shared", False, "INPUT"]
                summary = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return Array.from(document.querySelectorAll('.summary-fields .field-row'), row => "
                            "row.textContent)"
                        ),
                        "args": [],
                    },
                )
                assert any("Crash date" in row for row in summary)
                assert any("SignalSIGTRAP" in row for row in summary)
                assert not any("Signal number" in row for row in summary)
                layout = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return [document.querySelector('.report-heading img') !== null, "
                            "document.querySelector('.report-intro h3').textContent, "
                            "getComputedStyle(document.querySelector('.summary-fields dt')).whiteSpace, "
                            "reportTitle('ImageDecoder')]"
                        ),
                        "args": [],
                    },
                )
                assert layout == [True, "Help us improve Ladybird", "nowrap", "ImageDecoder crashed"]
                status, _, response = webdriver.request(
                    port, "POST", f"/session/{session}/window/rect", {"width": 390, "height": 700}
                )
                assert status == 200, response
                mobile = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "document.querySelector('.technical-details').open = true; return "
                            "[document.documentElement.scrollWidth <= document.documentElement.clientWidth, "
                            "Array.from(document.querySelectorAll('.field-row dt'), key => "
                            "getComputedStyle(key).whiteSpace).every(value => value === 'nowrap')]"
                        ),
                        "args": [],
                    },
                )
                assert mobile == [True, True]
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": "document.querySelector('.technical-details').open = false",
                        "args": [],
                    },
                )
                status, _, response = webdriver.request(
                    port, "POST", f"/session/{session}/window/rect", {"width": 1100, "height": 700}
                )
                assert status == 200, response
                technical = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return Array.from(document.querySelectorAll('.technical-fields .field-row'), row => "
                            "row.textContent)"
                        ),
                        "args": [],
                    },
                )
                assert any("Termination signal number" in row for row in technical)
                assert not any("Termination signal name" in row for row in technical)
                assert not any("Verification failed" in row for row in technical)
                assert not any("SIGTRAP (5)" in row for row in technical)
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": "document.querySelector('.report-card button').click()",
                        "args": [],
                    },
                )
                wait_for(
                    port,
                    session,
                    (
                        "return Boolean(document.querySelector('.progress-track[aria-valuenow]') "
                        "&& document.querySelector('.feedback.sending') "
                        "&& !document.querySelector('.feedback.sending .feedback-icon'))"
                    ),
                    lambda value: value,
                )
                progress = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "document.querySelector('.progress-track').dataset.testMarker = 'same'; return "
                            "[document.querySelector('.progress-track')?.getAttribute('aria-valuenow'), "
                            "Boolean(document.querySelector('.submission-panel .report-heading img'))]"
                        ),
                        "args": [],
                    },
                )
                assert progress[0] is not None and progress[1]
                assert (
                    command(
                        port,
                        session,
                        "/execute/sync",
                        {
                            "script": "return document.querySelectorAll('.report-card').length",
                            "args": [],
                        },
                    )
                    == 0
                )
                prepared_path = reports_directory / (reports[0].name + ".prepared")
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while not prepared_path.exists() and time.monotonic() < deadline:
                    time.sleep(0.1)
                assert prepared_path.exists()
                prepared_bytes = prepared_path.read_bytes()
                assert uuid.UUID(json.loads(prepared_bytes)["submission_id"]).version == 7
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.sent-confirmation h2')?.textContent ?? ''",
                    lambda value: value == "Report sent",
                )
                assert command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return Boolean(document.querySelector('.feedback.success .feedback-icon') && "
                            "document.querySelector('.sent-confirmation .report-heading img') && "
                            "document.querySelector('.sent-confirmation "
                            '.progress-track[aria-valuenow="100"][data-test-marker="same"]\'))'
                        ),
                        "args": [],
                    },
                )
                assert ReportsHandler.submitted_manifest == prepared_bytes
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while reports[0].exists() and time.monotonic() < deadline:
                    time.sleep(0.1)
                assert not reports[0].exists()
                for suffix in (".prepared", ".ignored"):
                    assert not (reports_directory / (reports[0].name + suffix)).exists()

                command(port, session, "/url", {"url": "about:crash-report"})
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.status-panel [role=status]')?.textContent ?? ''",
                    lambda value: value == "There is no crash report to review.",
                )
                assert (
                    command(
                        port,
                        session,
                        "/execute/sync",
                        {
                            "script": "return document.querySelectorAll('.report-card, .sent-confirmation').length",
                            "args": [],
                        },
                    )
                    == 0
                )

                command(port, session, "/url", {"url": "data:text/html,<title>SECOND_PRIVATE_PAGE</title>"})
                command(port, session, "/ladybird/crash-current-page", {})
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while time.monotonic() < deadline:
                    second_reports = [
                        path
                        for path in reports_directory.glob("*-WebContent-*.txt")
                        if path != reports[0] and "Stacks may be partial." in path.read_text()
                    ]
                    if second_reports:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("Ladybird did not save the second crash report")

                command(
                    port,
                    session,
                    "/url",
                    {
                        "url": "about:crash-report?"
                        + urlencode(
                            {
                                "report": second_reports[0].name,
                                "website": "https://private.example/second-page?token=secret",
                            }
                        )
                    },
                )
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                second_reports[0].write_text(second_reports[0].read_text() + "\nChanged after review\n")
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelector('.report-card button').click()", "args": []},
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.submission-panel .feedback.error')?.textContent ?? ''",
                    lambda value: "Reload the page to review it again" in value,
                )
                assert not (reports_directory / (second_reports[0].name + ".prepared")).exists()
                second_prepared_path = reports_directory / (second_reports[0].name + ".prepared")
                previous_manifest = json.loads(prepared_bytes)
                previous_manifest["fields"].append(
                    {"key": "extra_context", "value": {"type": "text", "value": "REVIEWED_VALUE"}}
                )
                second_prepared_path.write_text(json.dumps(previous_manifest))
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "if (document.querySelector('.submission-panel button').textContent !== "
                            "'Review report again') throw Error('Missing review action'); "
                            "document.querySelector('.submission-panel button').click()"
                        ),
                        "args": [],
                    },
                )
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                prepared_review = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return document.querySelector('.technical-details').textContent.includes('REVIEWED_VALUE')"
                        ),
                        "args": [],
                    },
                )
                assert prepared_review
                second_prepared_path.write_text(
                    second_prepared_path.read_text().replace("REVIEWED_VALUE", "CHANGED_VALUE")
                )
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelector('.report-card button').click()", "args": []},
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.submission-panel .feedback.error')?.textContent ?? ''",
                    lambda value: "Reload the page to review it again" in value,
                )
                second_prepared_path.unlink()
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelector('.submission-panel button').click()", "args": []},
                )
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                ReportsHandler.failures_remaining = 6
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "document.querySelector('.website-value').value = 'https://private.example/second-page'; "
                            "document.querySelector('.report-card input[type=checkbox]').click(); "
                            "document.querySelector('.report-card button').click()"
                        ),
                        "args": [],
                    },
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.submission-panel button')?.textContent ?? ''",
                    lambda value: value == "Try sending again",
                )
                assert ReportsHandler.failures_remaining == 0
                assert (
                    command(
                        port,
                        session,
                        "/execute/sync",
                        {
                            "script": "return document.querySelectorAll('.report-card').length",
                            "args": [],
                        },
                    )
                    == 0
                )

                # The server would reject the same report again, so a rejection is not retried.
                ReportsHandler.rejections_remaining = 1
                attempts_before_rejection = ReportsHandler.attempts
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": "document.querySelector('.submission-panel button').click()",
                        "args": [],
                    },
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.submission-panel .feedback.error')?.textContent ?? ''",
                    lambda value: value == "Report server returned 400 while sending the report.",
                )
                assert ReportsHandler.rejections_remaining == 0
                assert ReportsHandler.attempts == attempts_before_rejection + 1

                # A rate limit is retried after the delay the server asks for.
                ReportsHandler.rate_limits_remaining = 1
                command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": "document.querySelector('.submission-panel button').click()",
                        "args": [],
                    },
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelector('.submission-panel .feedback.sending')?.textContent ?? ''",
                    lambda value: "Trying again in 4 seconds (1 of 5)" in value,
                )
                wait_for(
                    port,
                    session,
                    "return document.querySelectorAll('.sent-confirmation').length",
                    lambda value: value == 1,
                )
                assert ReportsHandler.rate_limits_remaining == 0
                assert ReportsHandler.submitted_manifest is not None
                submitted_fields = {
                    field["key"]: field["value"] for field in json.loads(ReportsHandler.submitted_manifest)["fields"]
                }
                assert submitted_fields["url"] == {"type": "text", "value": "https://private.example/second-page"}
                assert b"token=secret" not in ReportsHandler.submitted_manifest
                sent_actions = command(
                    port,
                    session,
                    "/execute/sync",
                    {
                        "script": (
                            "return Array.from(document.querySelectorAll('.sent-confirmation .actions button'), "
                            "button => button.textContent)"
                        ),
                        "args": [],
                    },
                )
                assert sent_actions == ["Close this tab"]

                for _ in range(2):
                    previous = set(reports_directory.glob("*-WebContent-*.txt"))
                    command(port, session, "/url", {"url": "data:text/html,<title>IGNORED_PAGE</title>"})
                    command(port, session, "/ladybird/crash-current-page", {})
                    deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                    while time.monotonic() < deadline:
                        if set(reports_directory.glob("*-WebContent-*.txt")) - previous:
                            break
                        time.sleep(0.1)
                    else:
                        raise AssertionError("Ladybird did not save a crash report to ignore")

                # No report is named, so the page picks the newest one nobody has answered yet.
                command(port, session, "/url", {"url": "about:crash-report"})
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelectorAll('.report-card button')[1].click()", "args": []},
                )
                ignored_actions = wait_for(
                    port,
                    session,
                    (
                        "return Array.from(document.querySelectorAll('.ignored-confirmation .actions button'), "
                        "button => button.textContent)"
                    ),
                    lambda value: len(value) == 2,
                )
                assert ignored_actions == ["Review the next crash report", "Close this tab"]
                assert len(list(reports_directory.glob("*.ignored"))) == 1

                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelector('.ignored-confirmation .actions button').click()", "args": []},
                )
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelectorAll('.report-card button')[1].click()", "args": []},
                )
                ignored_actions = wait_for(
                    port,
                    session,
                    (
                        "return Array.from(document.querySelectorAll('.ignored-confirmation .actions button'), "
                        "button => button.textContent)"
                    ),
                    lambda value: len(value) == 1,
                )
                assert ignored_actions == ["Close this tab"]
                assert len(list(reports_directory.glob("*.ignored"))) == 2

                # This is the only tab, so closing it would take the window with it. The review page
                # gives way to the new tab page instead.
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelector('.ignored-confirmation .actions button').click()", "args": []},
                )
                wait_for(
                    port,
                    session,
                    "return location.href",
                    lambda value: value == "about:newtab",
                )

                # With a second tab open, the same button closes the review tab instead. The crash
                # has to happen in the first tab, because a headless tab is discarded when the page
                # it holds crashes.
                previous = set(reports_directory.glob("*-WebContent-*.txt"))
                command(port, session, "/url", {"url": "data:text/html,<title>CLOSED_PAGE</title>"})
                command(port, session, "/ladybird/crash-current-page", {})
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while time.monotonic() < deadline:
                    if set(reports_directory.glob("*-WebContent-*.txt")) - previous:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("Ladybird did not save a crash report for the closing tab")

                status, payload, response = webdriver.request(port, "GET", f"/session/{session}/window/handles", None)
                assert status == 200, response
                first_handle = payload["value"][0]
                status, payload, response = webdriver.request(
                    port, "POST", f"/session/{session}/window/new", {"type": "tab"}
                )
                assert status == 200, response
                command(port, session, "/window", {"handle": payload["value"]["handle"]})

                command(port, session, "/url", {"url": "about:crash-report"})
                wait_for(
                    port, session, "return document.querySelectorAll('.report-card').length", lambda value: value == 1
                )
                command(
                    port,
                    session,
                    "/execute/sync",
                    {"script": "document.querySelectorAll('.report-card button')[1].click()", "args": []},
                )
                wait_for(
                    port,
                    session,
                    (
                        "return Array.from(document.querySelectorAll('.ignored-confirmation .actions button'), "
                        "button => button.textContent)"
                    ),
                    lambda value: value == ["Close this tab"],
                )
                # This click closes the window running the script, so the command itself may come
                # back as "no such window" instead of a result. Either way, the tab has to go.
                webdriver.request(
                    port,
                    "POST",
                    f"/session/{session}/execute/sync",
                    {"script": "document.querySelector('.ignored-confirmation .actions button').click()", "args": []},
                )
                deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
                while time.monotonic() < deadline:
                    status, payload, _ = webdriver.request(port, "GET", f"/session/{session}/window/handles", None)
                    if status == 200 and payload["value"] == [first_handle]:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError("The review tab was not closed")

                webdriver.request(port, "DELETE", f"/session/{session}")
            finally:
                process.terminate()
                process.wait(timeout=5)
    finally:
        server.shutdown()
        server.server_close()
        server_thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
