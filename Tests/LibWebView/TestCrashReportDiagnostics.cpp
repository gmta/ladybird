/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/CrashReportDiagnostics.h>

using WebView::CrashReportDiagnostics;

static constexpr auto stack_heading = "Native stack (binary build ID, object address):"sv;

TEST_CASE(named_lines_are_read_in_order)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Process: WebContent\n"
                                                     "Version: 1.0\n"
                                                     "Platform: macOS\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 3u);
    EXPECT_EQ(diagnostics.headers[0].name, "Process"sv);
    EXPECT_EQ(diagnostics.headers[0].value, "WebContent"sv);
    EXPECT_EQ(diagnostics.headers[2].name, "Platform"sv);
    EXPECT_EQ(diagnostics.header("Version"sv), "1.0"sv);

    // The first line names the format rather than a field.
    EXPECT_EQ(diagnostics.header("Ladybird crash report, format 1"sv), ""sv);
    EXPECT_EQ(diagnostics.header("Architecture"sv), ""sv);
}

TEST_CASE(a_value_may_contain_colons)
{
    auto diagnostics = CrashReportDiagnostics::parse(
        "Ladybird crash report, format 1\n"
        "Verification failed: value != 0 at Services/WebContent/Foo.cpp:123\n"sv);

    EXPECT_EQ(diagnostics.header("Verification failed"sv), "value != 0 at Services/WebContent/Foo.cpp:123"sv);
}

TEST_CASE(a_wrapped_value_belongs_to_the_field_above_it)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Build options: first\n"
                                                     "    second\n"
                                                     "Process: WebContent\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 2u);
    EXPECT_EQ(diagnostics.header("Build options"sv), "first\n    second"sv);
    EXPECT_EQ(diagnostics.header("Process"sv), "WebContent"sv);
}

TEST_CASE(a_signal_is_read_from_its_own_name_and_number_lines)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal name: SIGSEGV\n"
                                                     "Termination signal number: 11\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGSEGV"sv);
    EXPECT_EQ(diagnostics.signal.number, 11u);
}

TEST_CASE(a_signal_written_by_an_older_build_is_split_apart)
{
    // Reports saved before the name and number were split are still on disk after an upgrade.
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal: SIGTRAP (5)\n"
                                                     "Captured signal: SIGTRAP (5)\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 4u);
    EXPECT_EQ(diagnostics.headers[0].name, "Termination signal name"sv);
    EXPECT_EQ(diagnostics.headers[0].value, "SIGTRAP"sv);
    EXPECT_EQ(diagnostics.headers[1].name, "Termination signal number"sv);
    EXPECT_EQ(diagnostics.headers[1].value, "5"sv);
    EXPECT_EQ(diagnostics.headers[2].name, "Captured signal name"sv);
    EXPECT_EQ(diagnostics.headers[3].name, "Captured signal number"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGTRAP"sv);
    EXPECT_EQ(diagnostics.signal.number, 5u);
}

TEST_CASE(a_signal_line_that_does_not_carry_a_number_is_left_alone)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal: SIGSEGV\n"
                                                     "Captured signal: unknown (not a number)\n"sv);

    EXPECT_EQ(diagnostics.headers.size(), 2u);
    EXPECT_EQ(diagnostics.headers[0].name, "Termination signal"sv);
    EXPECT_EQ(diagnostics.headers[1].name, "Captured signal"sv);
    EXPECT_EQ(diagnostics.signal.name, ""sv);
    EXPECT(!diagnostics.signal.number.has_value());
}

TEST_CASE(a_captured_signal_is_used_when_the_report_has_no_termination_signal)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Exit code: 3\n"
                                                     "Captured signal name: SIGABRT\n"
                                                     "Captured signal number: 6\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGABRT"sv);
    EXPECT_EQ(diagnostics.signal.number, 6u);
}

