#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import os
import shlex
import signal
import subprocess
import tempfile
import time

from pathlib import Path

webdriver = importlib.import_module("test-webdriver-delete-session")


def launch(webdriver_binary, environment):
    port = webdriver.unused_port()
    process = subprocess.Popen(
        [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port)],
        env=environment,
    )
    webdriver.wait_for_port(port)
    status, payload, response = webdriver.request(
        port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": True}}}
    )
    assert status == 200, response
    return process, port, payload["value"]["sessionId"]


def browser_child_pid(webdriver_pid):
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines()
    children = {}
    for row in rows:
        fields = row.strip().split(None, 2)
        if len(fields) == 3:
            children.setdefault(int(fields[1]), []).append((int(fields[0]), fields[2]))

    pending = [webdriver_pid]
    browser_pids = []
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            arguments = shlex.split(command)
            if arguments and Path(arguments[0]).name.lower() == "ladybird":
                browser_pids.append(pid)
    assert len(browser_pids) == 1, browser_pids
    return browser_pids[0]


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-browser-crash-") as temporary:
        environment = os.environ.copy()
        environment.pop("LADYBIRD_SOURCE_DIR", None)
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        reports_directory = Path(temporary) / "data/Ladybird/CrashReports"

        first, _port, _session = launch(webdriver_binary, environment)
        try:
            os.kill(browser_child_pid(first.pid), signal.SIGSEGV)
            deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                if list(reports_directory.glob("Browser-*.pending")):
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Browser did not leave signal-safe crash records")
        finally:
            stop(first)

        second, port, session = launch(webdriver_binary, environment)
        try:
            deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                reports = list(reports_directory.glob("*-Browser-*.txt"))
                if reports:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Browser crash was not recovered on restart")
            assert len(reports) == 1, reports
            text = reports[0].read_text()
            assert "Process: Browser\n" in text
            assert "Termination signal name: SIGSEGV" in text
            assert "Termination signal number: 11" in text
            assert "Native stack" in text

            status, _, response = webdriver.request(
                port, "POST", f"/session/{session}/url", {"url": "about:crash-report"}
            )
            assert status == 200, response
            deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                status, payload, response = webdriver.request(
                    port,
                    "POST",
                    f"/session/{session}/execute/sync",
                    {
                        "script": "return document.querySelector('.report-card h2')?.textContent ?? ''",
                        "args": [],
                    },
                )
                if status == 200 and "Ladybird crashed" in payload["value"]:
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("Recovered Browser crash not shown in the report page")

            # The page was opened without naming a report, so it picked the one crash nobody has
            # answered yet. Showing it is the prompt for it: a user who closes the tab instead of
            # answering must not be asked about the same crash on every launch from here on.
            ignored_marker = reports_directory / (reports[0].name + ".ignored")
            deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
            while not ignored_marker.exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            assert ignored_marker.exists()

            # The report is still on disk, so it can be reopened and sent by name.
            assert reports[0].exists()
            status, _, response = webdriver.request(
                port, "POST", f"/session/{session}/url", {"url": "about:crash-report"}
            )
            assert status == 200, response
            deadline = time.monotonic() + webdriver.EVENT_TIMEOUT_SECONDS
            while time.monotonic() < deadline:
                status, payload, _ = webdriver.request(
                    port,
                    "POST",
                    f"/session/{session}/execute/sync",
                    {
                        "script": "return document.querySelector('.status-panel [role=status]')?.textContent ?? ''",
                        "args": [],
                    },
                )
                if status == 200 and payload["value"] == "There is no crash report to review.":
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("An answered crash report was offered for review again")
            webdriver.request(port, "DELETE", f"/session/{session}")
        finally:
            stop(second)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
