/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    static inline ByteString s_resource_map_path;

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        request_server_options.resource_substitution_map_path = s_resource_map_path;
        web_content_options.is_test_mode = WebView::IsTestMode::No;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

static ErrorOr<void> write_file(ByteString const& path, StringView contents)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(contents.bytes()));
    return {};
}

static u64 process_of(WebView::HeadlessWebView& view)
{
    return view.page().client().child_id();
}

static void load_and_wait(WebView::HeadlessWebView& view, URL::URL const& url)
{
    bool loaded = false;
    view.on_load_finish = [&](URL::URL const& finished_url) {
        if (finished_url == url)
            loaded = true;
    };
    view.load(url);
    Core::EventLoop::current().spin_until([&] { return loaded; });
}

static void run_and_wait_for_load(WebView::HeadlessWebView& view, String const& script, URL::URL const& url)
{
    bool loaded = false;
    view.on_load_finish = [&](URL::URL const& finished_url) {
        if (finished_url == url)
            loaded = true;
    };
    view.run_javascript(script);
    Core::EventLoop::current().spin_until([&] { return loaded; });
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestAboutBlankProcessReuse-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_directory, Core::Environment::Overwrite::Yes));

    auto page_path = ByteString::formatted("{}/page.html", test_directory);
    TRY(write_file(page_path, "<!DOCTYPE html><p>page</p>"sv));
    TestApplication::s_resource_map_path = ByteString::formatted("{}/resources.json", test_directory);
    StringBuilder map;
    map.append("{\"substitutions\":["sv);
    for (auto site : { "site-a"sv, "site-b"sv, "site-c"sv })
        map.appendff("{}{{\"url\":\"https://{}.test/\",\"file\":\"{}\",\"content_type\":\"text/html\"}}", site == "site-a"sv ? ""sv : ","sv, site, page_path);
    map.append("]}"sv);
    TRY(write_file(TestApplication::s_resource_map_path, map.string_view()));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto site_a = URL::Parser::basic_parse("https://site-a.test/"sv).release_value();
    auto site_b = URL::Parser::basic_parse("https://site-b.test/"sv).release_value();
    auto site_c = URL::Parser::basic_parse("https://site-c.test/"sv).release_value();
    auto local_file = URL::create_with_file_scheme(page_path).release_value();
    auto data_url = URL::Parser::basic_parse("data:text/html,<p>data</p>"sv).release_value();
    auto about_blank = URL::about_blank();

    // The about:blank the UI loads after a site's document gets a process of its own, which the next document takes,
    // whatever its site, local file content included.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        load_and_wait(*view, site_a);
        auto site_a_process = process_of(*view);
        load_and_wait(*view, about_blank);
        auto about_blank_process = process_of(*view);
        VERIFY(about_blank_process != site_a_process);
        load_and_wait(*view, site_b);
        VERIFY(process_of(*view) == about_blank_process);

        load_and_wait(*view, about_blank);
        about_blank_process = process_of(*view);
        VERIFY(about_blank_process != site_a_process);
        load_and_wait(*view, local_file);
        VERIFY(process_of(*view) == about_blank_process);
    }

    // An about:blank a document navigates to stays in that document's process, which is not the next site's.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        load_and_wait(*view, site_a);
        auto site_a_process = process_of(*view);
        run_and_wait_for_load(*view, "location.href = 'about:blank'"_string, about_blank);
        VERIFY(process_of(*view) == site_a_process);
        load_and_wait(*view, site_b);
        VERIFY(process_of(*view) != site_a_process);
    }

    // The about:blank the UI loads after a document of an opaque origin stays in that document's process, which has
    // hosted content.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        load_and_wait(*view, site_a);
        load_and_wait(*view, data_url);
        auto data_url_process = process_of(*view);
        load_and_wait(*view, about_blank);
        VERIFY(process_of(*view) == data_url_process);
        load_and_wait(*view, site_b);
        VERIFY(process_of(*view) != data_url_process);
    }

    // A process holding another page, such as a popup, is not taken over by the next site.
    {
        auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
        load_and_wait(*view, site_a);
        load_and_wait(*view, about_blank);
        auto about_blank_process = process_of(*view);

        OwnPtr<WebView::HeadlessWebView> popup;
        bool popup_loaded = false;
        view->on_new_web_view = [&](auto, auto, WebView::CanonicalTraversable& traversable) {
            popup = WebView::HeadlessWebView::create_child(*view, traversable);
            popup->on_load_finish = [&](auto const&) { popup_loaded = true; };
            return popup->handle();
        };
        view->run_javascript("window.open('about:blank')"_string);
        Core::EventLoop::current().spin_until([&] { return popup_loaded; });
        VERIFY(process_of(*popup) == about_blank_process);

        load_and_wait(*view, site_c);
        VERIFY(process_of(*view) != about_blank_process);
    }

    outln("PASS: only a process that has hosted no content is taken over by the next document");
    return 0;
}
