const pageRoot = document.querySelector("#report");

const submissionStages = {
    preparing: { percent: 15, message: "Preparing report…" },
    challenge: { percent: 40, message: "Preparing report…" },
    proof: { percent: 65, message: "Preparing report…" },
    sending: { percent: 85, message: "Sending crash report…" },
};

let currentReport;
let submission;

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "crashReportLoaded") renderReport(data);
    if (name === "crashReportUnavailable") renderMessage("There is no crash report to review.");
    if (name === "crashReportError") renderMessage(data);
    if (name === "crashReportIgnored") renderIgnored();
    if (!submission) return;
    if (name === "crashReportPrepared") submission.report.manifestDigest = data;
    if (name === "crashReportProgress") {
        const stage = submissionStages[data.stage];
        if (stage) setProgress(submission.view, stage.percent, stage.message);
    }
    if (name === "crashReportRetrying") {
        setFeedback(
            submission.view.status,
            "sending",
            `${data.reason} Trying again in 3 seconds (${data.retry} of ${data.maximumRetries})…`
        );
    }
    if (name === "crashReportSent") finishSubmission(submission.view);
    if (name === "crashReportFailed") failSubmission(submission.view, data);
});

function reportTitle(process) {
    if (process === "Browser") return "Ladybird crashed";
    if (process === "WebContent" || process === "WebWorker") return "A web page crashed";
    return process ? `${process} crashed` : "Ladybird crashed";
}

function reportDate(name) {
    return /^\d{4}-\d{2}-\d{2}T\d{2}-\d{2}-\d{2}Z-/.test(name)
        ? `${name.slice(0, 10)} ${name.slice(11, 19).replaceAll("-", ":")} UTC`
        : "";
}

function addField(list, label, value) {
    if (!value) return;
    const row = document.createElement("div");
    row.className = "field-row";
    const name = document.createElement("dt");
    name.textContent = label;
    const content = document.createElement("dd");
    content.textContent = value;
    row.append(name, content);
    list.append(row);
}

function addWebsiteField(list, urlInput, includeUrl) {
    const row = document.createElement("div");
    row.className = "field-row website-field";
    const name = document.createElement("dt");
    name.textContent = "Website URL";
    const value = document.createElement("dd");
    const includeLabel = document.createElement("label");
    includeLabel.className = "checkbox-row";
    includeLabel.append(includeUrl, "Include this website URL in the report");
    value.append(urlInput, includeLabel);
    row.append(name, value);
    list.append(row);
}

function setFeedback(element, kind, message) {
    element.className = `status feedback ${kind}`;
    const text = document.createElement("span");
    text.textContent = message;
    if (kind === "sending") {
        element.replaceChildren(text);
        return;
    }
    const icon = document.createElement("span");
    icon.className = "feedback-icon";
    icon.setAttribute("aria-hidden", "true");
    element.replaceChildren(icon, text);
}

function renderTechnicalDetails(report, shownFields) {
    const details = document.createElement("details");
    details.className = "technical-details";
    const summary = document.createElement("summary");
    summary.textContent = "More report details";
    details.append(summary);

    if (report.frames.length) {
        const title = document.createElement("h3");
        title.textContent = "Stack trace";
        const frames = document.createElement("ol");
        frames.className = "stack-frames";
        for (const frame of report.frames) {
            const item = document.createElement("li");
            const symbol = document.createElement("span");
            symbol.className = "frame-symbol";
            symbol.textContent = frame.symbol;
            item.append(symbol);
            if (frame.binary) {
                const address = document.createElement("small");
                address.textContent = `${frame.binary} · ${frame.address}`;
                item.append(address);
            }
            frames.append(item);
        }
        details.append(title, frames);
    } else if (report.stack) {
        const stack = document.createElement("pre");
        stack.className = "raw-report";
        stack.textContent = report.stack;
        details.append(stack);
    }

    const metadata = document.createElement("dl");
    metadata.className = "technical-fields";
    for (const entry of report.headers) {
        if (shownFields.has(entry.name)) continue;
        if (
            (entry.name === "Termination signal name" || entry.name === "Captured signal name") &&
            entry.value === report.signalName
        )
            continue;
        addField(metadata, entry.name, entry.value);
    }
    details.append(metadata);

    if (report.preparedManifest) {
        const prepared = document.createElement("details");
        prepared.className = "original-text";
        const preparedSummary = document.createElement("summary");
        preparedSummary.textContent = "Prepared submission for this retry";
        const manifest = document.createElement("pre");
        manifest.className = "raw-report";
        manifest.textContent = JSON.stringify(JSON.parse(report.preparedManifest), null, 2);
        prepared.append(preparedSummary, manifest);
        details.append(prepared);
    }

    const original = document.createElement("details");
    original.className = "original-text";
    const originalSummary = document.createElement("summary");
    originalSummary.textContent = "Full diagnostics (.txt attachment)";
    const raw = document.createElement("pre");
    raw.className = "raw-report";
    raw.textContent = report.text;
    original.append(originalSummary, raw);
    details.append(original);
    return details;
}

