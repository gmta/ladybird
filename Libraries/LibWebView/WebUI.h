/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibWebView/Forward.h>
#include <WebContent/WebUIClientEndpoint.h>
#include <WebContent/WebUIServerEndpoint.h>

namespace WebView {

class WEBVIEW_API WebUI
    : public IPC::ConnectionToServer<WebUIClientEndpoint, WebUIServerEndpoint>
    , public WebUIClientEndpoint {
public:
    enum class PageType {
        Static,
        Dynamic,
    };

    struct Page {
        StringView host;
        StringView title;
        PageType type;
    };

    static ReadonlySpan<Page> pages();
    static Optional<Page const&> page_for_host(StringView);
    static ErrorOr<RefPtr<WebUI>> create(WebContentPage&, String host);
    virtual ~WebUI();

    String const& host() const { return m_host; }

protected:
    WebUI(WebContentPage&, NonnullOwnPtr<IPC::Transport>, String host);

    WebContentClient& client() const { return m_client; }
    // The tab this page is displayed in, if it is still around.
    Optional<ViewImplementation&> view() const;

    using Interface = Function<void(JsonValue)>;

    virtual void register_interfaces() { }
    void register_interface(StringView name, Interface);

private:
    virtual void die() override;
    virtual void received_message(String name, JsonValue data) override;

    WebContentClient& m_client;
    NonnullRefPtr<WebContentPage> m_page;
    String m_host;

    HashMap<StringView, Interface> m_interfaces;
};

#define WEB_UI(WebUIType)                                                                                              \
public:                                                                                                                \
    static NonnullRefPtr<WebUIType> create(WebContentPage& page, NonnullOwnPtr<IPC::Transport> transport, String host) \
    {                                                                                                                  \
        return adopt_ref(*new WebUIType(page, move(transport), move(host)));                                           \
    }                                                                                                                  \
                                                                                                                       \
private:                                                                                                               \
    WebUIType(WebContentPage& page, NonnullOwnPtr<IPC::Transport> transport, String host)                              \
        : WebView::WebUI(page, move(transport), move(host))                                                            \
    {                                                                                                                  \
    }

}
