/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CompositorClient.h>

namespace WebView {

CompositorClient::CompositorClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorControlClientEndpoint, CompositorControlServerEndpoint>(*this, move(transport))
{
}

// The browser carries on without the Compositor: ProcessManager reports the loss, and the Compositor is restarted.
void CompositorClient::die()
{
}

void CompositorClient::gpu_device_identity_reported(Optional<Gfx::GpuDeviceIdentity> identity)
{
    if (on_gpu_device_identity_reported)
        on_gpu_device_identity_reported(move(identity));
}

}