function renderHeading(titleText) {
    const heading = document.createElement("div");
    heading.className = "report-heading";
    const logo = document.createElement("img");
    logo.src = "resource://icons/128x128/app-browser.png";
    logo.alt = "";
    const title = document.createElement("h2");
    title.textContent = titleText;
    heading.append(logo, title);
    return { heading, title };
}

function renderMessagePanel(titleText, message) {
    const panel = document.createElement("article");
    panel.className = "status-panel";
    const { heading } = renderHeading(titleText);
    const body = document.createElement("div");
    body.className = "status-body";
    const text = document.createElement("p");
    text.setAttribute("role", "status");
    text.textContent = message;
    body.append(text);
    panel.append(heading, body);
    return panel;
}

function renderMessage(message) {
    pageRoot.replaceChildren(renderMessagePanel("Crash report", message));
}

// Once this report is answered, the only things left to do are the next report and getting on with
// browsing. Both leave this page, so neither can be offered before the report is out of the way.
function appendFollowUpActions(body) {
    const actions = document.createElement("div");
    actions.className = "actions";
    if (currentReport.hasNextReport) {
        const next = document.createElement("button");
        next.className = "primary-button";
        next.textContent = "Review the next crash report";
        next.addEventListener("click", () => location.assign("about:crash-report"));
        actions.append(next);
    }
    const close = document.createElement("button");
    close.className = currentReport.hasNextReport ? "secondary-button" : "primary-button";
    close.textContent = "Close this tab";
    close.addEventListener("click", () => ladybird.sendMessage("closeCrashReportTab"));
    actions.append(close);
    body.append(actions);
}

function renderIgnored() {
    const panel = renderMessagePanel("Report not sent", "This report stays on your device and is not sent.");
    panel.classList.add("ignored-confirmation");
    appendFollowUpActions(panel.querySelector(".status-body"));
    pageRoot.replaceChildren(panel);
}

function createStatusPanel() {
    const panel = document.createElement("article");
    panel.className = "status-panel submission-panel";
    const { heading, title } = renderHeading("Sending crash report");
    const body = document.createElement("div");
    body.className = "status-body";
    const progress = document.createElement("div");
    progress.className = "progress-track";
    progress.setAttribute("role", "progressbar");
    progress.setAttribute("aria-label", "Report submission progress");
    progress.setAttribute("aria-valuemin", "0");
    progress.setAttribute("aria-valuemax", "100");
    const fill = document.createElement("div");
    fill.className = "progress-fill";
    progress.append(fill);
    const status = document.createElement("p");
    status.setAttribute("role", "status");
    body.append(progress, status);
    panel.append(heading, body);
    return { panel, title, body, progress, fill, status };
}

function setProgress(view, percent, message) {
    view.fill.style.width = `${percent}%`;
    view.progress.setAttribute("aria-valuenow", String(percent));
    view.progress.setAttribute("aria-valuetext", message);
    setFeedback(view.status, "sending", message);
}

function finishSubmission(view) {
    submission = undefined;
    view.panel.classList.add("complete", "sent-confirmation");
    view.title.textContent = "Report sent";
    view.fill.style.width = "100%";
    view.progress.setAttribute("aria-valuenow", "100");
    view.progress.setAttribute("aria-valuetext", "Complete");
    setFeedback(view.status, "success", "Thank you for helping Ladybird.");
    appendFollowUpActions(view.body);
}

function failSubmission(view, message) {
    const { report, description, url } = submission;
    submission = undefined;
    view.panel.classList.add("failed");
    view.title.textContent = "Couldn’t send report";
    setFeedback(view.status, "error", message);
    const retry = document.createElement("button");
    retry.className = "primary-button";
    if (message.startsWith("Could not prepare this report.")) {
        retry.textContent = "Review report again";
        retry.addEventListener("click", () => ladybird.sendMessage("loadCrashReport", report.name));
    } else {
        retry.textContent = "Try sending again";
        retry.addEventListener("click", () => startSubmission(report, description, url, view));
    }
    view.body.append(retry);
}

function startSubmission(report, description, url, existingView) {
    if (submission) return;
    const view = existingView || submissionPanel();
    submission = { report, description, url, view };
    view.panel.classList.remove("failed");
    view.title.textContent = "Sending crash report";
    view.body.querySelector(":scope > button")?.remove();
    setProgress(view, submissionStages.preparing.percent, submissionStages.preparing.message);
    ladybird.sendMessage("submitCrashReport", {
        name: report.name,
        description,
        url,
        textDigest: report.textDigest,
        manifestDigest: report.manifestDigest,
    });
}

