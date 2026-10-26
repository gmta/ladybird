/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/ProcessLauncherPOSIX.h>

namespace WebView {

class WEBVIEW_API ProcessLauncherSocket final : public ProcessLauncherPOSIX {
private:
    virtual ErrorOr<SpawnedProcess> spawn(ProcessType, Core::ProcessSpawnOptions&) override;
};

}
