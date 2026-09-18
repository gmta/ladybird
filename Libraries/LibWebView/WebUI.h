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
#include <LibWeb/Page/PageId.h>
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
    static ErrorOr<RefPtr<WebUI>> create(WebContentClient&, Web::PageId page_id, String host);
    virtual ~WebUI();

    String const& host() const { return m_host; }

protected:
    WebUI(WebContentClient&, NonnullOwnPtr<IPC::Transport>, String host, Web::PageId);

    WebContentClient& client() const { return m_client; }
    Web::PageId page_id() const { return m_page_id; }

    // The tab this page is displayed in, if it is still around.
    Optional<ViewImplementation&> view() const;

    using Interface = Function<void(JsonValue)>;

    virtual void register_interfaces() { }
    void register_interface(StringView name, Interface);

private:
    virtual void die() override;
    virtual void received_message(String name, JsonValue data) override;

    WebContentClient& m_client;
    String m_host;
    Web::PageId m_page_id { 0 };

    HashMap<StringView, Interface> m_interfaces;
};

#define WEB_UI(WebUIType)                                                                                                  \
public:                                                                                                                    \
    static NonnullRefPtr<WebUIType> create(WebContentClient& client, NonnullOwnPtr<IPC::Transport> transport, String host, Web::PageId page_id) \
    {                                                                                                                      \
        return adopt_ref(*new WebUIType(client, move(transport), move(host), page_id));                                    \
    }                                                                                                                      \
                                                                                                                           \
private:                                                                                                                   \
    WebUIType(WebContentClient& client, NonnullOwnPtr<IPC::Transport> transport, String host, Web::PageId page_id)         \
        : WebView::WebUI(client, move(transport), move(host), page_id)                                                     \
    {                                                                                                                      \
    }

}
