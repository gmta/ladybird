# Crash reports

On macOS and Linux, Ladybird saves local text reports when the browser or a
helper process crashes: WebContent, WebWorker, RequestServer, ImageDecoder,
Compositor and WasmCompiler. Reports are stored in
`~/Library/Application Support/Ladybird/CrashReports/` on macOS and
`~/.local/share/Ladybird/CrashReports/` on Linux, or under
`$XDG_DATA_HOME/Ladybird/CrashReports/` if that variable is set.
The directory is private to the current user, and report files have mode `0600`.
The newest 20 reports across all process types are kept. Filenames start with a
UTC date and time, for example
`2026-09-06T12-34-56Z-WebContent-a1B2c3.txt`, so they sort chronologically.
Hyphens in the time keep filenames compatible with Windows; the random suffix
avoids collisions. Retention includes reports saved with the older filenames.
Nothing is uploaded automatically.

The crash screen provides **Reload Page** and **Review crash report** actions.
The latter opens `about:crash-report`, an HTML/JavaScript page that reviews one
saved report: the one the crash screen is asking about, named in the page's
`report` query parameter. It shows the most useful crash details first, with
technical fields and the original saved text available on request. Opening
`about:crash-report` without naming a report reviews the newest report that has
not been answered yet, which is what the next launch after a browser-process
crash does. A launch opens the review page ahead of whatever URLs it was asked
for, and falls back to the new tab page only when that would leave the window
with nothing to show. **Settings > Advanced > Crash reports > Open folder** opens the
directory of saved reports and is available even when nothing has crashed.
Each saved crash prompts at most once: displaying the crash screen counts as
the prompt for the report behind it, and so does opening the review page on it.
Closing the tab without answering therefore does not bring the same crash back
on the next launch. Reload restores the failed page without adding a
crash-screen history entry; Back and Forward continue to use the original
session history. The crash overlay is native browser UI, so displaying it does
not require the replacement renderer to load a crash document.

The review page sends the report or declines it. **Don't send** leaves the
report on the device without sending it. Once the report is sent or declined,
the page offers **Close this tab**, and the next unanswered report when there
is one. Closing the only tab left would take the browser window with it, so the
page gives way to the new tab page instead.

The page sends no website URL by default. For a crashed web page, it can show
the failed URL locally without including it in the report. A user can
edit or censor the URL before explicitly choosing to include it, and may add a
description. The
report contains separate text and numeric signal fields, the native stack in a
`stack_trace` field, and the complete original diagnostic text as the
`crash-diagnostics.txt` attachment, so future server parsers can reinterpret
older submissions. The attachment text is available in the review page.
It does not collect contact information. Sending requests a proof-of-work
challenge, computes the proof locally, then uploads the report. The page shows
each stage in a progress bar after replacing the review form. The bar remains
visible through retries and the final result. Failed sends
are retried after three seconds, up to five times, before offering a manual
retry. The report stays on disk if the network or server fails. A
prepared manifest is saved before the first request so retries reuse the same
UUIDv7 submission ID and exact data even after restarting Ladybird. A successful
server receipt removes the local report and its submission metadata. Reports that
have not been sent remain on the device for review or a later retry.
For local development, `LADYBIRD_REPORTS_ENDPOINT` overrides the default
`https://reports.app.ladybird.org` endpoint.

Reports and filenames identify the process type. Build information includes the
full Git commit, tracked-source modification state, C++ compiler identity and
version, macOS SDK version when applicable, CMake build options, and flags from
the helper's compilation command. Include/output paths, string-valued defines
and arbitrary compiler arguments are omitted. The metadata refreshes on
incremental builds and does not require Git at runtime. A recovered
browser-process crash uses the version and build metadata of the instance that
recovers it, so those values may differ if Ladybird was upgraded before the next
launch. Source archives without Git metadata report an unknown revision; local
source modifications and `-march=native` builds still require the corresponding
source changes and build-machine target to reproduce.
The submission sends the Git commit, build configuration, and C++ compiler as
separate diagnostic fields. The complete saved diagnostic text is also attached.