TEST_CASE(a_termination_signal_is_preferred_over_a_captured_one)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Termination signal name: SIGSEGV\n"
                                                     "Termination signal number: 11\n"
                                                     "Captured signal name: SIGABRT\n"
                                                     "Captured signal number: 6\n"sv);

    EXPECT_EQ(diagnostics.signal.name, "SIGSEGV"sv);
    EXPECT_EQ(diagnostics.signal.number, 11u);
}

TEST_CASE(stack_frames_are_split_into_binary_address_and_symbol)
{
    auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\n"
                                                                           "Process: WebContent\n"
                                                                           "\n{}\n"
                                                                           "#0 a1b2c3 0x1000 first_symbol + 0x10\n"
                                                                           "#1 d4e5f6 0x2000\n"
                                                                           "\nStacks may be partial.\n",
        stack_heading));

    EXPECT_EQ(diagnostics.frames.size(), 2u);
    EXPECT_EQ(diagnostics.frames[0].binary, "a1b2c3"sv);
    EXPECT_EQ(diagnostics.frames[0].address, "0x1000"sv);
    EXPECT_EQ(diagnostics.frames[0].symbol, "first_symbol + 0x10"sv);
    EXPECT_EQ(diagnostics.frames[1].binary, "d4e5f6"sv);
    EXPECT_EQ(diagnostics.frames[1].address, "0x2000"sv);
    EXPECT_EQ(diagnostics.frames[1].symbol, ""sv);

    // A header line below the stack heading is part of the stack, not a field.
    EXPECT_EQ(diagnostics.headers.size(), 1u);
}

TEST_CASE(a_frame_that_cannot_be_split_keeps_its_whole_line)
{
    auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\n"
                                                                           "\n{}\n"
                                                                           "#0 unavailable\n"
                                                                           "#1 not-hex 0x2000 symbol\n"
                                                                           "not a frame at all\n",
        stack_heading));

    EXPECT_EQ(diagnostics.frames.size(), 2u);
    EXPECT_EQ(diagnostics.frames[0].binary, ""sv);
    EXPECT_EQ(diagnostics.frames[0].symbol, "unavailable"sv);
    EXPECT_EQ(diagnostics.frames[1].binary, ""sv);
    EXPECT_EQ(diagnostics.frames[1].symbol, "not-hex 0x2000 symbol"sv);
}

TEST_CASE(the_stack_keeps_its_heading_and_drops_the_note_about_partial_stacks)
{
    auto diagnostics = CrashReportDiagnostics::parse(ByteString::formatted("Ladybird crash report, format 1\n"
                                                                           "Process: WebContent\n"
                                                                           "\n{}\n"
                                                                           "#0 a1b2c3 0x1000 symbol\n"
                                                                           "\nStacks may be partial.\n",
        stack_heading));

    EXPECT(diagnostics.stack.starts_with(stack_heading));
    EXPECT(diagnostics.stack.contains("#0 a1b2c3 0x1000 symbol"sv));
    EXPECT(!diagnostics.stack.contains("Stacks may be partial."sv));
    EXPECT(!diagnostics.stack.contains("Process: WebContent"sv));
}

TEST_CASE(a_report_without_a_stack_section_still_reads_its_fields)
{
    auto diagnostics = CrashReportDiagnostics::parse("Ladybird crash report, format 1\n"
                                                     "Process: WebContent\n"
                                                     "Unavailable: the process exited without a captured native stack.\n"sv);

    EXPECT_EQ(diagnostics.header("Process"sv), "WebContent"sv);
    EXPECT(diagnostics.frames.is_empty());
    EXPECT(diagnostics.stack.is_empty());
}

TEST_CASE(an_empty_report_parses_to_nothing)
{
    auto diagnostics = CrashReportDiagnostics::parse(""sv);
    EXPECT(diagnostics.headers.is_empty());
    EXPECT(diagnostics.frames.is_empty());
    EXPECT(diagnostics.stack.is_empty());
    EXPECT_EQ(diagnostics.signal.name, ""sv);
}
