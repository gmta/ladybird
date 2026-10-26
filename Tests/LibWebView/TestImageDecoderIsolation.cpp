/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
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

HashMap<pid_t, u64> renderers_by_image_decoder()
{
    HashMap<pid_t, u64> renderers;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::ImageDecoder && process.owner_child_id().has_value())
            renderers.set(process.pid(), *process.owner_child_id());
    });
    return renderers;
}

}

// An ImageDecoder parses untrusted images, so each renderer has one of its own, and never sees another's images. A
// decoder that crashes is replaced for its renderer alone.

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

    auto renderers = renderers_by_image_decoder();
    HashTable<u64> renderers_with_a_decoder;
    for (auto const& [decoder, renderer] : renderers)
        renderers_with_a_decoder.set(renderer);
    VERIFY(renderers_with_a_decoder.size() >= 2);
    VERIFY(renderers_with_a_decoder.size() == renderers.size());

    auto [crashed_decoder, crashed_renderer] = *renderers.begin();
    TRY(Core::System::kill(crashed_decoder, SIGKILL));

    // The replacement is launched as the Browser learns that the decoder died, so this cannot finish without it.
    Core::EventLoop::current().spin_until([&] {
        for (auto const& [decoder, renderer] : renderers_by_image_decoder()) {
            if (renderer == crashed_renderer && decoder != crashed_decoder)
                return true;
        }
        return false;
    });

    auto renderers_after_crash = renderers_by_image_decoder();
    for (auto const& [decoder, renderer] : renderers) {
        if (decoder != crashed_decoder)
            VERIFY(renderers_after_crash.get(decoder) == renderer);
    }

    outln("PASS: each renderer has an image decoder of its own, which is replaced alone when it crashes");
    return 0;
}
