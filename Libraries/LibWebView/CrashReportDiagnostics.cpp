/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/StringBuilder.h>
#include <LibWebView/CrashReportDiagnostics.h>

namespace WebView {

static constexpr auto stack_heading = "Native stack (binary build ID, object address):"sv;
static constexpr auto partial_stack_note = "Stacks may be partial."sv;

// The text report keeps the signal in one readable value. Split it here so the UI and report
// submission can expose the name and number independently. Separate lines remain supported for
// reports already written in that form.
static bool append_split_signal(Vector<CrashReportDiagnostics::Header>& headers, StringView name, StringView value)
{
    if (name != "Termination signal"sv && name != "Captured signal"sv)
        return false;

    auto separator = value.find(' ');
    if (!separator.has_value())
        return false;
    auto number = value.substring_view(*separator).trim_whitespace();
    if (!number.starts_with('(') || !number.ends_with(')'))
        return false;
    number = number.substring_view(1, number.length() - 2);
    if (number.is_empty() || !all_of(number, is_ascii_digit))
        return false;

    headers.append({ ByteString::formatted("{} name", name), value.substring_view(0, *separator) });
    headers.append({ ByteString::formatted("{} number", name), number });
    return true;
}

static CrashReportDiagnostics::Frame parse_frame(StringView text)
{
    // "<build id> <object address>[ <symbol>]", where a frame the browser could not describe has
    // neither, and one from an unloaded binary has no symbol.
    auto first_space = text.find(' ');
    if (first_space.has_value()) {
        auto binary = text.substring_view(0, *first_space);
        auto rest = text.substring_view(*first_space + 1).trim_whitespace();
        auto address_end = rest.find(' ').value_or(rest.length());
        auto address = rest.substring_view(0, address_end);
        if (!binary.is_empty() && all_of(binary, is_ascii_hex_digit) && address.starts_with("0x"sv)
            && address.length() > 2 && all_of(address.substring_view(2), is_ascii_hex_digit)) {
            return { binary, address, rest.substring_view(address_end).trim_whitespace() };
        }
    }
    return { {}, {}, text };
}

// The review page and submission manifest use the same parse, so submitted diagnostics are
// identical to those the user reviewed.
CrashReportDiagnostics CrashReportDiagnostics::parse(StringView text)
{
    CrashReportDiagnostics diagnostics;

    auto stack_start = text.find(ByteString::formatted("{}\n", stack_heading));
    auto header_section = stack_start.has_value() ? text.substring_view(0, *stack_start) : text;

    // The first line names the format rather than a field.
    auto lines = header_section.split_view('\n', SplitBehavior::KeepEmpty);
    for (size_t i = 1; i < lines.size(); ++i) {
        auto line = lines[i];

        // The first colon separates the two, so a name never contains one but a value may.
        if (auto separator = line.find(':'); separator.has_value() && *separator > 0) {
            auto name = line.substring_view(0, *separator);
            auto value = line.substring_view(*separator + 1).trim_whitespace();
            if (!append_split_signal(diagnostics.headers, name, value))
                diagnostics.headers.append({ name, value });
            continue;
        }

        // A value that wrapped onto its own line belongs to the field above it.
        if (!line.trim_whitespace().is_empty() && !diagnostics.headers.is_empty()) {
            auto& previous = diagnostics.headers.last();
            previous.value = ByteString::formatted("{}\n{}", previous.value, line);
        }
    }

    for (auto prefix : { "Termination signal"sv, "Captured signal"sv }) {
        auto name = diagnostics.header(ByteString::formatted("{} name", prefix));
        if (name.is_empty())
            continue;
        auto number = diagnostics.header(ByteString::formatted("{} number", prefix));
        diagnostics.signal = { move(name), number.view().to_number<u64>() };
        break;
    }

    if (!stack_start.has_value())
        return diagnostics;

    StringBuilder stack;
    for (auto line : text.substring_view(*stack_start).split_view('\n', SplitBehavior::KeepEmpty)) {
        if (line.trim_whitespace() == partial_stack_note)
            continue;
        stack.appendff("{}\n", line);

        if (!line.starts_with('#'))
            continue;
        auto separator = line.find(' ');
        if (!separator.has_value())
            continue;
        if (!all_of(line.substring_view(1, *separator - 1), is_ascii_digit))
            continue;
        diagnostics.frames.append(parse_frame(line.substring_view(*separator + 1).trim_whitespace()));
    }
    diagnostics.stack = stack.string_view().trim_whitespace();

    return diagnostics;
}

ByteString CrashReportDiagnostics::header(StringView name) const
{
    for (auto const& entry : headers) {
        if (entry.name == name)
            return entry.value;
    }
    return {};
}

}
