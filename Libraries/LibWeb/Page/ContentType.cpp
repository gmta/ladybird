/*
 * Copyright (c) 2026, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Page/ContentType.h>

namespace Web {

static Array const s_file_type_definitions {
    FileTypeDefinition {
        .file_type = FileType::PDF,
        .settings_key = "pdf"sv,
        .display_name = "PDF documents"sv,
        .mime_types = { "application/pdf"sv, "text/pdf"sv },
        .default_action = ContentTypeAction::View,
    },
};

ReadonlySpan<FileTypeDefinition const> file_type_definitions()
{
    return s_file_type_definitions.span();
}

ContentTypeSettings default_content_type_settings()
{
    ContentTypeSettings settings;
    for (auto const& definition : file_type_definitions())
        settings.actions.set(definition.file_type, definition.default_action);
    return settings;
}

Optional<FileType> file_type_from_mime_type(StringView mime_type)
{
    for (auto const& definition : file_type_definitions()) {
        if (definition.mime_types.contains_slow(mime_type))
            return definition.file_type;
    }
    return {};
}

StringView file_type_to_string(FileType file_type)
{
    for (auto const& definition : file_type_definitions()) {
        if (definition.file_type == file_type)
            return definition.settings_key;
    }
    VERIFY_NOT_REACHED();
}

Optional<FileType> file_type_from_string(StringView settings_key)
{
    for (auto const& definition : file_type_definitions()) {
        if (settings_key == definition.settings_key)
            return definition.file_type;
    }
    return {};
}

ContentTypeAction default_file_type_action(FileType file_type)
{
    for (auto const& definition : file_type_definitions()) {
        if (definition.file_type == file_type)
            return definition.default_action;
    }
    VERIFY_NOT_REACHED();
}

bool file_type_supports_inline_viewing(FileType file_type)
{
    return default_file_type_action(file_type) == ContentTypeAction::View;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::ContentTypeSettings const& settings)
{
    TRY(encoder.encode(settings.default_action));
    TRY(encoder.encode(settings.actions));
    return {};
}

template<>
ErrorOr<Web::ContentTypeSettings> decode(Decoder& decoder)
{
    auto default_action = TRY(decoder.decode<Web::ContentTypeAction>());
    auto actions = TRY((decoder.decode<HashMap<Web::FileType, Web::ContentTypeAction>>()));

    return Web::ContentTypeSettings {
        .default_action = default_action,
        .actions = move(actions),
    };
}

}
