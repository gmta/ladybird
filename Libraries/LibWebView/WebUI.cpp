/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <LibWebView/WebUI.h>
#include <LibWebView/WebUI/BookmarksUI.h>
#include <LibWebView/WebUI/DownloadsUI.h>
#include <LibWebView/WebUI/HistoryUI.h>
#include <LibWebView/WebUI/SettingsUI.h>
#include <LibWebView/WebUI/VersionUI.h>

namespace WebView {

static constexpr auto s_pages = to_array<WebUI::Page>({
    { "about"sv, "About URLs"sv, WebUI::PageType::Static },
    { "blocking"sv, "Blocking"sv, WebUI::PageType::Static },
    { "bookmarks"sv, "Bookmarks"sv, WebUI::PageType::Dynamic },
    { "downloads"sv, "Downloads"sv, WebUI::PageType::Dynamic },
    { "history"sv, "History"sv, WebUI::PageType::Dynamic },
    { "newtab"sv, "New Tab"sv, WebUI::PageType::Static },
    { "settings"sv, "Settings"sv, WebUI::PageType::Dynamic },
    { "services"sv, "Services"sv, WebUI::PageType::Static },
    { "version"sv, "Version"sv, WebUI::PageType::Dynamic },
});

ReadonlySpan<WebUI::Page> WebUI::pages()
{
    return s_pages;
}

Optional<WebUI::Page const&> WebUI::page_for_host(StringView host)
{
    for (auto const& page : s_pages) {
        if (page.host == host)
            return page;
    }
    return {};
}

template<typename WebUIType>
static ErrorOr<NonnullRefPtr<WebUIType>> create_web_ui(WebContentPage& page, String host)
{
    auto paired = TRY(IPC::Transport::create_paired());
    auto handle = move(paired.remote_handle);

    auto web_ui = WebUIType::create(page, move(paired.local), move(host));
    page.client().async_connect_to_web_ui(page.id(), move(handle));

    return web_ui;
}

ErrorOr<RefPtr<WebUI>> WebUI::create(WebContentPage& web_content_page, String host)
{
    auto page_info = page_for_host(host);
    if (!page_info.has_value() || page_info->type == PageType::Static)
        return nullptr;

    RefPtr<WebUI> web_ui;

    if (page_info->host == "bookmarks"sv)
        web_ui = TRY(create_web_ui<BookmarksUI>(web_content_page, move(host)));
    else if (page_info->host == "downloads"sv)
        web_ui = TRY(create_web_ui<DownloadsUI>(web_content_page, move(host)));
    else if (page_info->host == "history"sv)
        web_ui = TRY(create_web_ui<HistoryUI>(web_content_page, move(host)));
    else if (page_info->host == "settings"sv)
        web_ui = TRY(create_web_ui<SettingsUI>(web_content_page, move(host)));
    else if (page_info->host == "version"sv)
        web_ui = TRY(create_web_ui<VersionUI>(web_content_page, move(host)));

    VERIFY(web_ui);
    web_ui->register_interfaces();

    return web_ui;
}

WebUI::WebUI(WebContentPage& page, NonnullOwnPtr<IPC::Transport> transport, String host)
    : IPC::ConnectionToServer<WebUIClientEndpoint, WebUIServerEndpoint>(*this, move(transport))
    , m_client(page.client())
    , m_page(page)
    , m_host(move(host))
{
}

Optional<ViewImplementation&> WebUI::view() const
{
    if (!m_page->is_open() || !m_page->displays_tab())
        return {};
    return m_page->view();
}

WebUI::~WebUI() = default;

void WebUI::die()
{
    m_client.web_ui_disconnected({});
}

void WebUI::register_interface(StringView name, Interface interface)
{
    auto result = m_interfaces.set(name, move(interface));
    VERIFY(result == HashSetResult::InsertedNewEntry);
}

void WebUI::received_message(String name, JsonValue data)
{
    auto interface = m_interfaces.get(name);
    if (!interface.has_value()) {
        warnln("Received message from WebUI for unrecognized interface: {}", name);
        return;
    }

    interface.value()(move(data));
}

}
