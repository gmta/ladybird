/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

Vector<pid_t> image_decoders_of(WebView::HeadlessWebView& view)
{
    auto& process_manager = WebView::Application::process_manager();
    auto renderer = process_manager.find_process(view.web_content_pid());
    if (!renderer.has_value())
        return {};

    Vector<pid_t> decoders;
    process_manager.for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::ImageDecoder && process.owner_child_id() == renderer->child_id())
            decoders.append(process.pid());
    });
    return decoders;
}

void load_image(WebView::HeadlessWebView& view, StringView png_base64)
{
    view.load(URL::Parser::basic_parse(ByteString::formatted("data:text/html,<img src=\"data:image/png;base64,{}\">", png_base64)).release_value());
}

constexpr auto red_pixel_png = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"sv;
constexpr auto blue_square_png = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAD0lEQVR4nGNgYPgPRmAKABf2A/1+6zfzAAAAAElFTkSuQmCC"sv;

}

// An ImageDecoder parses untrusted images, so each renderer has one of its own, and never sees another's images. A
// decoder that crashes is replaced for its renderer alone, once that renderer has another image to decode.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestImageDecoderIsolation-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto first_view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto second_view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    // A view drops a URL it is asked to load before its initial about:blank has loaded.
    size_t loads = 0;
    first_view->on_load_finish = [&](auto const&) { ++loads; };
    second_view->on_load_finish = [&](auto const&) { ++loads; };
    Core::EventLoop::current().spin_until([&] { return loads == 2; });

    // A page only finishes loading once its image is decoded.
    load_image(*first_view, red_pixel_png);
    load_image(*second_view, red_pixel_png);
    Core::EventLoop::current().spin_until([&] { return loads == 4; });

    auto first_decoders = image_decoders_of(*first_view);
    auto second_decoders = image_decoders_of(*second_view);
    VERIFY(first_decoders.size() == 1);
    VERIFY(second_decoders.size() == 1);
    VERIFY(first_decoders.first() != second_decoders.first());

    auto crashed_decoder = first_decoders.first();
    TRY(Core::System::kill(crashed_decoder, SIGKILL));

    // The renderer asks for a new decoder once it has another image to decode.
    load_image(*first_view, blue_square_png);
    Core::EventLoop::current().spin_until([&] {
        return any_of(image_decoders_of(*first_view), [&](pid_t decoder) { return decoder != crashed_decoder; });
    });
    VERIFY(image_decoders_of(*second_view) == second_decoders);

    outln("PASS: each renderer has an image decoder of its own, which is replaced alone when it crashes");
    return 0;
}
