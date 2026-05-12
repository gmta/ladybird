const fileTypeActions = document.querySelector("#file-type-actions");

let FILE_TYPE_ACTIONS = {};

const loadSettings = settings => {
    FILE_TYPE_ACTIONS = settings.fileTypeActions || {};
    renderFileTypeActions(settings.fileTypes || []);
};

function makeActionSelect(value, includeView = true) {
    const select = document.createElement("select");

    if (includeView) {
        const view = document.createElement("option");
        view.value = "view";
        view.textContent = "Open in Ladybird";
        select.appendChild(view);
    }

    const download = document.createElement("option");
    download.value = "download";
    download.textContent = "Download";
    select.appendChild(download);

    const ask = document.createElement("option");
    ask.value = "ask";
    ask.textContent = "Ask every time";
    select.appendChild(ask);

    select.value = value;
    return select;
}

function renderFileTypeActions(fileTypes) {
    fileTypeActions.replaceChildren();

    Array.from(fileTypes)
        .sort((a, b) => a.label.localeCompare(b.label))
        .forEach(fileType => {
            const row = document.createElement("tr");

            const label = document.createElement("td");
            label.textContent = fileType.label;

            const action = document.createElement("td");
            const select = makeActionSelect(FILE_TYPE_ACTIONS[fileType.key] || FILE_TYPE_ACTIONS["*"] || "ask");
            select.addEventListener("change", () => {
                FILE_TYPE_ACTIONS[fileType.key] = select.value;
                saveFileTypeActions();
            });
            action.appendChild(select);

            row.appendChild(label);
            row.appendChild(action);
            fileTypeActions.appendChild(row);
        });

    const defaultRow = document.createElement("tr");

    const defaultLabel = document.createElement("td");
    defaultLabel.textContent = "All other file types";

    const defaultAction = document.createElement("td");
    const defaultSelect = makeActionSelect(FILE_TYPE_ACTIONS["*"] || "ask", false);
    defaultSelect.addEventListener("change", () => {
        FILE_TYPE_ACTIONS["*"] = defaultSelect.value;
        saveFileTypeActions();
    });
    defaultAction.appendChild(defaultSelect);

    defaultRow.appendChild(defaultLabel);
    defaultRow.appendChild(defaultAction);
    fileTypeActions.appendChild(defaultRow);
}

function saveFileTypeActions() {
    ladybird.sendMessage("setContentTypeSettings", FILE_TYPE_ACTIONS);
}

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
