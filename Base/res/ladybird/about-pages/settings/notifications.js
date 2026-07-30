const enableNotifications = document.querySelector("#enable-notifications");

enableNotifications.addEventListener("change", () => {
    ladybird.sendMessage("setNotificationsEnabled", enableNotifications.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        enableNotifications.disabled = event.detail.data.notifications !== true;
        return;
    }

    if (event.detail.name !== "loadSettings") return;

    enableNotifications.checked = event.detail.data.notificationsEnabled === true;
});
