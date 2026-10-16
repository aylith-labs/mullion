// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "../WindowsTerminal/ControlPipeProtocol.h"

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace ControlUnitTests
{
    // The control pipe's wire format is a contract with clients that live
    // outside this repo (see doc/control-pipe.md), so the parsing gets to be
    // tested on its own, without a Terminal anywhere near it.
    class ControlPipeProtocolTests
    {
        BEGIN_TEST_CLASS(ControlPipeProtocolTests)
        END_TEST_CLASS()

        TEST_METHOD(PaneAddressRoundTrips);
        TEST_METHOD(PaneAddressRejectsGarbage);
        TEST_METHOD(ParsesEveryOp);
        TEST_METHOD(SessionIdParsesBothForms);
        TEST_METHOD(RejectsBadRequests);
        TEST_METHOD(KeepsTextVerbatim);
        TEST_METHOD(IgnoresUnknownMembers);
        TEST_METHOD(FormatsResponses);
        TEST_METHOD(DescribesEveryOp);
        TEST_METHOD(RecordedTextSpellsOutControlCharacters);
        TEST_METHOD(RecordedTextIsCapped);
        TEST_METHOD(DescribesUnparsedLines);
        TEST_METHOD(RequiresReasonAndCaller);
    };

    void ControlPipeProtocolTests::PaneAddressRoundTrips()
    {
        for (const auto& text : { "0.0.0", "1.0.3", "12.34.56", "18446744073709551615.4294967295.4294967295" })
        {
            const auto parsed = ControlPipe::ParsePaneAddress(text);
            VERIFY_IS_TRUE(parsed.has_value(), NoThrowString().Format(L"parsing %hs", text));
            VERIFY_ARE_EQUAL(std::string{ text }, ControlPipe::FormatPaneAddress(*parsed));
        }

        const auto parsed = ControlPipe::ParsePaneAddress("7.2.9");
        VERIFY_IS_TRUE(parsed.has_value());
        VERIFY_ARE_EQUAL(7u, static_cast<unsigned>(parsed->window));
        VERIFY_ARE_EQUAL(2u, parsed->tab);
        VERIFY_ARE_EQUAL(9u, parsed->pane);
    }

    void ControlPipeProtocolTests::PaneAddressRejectsGarbage()
    {
        // A pane id we can't be sure of is a pane we must not write to, so
        // every one of these has to fail rather than land on a default.
        for (const auto& text : { "", "1", "1.2", "1.2.3.4", "1..3", ".2.3", "1.2.", "-1.2.3", "+1.2.3", "1.2.3 ", " 1.2.3", "a.b.c", "1.2.0x3", "99999999999999999999.1.1" })
        {
            VERIFY_IS_FALSE(ControlPipe::ParsePaneAddress(text).has_value(), NoThrowString().Format(L"parsing %hs", text));
        }
    }

    void ControlPipeProtocolTests::ParsesEveryOp()
    {
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"ping"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::Ping);
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"list-panes"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::ListPanes);
            VERIFY_IS_TRUE(request->containing.empty());
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"list-panes","containing":"6f1a2b3c"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"6f1a2b3c" }, request->containing);
        }
        {
            // No `lines` means the whole viewport, which we spell 0.
            const auto request = ControlPipe::ParseRequest(R"({"op":"capture-pane","pane":"1.0.3"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::CapturePane);
            VERIFY_ARE_EQUAL(0, request->lines);
            VERIFY_ARE_EQUAL(1u, static_cast<unsigned>(request->pane->window));
            VERIFY_ARE_EQUAL(3u, request->pane->pane);
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"capture-pane","pane":"1.0.3","lines":80})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(80, request->lines);
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"2.1.4","text":"hello","requireContains":"needle"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::SendInput);
            VERIFY_ARE_EQUAL(std::wstring{ L"hello" }, request->text);
            VERIFY_ARE_EQUAL(std::wstring{ L"needle" }, request->requireContains);
        }
        {
            // An empty send is legal and writes nothing; it must not be
            // mistaken for a missing field.
            const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"2.1.4","text":""})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->text.empty());
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"focus-pane","session":"{6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8}"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::FocusPane);
            VERIFY_IS_TRUE(request->session.has_value());
            VERIFY_IS_FALSE(request->pane.has_value());
        }
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"focus-pane","pane":"1.2.3"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_TRUE(request->op == ControlPipe::Op::FocusPane);
            VERIFY_IS_TRUE(request->pane.has_value());
            VERIFY_IS_FALSE(request->session.has_value());
        }
        {
            // Every other op ignores `session` like any unknown member, even a malformed one.
            const auto request = ControlPipe::ParseRequest(R"({"op":"ping","session":5})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_IS_FALSE(request->session.has_value());
        }
    }

    void ControlPipeProtocolTests::SessionIdParsesBothForms()
    {
        const auto braced = ControlPipe::ParseSessionId("{6F1A2B3C-4D5E-6F70-8192-A3B4C5D6E7F8}");
        const auto bare = ControlPipe::ParseSessionId("6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8");
        VERIFY_IS_TRUE(braced.has_value());
        VERIFY_IS_TRUE(bare.has_value());
        VERIFY_IS_TRUE(*braced == *bare);

        // Field order matters: the first group is Data1, not the first four bytes in memory order.
        VERIFY_ARE_EQUAL(0x6f1a2b3cu, static_cast<unsigned>(bare->Data1));
        VERIFY_ARE_EQUAL(0x4d5eu, static_cast<unsigned>(bare->Data2));
        VERIFY_ARE_EQUAL(0x6f70u, static_cast<unsigned>(bare->Data3));
        VERIFY_ARE_EQUAL(0x81u, static_cast<unsigned>(bare->Data4[0]));
        VERIFY_ARE_EQUAL(0x92u, static_cast<unsigned>(bare->Data4[1]));
        VERIFY_ARE_EQUAL(0xf8u, static_cast<unsigned>(bare->Data4[7]));

        for (const auto& text : { "",
                                  "{}",
                                  "6f1a2b3c4d5e6f708192a3b4c5d6e7f8",
                                  "{6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8",
                                  "6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8}",
                                  "6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7fg",
                                  "6f1a2b3c_4d5e-6f70-8192-a3b4c5d6e7f8",
                                  " 6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f",
                                  "{6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8}x" })
        {
            VERIFY_IS_FALSE(ControlPipe::ParseSessionId(text).has_value(), NoThrowString().Format(L"parsing %hs", text));
        }
    }

    void ControlPipeProtocolTests::RejectsBadRequests()
    {
        const char* const cases[]{
            "",
            "not json",
            "[]",
            R"("op")",
            R"({})",
            R"({"op":"nope"})",
            R"({"op":5})",
            // A pane op with no pane, or a pane we can't parse.
            R"({"op":"capture-pane"})",
            R"({"op":"capture-pane","pane":"nonsense"})",
            R"({"op":"capture-pane","pane":1})",
            R"({"op":"send-input","pane":"1.0.0"})",
            R"({"op":"send-input","text":"hi"})",
            // Wrong types, rather than something we should coerce.
            R"({"op":"send-input","pane":"1.0.0","text":5})",
            R"({"op":"list-panes","containing":true})",
            R"({"op":"capture-pane","pane":"1.0.0","lines":"80"})",
            R"({"op":"capture-pane","pane":"1.0.0","lines":true})",
            // focus-pane names its pane exactly one way, and names it validly.
            R"({"op":"focus-pane"})",
            R"({"op":"focus-pane","session":"{6f1a2b3c-4d5e-6f70-8192-a3b4c5d6e7f8}","pane":"1.0.0"})",
            R"({"op":"focus-pane","session":"not-a-guid"})",
            R"({"op":"focus-pane","session":5})",
            R"({"op":"focus-pane","pane":"nonsense"})",
            // Two objects on one line is not one request.
            R"({"op":"ping"}{"op":"ping"})",
        };

        for (const auto& text : cases)
        {
            VERIFY_IS_FALSE(ControlPipe::ParseRequest(text).has_value(), NoThrowString().Format(L"parsing %hs", text));
        }
    }

    void ControlPipeProtocolTests::KeepsTextVerbatim()
    {
        // These are exactly the characters SendKeys mangles, which is the whole
        // reason the pipe exists. They have to survive the trip untouched.
        {
            const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"1.0.0","text":"+ ^ % ~ ( ) { } [ ]"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"+ ^ % ~ ( ) { } [ ]" }, request->text);
        }
        {
            // é, a CJK character and an astral-plane emoji: one, two and
            // four UTF-8 bytes, and a surrogate pair once widened.
            const auto request = ControlPipe::ParseRequest("{\"op\":\"send-input\",\"pane\":\"1.0.0\",\"text\":\"caf\xc3\xa9 \xe6\x97\xa5 \xf0\x9f\x9a\x80\"}");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"café 日 \U0001F680" }, request->text);
        }
        {
            // The same, spelled with JSON escapes including a surrogate pair.
            const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"1.0.0","text":"café 🚀"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"café \U0001F680" }, request->text);
        }
        {
            // A lone carriage return: the submit half of a two-call send.
            const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"1.0.0","text":"\r"})");
            VERIFY_IS_TRUE(request.has_value());
            VERIFY_ARE_EQUAL(std::wstring{ L"\r" }, request->text);
        }
    }

    void ControlPipeProtocolTests::IgnoresUnknownMembers()
    {
        // Forward compatibility: a client sending a field we don't know yet
        // gets served, it doesn't get a bad-request.
        const auto request = ControlPipe::ParseRequest(R"({"op":"ping","future":{"nested":[1,2]},"another":null})");
        VERIFY_IS_TRUE(request.has_value());
        VERIFY_IS_TRUE(request->op == ControlPipe::Op::Ping);
    }

    void ControlPipeProtocolTests::FormatsResponses()
    {
        // Responses are one line each: a newline in the middle of one would
        // desynchronise the client for the rest of the connection.
        const auto singleLine = [](const std::string& text) {
            return text.find('\n') == std::string::npos && text.find('\r') == std::string::npos;
        };

        VERIFY_ARE_EQUAL(std::string{ R"({"ok":true})" }, ControlPipe::OkResponse());
        VERIFY_ARE_EQUAL(std::string{ R"({"id":"1.0.3","ok":true})" }, ControlPipe::FocusPaneResponse({ 1, 0, 3 }));
        VERIFY_ARE_EQUAL(std::string{ R"({"error":"needle-gone","ok":false})" }, ControlPipe::ErrorResponse(ControlPipe::Error::NeedleGone));
        VERIFY_ARE_EQUAL(std::string{ "no-such-pane" }, std::string{ ControlPipe::ErrorCode(ControlPipe::Error::NoSuchPane) });
        VERIFY_ARE_EQUAL(std::string{ "disconnected" }, std::string{ ControlPipe::ErrorCode(ControlPipe::Error::Disconnected) });
        VERIFY_ARE_EQUAL(std::string{ "bad-request" }, std::string{ ControlPipe::ErrorCode(ControlPipe::Error::BadRequest) });

        const auto ping = ControlPipe::PingResponse(24680, { 1, 2 });
        VERIFY_IS_TRUE(singleLine(ping));
        VERIFY_IS_TRUE(ping.find(R"("pid":24680)") != std::string::npos, NoThrowString().Format(L"%hs", ping.c_str()));
        VERIFY_IS_TRUE(ping.find(R"("windows":[1,2])") != std::string::npos);
        VERIFY_IS_TRUE(ping.find(R"("version":2)") != std::string::npos);

        ControlPipe::PaneEntry pane;
        pane.address = { 1, 0, 3 };
        pane.title = L"stith - bash";
        pane.process = L"wsl.exe";
        pane.focused = true;
        pane.windowFocused = false;
        pane.alive = true;
        pane.pid = 12345;

        const auto list = ControlPipe::ListPanesResponse({ pane });
        VERIFY_IS_TRUE(singleLine(list));
        VERIFY_IS_TRUE(list.find(R"("id":"1.0.3")") != std::string::npos, NoThrowString().Format(L"%hs", list.c_str()));
        VERIFY_IS_TRUE(list.find(R"("focused":true)") != std::string::npos);
        VERIFY_IS_TRUE(list.find(R"("windowFocused":false)") != std::string::npos);
        VERIFY_IS_TRUE(list.find(R"("alive":true)") != std::string::npos);
        VERIFY_IS_TRUE(list.find(R"("process":"wsl.exe")") != std::string::npos);

        // A dead pane is still listed - its last screen is often the thing you
        // wanted - but says so, rather than making a client discover it by
        // attempting a write and reading back `disconnected`.
        pane.alive = false;
        const auto deadList = ControlPipe::ListPanesResponse({ pane });
        VERIFY_IS_TRUE(deadList.find(R"("alive":false)") != std::string::npos, NoThrowString().Format(L"%hs", deadList.c_str()));

        // A capture full of newlines still has to arrive as one line on the
        // wire, with the newlines escaped rather than emitted.
        const auto capture = ControlPipe::CapturePaneResponse(L"line one\nline two\n");
        VERIFY_IS_TRUE(singleLine(capture));
        VERIFY_IS_TRUE(capture.find(R"("text":"line one\nline two\n")") != std::string::npos, NoThrowString().Format(L"%hs", capture.c_str()));
    }

    // What the activity log's command column shows for each op. The point of
    // the record is "what did that program do to my panes", so each one has to
    // say which pane and what was asked, in a form that reads back unambiguously.
    void ControlPipeProtocolTests::DescribesEveryOp()
    {
        const auto summarize = [](std::string_view line) {
            const auto request = ControlPipe::ParseRequest(line);
            VERIFY_IS_TRUE(request.has_value());
            return ControlPipe::SummarizeCall(ControlPipe::DescribeRequest(*request));
        };

        VERIFY_ARE_EQUAL(std::wstring{ L"ping" }, summarize(R"({"op":"ping"})"));
        VERIFY_ARE_EQUAL(std::wstring{ L"list-panes" }, summarize(R"({"op":"list-panes"})"));
        VERIFY_ARE_EQUAL(std::wstring{ LR"(list-panes containing "6f1a")" }, summarize(R"({"op":"list-panes","containing":"6f1a"})"));
        VERIFY_ARE_EQUAL(std::wstring{ L"capture-pane 1.0.3 viewport" }, summarize(R"({"op":"capture-pane","pane":"1.0.3"})"));
        VERIFY_ARE_EQUAL(std::wstring{ L"capture-pane 1.0.3 lines 80" }, summarize(R"({"op":"capture-pane","pane":"1.0.3","lines":80})"));
        VERIFY_ARE_EQUAL(std::wstring{ LR"(send-input 2.1.4 "hello" requireContains "needle")" },
                         summarize(R"({"op":"send-input","pane":"2.1.4","text":"hello","requireContains":"needle"})"));
        VERIFY_ARE_EQUAL(std::wstring{ L"focus-pane 1.0.3" }, summarize(R"({"op":"focus-pane","pane":"1.0.3"})"));
        VERIFY_ARE_EQUAL(std::wstring{ L"focus-pane {6f1a2b3c-0000-4000-8000-00000000000a}" },
                         summarize(R"({"op":"focus-pane","session":"6F1A2B3C-0000-4000-8000-00000000000A"})"));

        // The structured fields the log also writes, for a script that would
        // rather not parse the summary back apart.
        const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"2.1.4","text":"x"})");
        const auto description = ControlPipe::DescribeRequest(*request);
        VERIFY_ARE_EQUAL(std::wstring{ L"send-input" }, description.op);
        VERIFY_ARE_EQUAL(std::wstring{ L"2.1.4" }, description.target);
    }

    // "Did the client send the Enter" is the usual question, and a raw CR in a
    // record would end the line it is printed on rather than answer it.
    void ControlPipeProtocolTests::RecordedTextSpellsOutControlCharacters()
    {
        const auto request = ControlPipe::ParseRequest(R"({"op":"send-input","pane":"1.0.0","text":"a\r\n\t\u001b\"\\z"})");
        VERIFY_IS_TRUE(request.has_value());
        VERIFY_ARE_EQUAL(std::wstring{ LR"("a\r\n\t\x1b\"\\z")" }, ControlPipe::DescribeRequest(*request).detail);

        // Non-ASCII is kept as itself, not escaped.
        const auto unicode = ControlPipe::ParseRequest("{\"op\":\"send-input\",\"pane\":\"1.0.0\",\"text\":\"\xC3\xA9\xF0\x9F\x98\x80\"}");
        VERIFY_IS_TRUE(unicode.has_value());
        VERIFY_ARE_EQUAL(std::wstring{ L"\"é\U0001F600\"" }, ControlPipe::DescribeRequest(*unicode).detail);
    }

    // A pasted megabyte must not become a megabyte of log, and a cut must not
    // split a surrogate pair into something that is no longer text.
    void ControlPipeProtocolTests::RecordedTextIsCapped()
    {
        ControlPipe::Request request;
        request.op = ControlPipe::Op::SendInput;
        request.pane = ControlPipe::PaneAddress{ 1, 0, 0 };
        request.text.assign(ControlPipe::MaxRecordedTextChars + 10, L'x');

        const auto detail = ControlPipe::DescribeRequest(request).detail;
        VERIFY_IS_TRUE(detail.ends_with(L" ...+10 chars"), detail.c_str());
        VERIFY_ARE_EQUAL(ControlPipe::MaxRecordedTextChars + 2 + std::wstring_view{ L" ...+10 chars" }.size(), detail.size());

        // The cap lands between the two halves of an emoji: back off a char.
        request.text.assign(ControlPipe::MaxRecordedTextChars - 1, L'x');
        request.text.append(L"\U0001F600");
        const auto split = ControlPipe::DescribeRequest(request).detail;
        VERIFY_IS_TRUE(split.ends_with(L"x\" ...+2 chars"), split.c_str());
    }

    // A line that never became a request is still recorded, by what was sent:
    // a client writing garbage into the pipe is exactly the one worth seeing.
    void ControlPipeProtocolTests::DescribesUnparsedLines()
    {
        const auto description = ControlPipe::DescribeUnparsed(R"({"op":"rm -rf"})");
        VERIFY_ARE_EQUAL(std::wstring{ L"unparsed" }, description.op);
        VERIFY_IS_TRUE(description.target.empty());
        VERIFY_ARE_EQUAL(std::wstring{ LR"("{\"op\":\"rm -rf\"}")" }, description.detail);

        // Not UTF-8 at all: keep the ASCII, mark the rest.
        const auto binary = ControlPipe::DescribeUnparsed("ab\xFF\xFE");
        VERIFY_ARE_EQUAL(std::wstring{ LR"("ab??")" }, binary.detail);

        // Long lines keep a prefix, cut on a character boundary, and say how
        // much was dropped.
        std::string longLine(ControlPipe::MaxRecordedRawBytes - 1, 'a');
        longLine.append("\xC3\xA9tail");
        const auto cut = ControlPipe::DescribeUnparsed(longLine);
        VERIFY_IS_TRUE(cut.detail.ends_with(L"a\" ...+6 bytes"), cut.detail.c_str());
    }

    void ControlPipeProtocolTests::RequiresReasonAndCaller()
    {
        // Parsed either way: a request without them is still the op it was, so
        // the refusal can be recorded as "list-panes, refused" and not as noise.
        const auto bare = ControlPipe::ParseRequest(R"({"op":"list-panes"})");
        VERIFY_IS_TRUE(bare.has_value());
        VERIFY_IS_FALSE(ControlPipe::HasAttribution(*bare));

        const auto onlyReason = ControlPipe::ParseRequest(R"({"op":"ping","reason":"health check"})");
        VERIFY_IS_TRUE(onlyReason.has_value());
        VERIFY_IS_FALSE(ControlPipe::HasAttribution(*onlyReason));

        const auto blank = ControlPipe::ParseRequest(R"({"op":"ping","reason":"  ","caller":"x"})");
        VERIFY_IS_TRUE(blank.has_value());
        VERIFY_IS_FALSE(ControlPipe::HasAttribution(*blank));

        const auto full = ControlPipe::ParseRequest(R"json({"op":"list-panes","reason":"label the host terminal","caller":"stith-server identifyTerminalProduct (pid 42)"})json");
        VERIFY_IS_TRUE(full.has_value());
        VERIFY_IS_TRUE(ControlPipe::HasAttribution(*full));
        const auto described = ControlPipe::DescribeRequest(*full);
        VERIFY_ARE_EQUAL(std::wstring{ L"label the host terminal" }, described.reason);
        VERIFY_ARE_EQUAL(std::wstring{ L"stith-server identifyTerminalProduct (pid 42)" }, described.caller);

        // The wrong type is a malformed request, not a missing reason.
        VERIFY_IS_FALSE(ControlPipe::ParseRequest(R"({"op":"ping","reason":7,"caller":"x"})").has_value());

        // A paragraph is kept to the cap.
        const std::string longReason(1000, 'r');
        const auto capped = ControlPipe::ParseRequest(R"({"op":"ping","caller":"x","reason":")" + longReason + R"("})");
        VERIFY_IS_TRUE(capped.has_value());
        VERIFY_ARE_EQUAL(ControlPipe::MaxAttributionChars, capped->reason.size());

        const auto refusal = ControlPipe::ErrorResponse(ControlPipe::Error::MissingReason);
        VERIFY_IS_TRUE(refusal.find(R"("error":"missing-reason")") != std::string::npos, NoThrowString().Format(L"%hs", refusal.c_str()));
        VERIFY_IS_TRUE(refusal.find(R"("message":)") != std::string::npos);
    }
}
