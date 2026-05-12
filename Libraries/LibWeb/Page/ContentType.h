/*
 * Copyright (c) 2026, Ladybird contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Export.h>

namespace Web {

enum class ContentTypeAction : u8 {
    View,
    Download,
    Ask,
};

enum class FileType : u8 {
    PDF,
};

struct ContentTypeSettings {
    ContentTypeAction default_action { ContentTypeAction::Ask };
    HashMap<FileType, ContentTypeAction> actions;
};

struct FileTypeDefinition {
    FileType file_type;
    StringView settings_key;
    StringView display_name;
    Vector<StringView> mime_types;
    ContentTypeAction default_action;
};

WEB_API ReadonlySpan<FileTypeDefinition const> file_type_definitions();
WEB_API ContentTypeSettings default_content_type_settings();
WEB_API Optional<FileType> file_type_from_mime_type(StringView);
WEB_API StringView file_type_to_string(FileType);
WEB_API Optional<FileType> file_type_from_string(StringView);
WEB_API ContentTypeAction default_file_type_action(FileType);
WEB_API bool file_type_supports_inline_viewing(FileType);

}

namespace IPC {

class Decoder;
class Encoder;

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::ContentTypeSettings const&);

template<>
WEB_API ErrorOr<Web::ContentTypeSettings> decode(Decoder&);

}