Reports also contain the browser version, platform, architecture, numeric kernel
release, build configuration, helper process uptime, termination signal or exit
code,
signal code when available, and a bounded native stack. Stack frames identify
their binaries by Mach-O UUID on macOS or ELF build ID on Linux and contain
object addresses with the load relocation removed. When a binary is also loaded
in the surviving browser, its nearest available native symbol and the offset
from that symbol are included. Binary IDs identify builds, not users or devices.

Saved crash diagnostics do not collect page URLs, titles, content, JavaScript stacks, cookies,
network requests, console output, stderr, command lines, environment variables,
usernames, hostnames, installation paths, absolute source paths, general
register values, or memory dumps. Native symbol names containing paths or
non-printable characters are omitted. This also applies to crashes in private
windows.

Fatal `VERIFY` and `ASSERT` failures include their compile-time expression and
source location. Locations inside the checkout are repository-relative; external
locations include only the filename and line. Assertion text is bounded and does
not include evaluated operands or runtime page data. It is saved before terminal
formatting and backtrace generation, and remains available if those fail.

## Architecture

After a WebContent crash, the browser displays a native AppKit or Qt overlay and
retains the failed URL, title and committed history entry. The replacement
WebContent process remains dormant until the user chooses a recovery action.
The overlay provides reload and review actions directly in the browser process.

The browser creates an unlinked temporary file before spawning each helper and
passes a descriptor to the child. The child cannot access the report directory.
It snapshots native executable ranges and binary IDs before sandboxing. The
POSIX signal handler writes fixed-size records to the descriptor, starting with
the termination reason. It walks a bounded frame-pointer chain using Mach reads
on macOS and a preopened `/proc/self/mem` descriptor on Linux. Unreadable memory
ends the walk. Linux also has a bounded stack-scan fallback for code without
frame pointers. The handler does not allocate, format strings, acquire
application locks, or symbolize the stack. Signal termination is preserved so
the operating system can still handle the crash normally.

After process exit, the browser reads a bounded number of records and formats
the report. It never copies arbitrary child-process text into the report. Clean
exits, SIGTERM and SIGKILL do not produce reports. Other abnormal exits still
produce a minimal report when capture was unavailable. Helpers launched by a
test-mode browser do not produce automatic reports.

The browser process itself writes signal-safe records to a private, persistent
pending file. On the next launch, Ladybird formats a pending report whose header
contains a captured fatal signal, then removes the pending file. Clean exits
remove their pending file. Browser exits without a captured fatal signal do not
produce a report because they cannot reliably be distinguished from an
intentional termination. A recovered browser report omits process uptime, which
cannot be recovered accurately from the signal-safe records.

The capture implementation and bounded record format live in LibCore, so all
helpers can install the handler before sandboxing without linking browser UI
code. Browser-owned report formatting and storage remain in LibWebView. Windows
capture is not implemented yet; it will need native binary IDs, stack capture,
storage and exit-status handling.

## Limitations and symbolication

Only the crashing thread is captured. Stacks can be partial due to corruption,
missing frame pointers, JIT code, or modules loaded after handler
initialization. An alternate signal stack protects main-thread stack overflow;
stack overflow on other threads may only produce a minimal report. Early startup
crashes and other exits that bypass the handler also produce minimal
reports for helpers. Helper reports require the browser to survive long enough
to format them. Browser crashes before the handler is installed, or exits that
bypass its signal handler, may not produce a report. Disk errors can prevent saving; they
are reported to stderr. The report page needs a functioning replacement
WebContent process; the native crash overlay remains available if it cannot
load.

Keep the binaries and debug symbols for distributed builds. A binary ID and object
address remain useful even when symbols were stripped from the user's install.
On macOS, use `dwarfdump --uuid <binary>` to match the report's build ID
(ignoring hyphens and case), then `atos -arch arm64 -o <binary>
<object-address>` to resolve a frame with the matching binary and dSYM. Use
`x86_64` for Intel reports. Report addresses are already unslid, so do not add
the user's ASLR slide. The report itself contains no local binary paths.

On Linux, use `readelf -n <binary>` to match the ELF build ID, then
`addr2line -f -C -e <binary> <object-address>` with matching debug symbols.