function renderReport(report) {
    currentReport = report;
    const headers = new Map(report.headers.map(entry => [entry.name, entry.value]));
    const card = document.createElement("article");
    card.className = "report-card";
    const { heading } = renderHeading(reportTitle(report.process));

    const body = document.createElement("div");
    body.className = "report-body";
    const intro = document.createElement("div");
    intro.className = "report-intro";
    const introTitle = document.createElement("h3");
    introTitle.textContent = "Help us improve Ladybird";
    const explanation = document.createElement("p");
    explanation.textContent =
        "Something went wrong, and Ladybird caught it. " +
        "You can help us find and fix the problem by sending this report. " +
        "Nothing is sent unless you press Send.";
    intro.append(introTitle, explanation);
    const fields = document.createElement("dl");
    fields.className = "summary-fields";
    const shownFields = new Set(["Version", "Platform"]);
    addField(fields, "Crash date", reportDate(report.name));
    for (const key of ["Verification failed", "Assertion failed", "Rust panic", "Exit code"]) {
        if (headers.has(key)) {
            addField(fields, key, headers.get(key));
            shownFields.add(key);
            break;
        }
    }
    addField(fields, "Signal", report.signalName);
    addField(fields, "Ladybird version", headers.get("Version"));
    addField(fields, "Platform", headers.get("Platform"));
    body.append(intro, fields);

    body.append(renderTechnicalDetails(report, shownFields));
    renderSubmissionControls(report, body, fields);
    card.append(heading, body);
    pageRoot.replaceChildren(card);
}

function renderSubmissionControls(report, body, fields) {
    const options = document.createElement("div");
    options.className = "review-options";
    const descriptionLabel = document.createElement("label");
    descriptionLabel.textContent = "What happened just before the crash? (optional)";
    const description = document.createElement("textarea");
    description.maxLength = 16000;
    description.placeholder = "A few words about what you were doing can help us reproduce it.";
    descriptionLabel.append(description);

    const pageContext = new URLSearchParams(location.search);
    const urlInput = document.createElement("input");
    urlInput.type = "text";
    urlInput.inputMode = "url";
    urlInput.className = "website-value";
    urlInput.setAttribute("aria-label", "Website URL to include in the report");
    urlInput.autocomplete = "off";
    urlInput.spellcheck = false;
    urlInput.maxLength = 1024;
    urlInput.placeholder = "https://example.com/";
    if (pageContext.get("report") === report.name) urlInput.value = pageContext.get("website") || "";
    const includeUrl = document.createElement("input");
    includeUrl.type = "checkbox";
    options.append(descriptionLabel);

    function lockPreparedChoices() {
        description.disabled = true;
        urlInput.disabled = true;
        includeUrl.disabled = true;
        if (options.querySelector(".retry-note")) return;
        const note = document.createElement("p");
        note.className = "retry-note";
        note.textContent = "These choices are fixed for this retry so the report is not submitted twice.";
        options.append(note);
    }

    if (report.preparedUrl !== undefined) {
        description.value = report.preparedDescription;
        if (report.preparedUrl) urlInput.value = report.preparedUrl;
        includeUrl.checked = Boolean(report.preparedUrl);
        lockPreparedChoices();
    }
    addWebsiteField(fields, urlInput, includeUrl);

    const actions = document.createElement("div");
    actions.className = "actions";
    const send = document.createElement("button");
    send.className = "primary-button";
    send.textContent = "Send report to Ladybird";
    const ignore = document.createElement("button");
    ignore.className = "secondary-button";
    ignore.textContent = "Don’t send";
    const status = document.createElement("p");
    status.className = "status";
    status.setAttribute("role", "status");
    actions.append(send, ignore, status);

    ignore.addEventListener("click", () => {
        if (submission) return;
        ladybird.sendMessage("ignoreCrashReport", report.name);
    });

    send.addEventListener("click", () => {
        if (submission) return;
        const selectedUrl = includeUrl.checked ? urlInput.value.trim() : "";
        if (includeUrl.checked && !selectedUrl) {
            setFeedback(status, "error", "Enter a website URL or leave the option unchecked.");
            return;
        }
        if (selectedUrl.length > 1024) {
            setFeedback(status, "error", "This website URL is too long to include in a report.");
            return;
        }
        if (selectedUrl && !/^https?:\/\//i.test(selectedUrl)) {
            setFeedback(status, "error", "Enter an http or https URL.");
            return;
        }
        if (includeUrl.checked) urlInput.value = selectedUrl;
        startSubmission(report, description.value, selectedUrl);
    });
    body.append(options, actions);
}

function submissionPanel() {
    const view = createStatusPanel();
    pageRoot.replaceChildren(view.panel);
    return view;
}

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadCrashReport", new URLSearchParams(location.search).get("report"));
});
