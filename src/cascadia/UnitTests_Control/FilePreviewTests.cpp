// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#ifdef FILE_PREVIEW_STANDALONE
#include <windows.h>
#include <cassert>
#include <iostream>
#include <stdexcept>
#define BEGIN_TEST_CLASS(...)
#define TEST_CLASS_PROPERTY(...)
#define END_TEST_CLASS()
#define TEST_METHOD(name) public: void name()
#define VERIFY_IS_TRUE(value) do { if (!(value)) throw std::runtime_error("Line " + std::to_string(__LINE__) + ": " #value); } while (false)
#define VERIFY_ARE_EQUAL(expected, actual) VERIFY_IS_TRUE((expected) == (actual))
#define VERIFY_IS_FALSE(value) VERIFY_IS_TRUE(!(value))
#else
#include "pch.h"
#endif
#include "../TerminalApp/FilePreviewReader.h"
#include <filesystem>
#include "../UIMarkdown/SyntaxHighlight.h"
#include "../UIMarkdown/MarkdownBlocks.h"
#include "../UIMarkdown/AdfMarkdown.h"
#include "../inc/LintelFileTypes.g.h"
#include "../inc/PreviewPresentation.h"
#include "../../inc/LintelPaths.h"
#ifndef FILE_PREVIEW_STANDALONE
#include "../UIMarkdown/Frontmatter.h"
#endif

#ifndef FILE_PREVIEW_STANDALONE
using namespace WEX::TestExecution;
#endif
using namespace Microsoft::Terminal::FilePreview;

namespace ControlUnitTests
{
    struct PackageFixture
    {
        winrt::com_ptr<IOpcFactory> factory;
        winrt::com_ptr<IOpcPackage> package;
        std::wstring path;
        PackageFixture()
        {
            winrt::init_apartment();
            winrt::check_hresult(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IOpcFactory), factory.put_void()));
            winrt::check_hresult(factory->CreatePackage(package.put()));
            wchar_t directory[MAX_PATH]{}, file[MAX_PATH]{};
            if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"wfp", 0, file)) winrt::throw_last_error();
            path = file;
        }
        ~PackageFixture()
        {
            package = nullptr;
            factory = nullptr;
            DeleteFileW(path.c_str());
            winrt::uninit_apartment();
        }
        void Add(const wchar_t* name, const std::string& xml)
        {
            winrt::com_ptr<IOpcPartSet> parts;
            winrt::check_hresult(package->GetPartSet(parts.put()));
            winrt::com_ptr<IOpcPartUri> uri;
            winrt::check_hresult(factory->CreatePartUri(name, uri.put()));
            winrt::com_ptr<IOpcPart> part;
            winrt::check_hresult(parts->CreatePart(uri.get(), L"application/xml", OPC_COMPRESSION_NORMAL, part.put()));
            winrt::com_ptr<IStream> stream;
            winrt::check_hresult(part->GetContentStream(stream.put()));
            winrt::check_hresult(stream->Write(xml.data(), static_cast<ULONG>(xml.size()), nullptr));
        }
        void Save()
        {
            winrt::com_ptr<IStream> stream;
            winrt::check_hresult(factory->CreateStreamOnFile(path.c_str(), OPC_STREAM_IO_WRITE, nullptr, FILE_ATTRIBUTE_NORMAL, stream.put()));
            winrt::check_hresult(factory->WritePackageToStream(package.get(), OPC_WRITE_DEFAULT, stream.get()));
        }
    };

    class FilePreviewTests
    {
        BEGIN_TEST_CLASS(FilePreviewTests)
            TEST_CLASS_PROPERTY(L"ThreadingModel", L"MTA")
        END_TEST_CLASS()
        TEST_METHOD(TextEncodings);
        TEST_METHOD(BoundedUtf8);
        TEST_METHOD(OfficeParagraphs);
        TEST_METHOD(OfficeSheets);
        TEST_METHOD(RejectDtd);
        TEST_METHOD(SourceLocations);
        TEST_METHOD(SyntaxTokens);
        TEST_METHOD(SyntaxTokenWinRTText);
        TEST_METHOD(RichMarkdownBlocks);
        TEST_METHOD(JiraAdfBlocks);
        TEST_METHOD(SharedPathResolution);
        TEST_METHOD(PreviewFieldPresentation);
#ifndef FILE_PREVIEW_STANDALONE
        TEST_METHOD(YamlFrontmatter);
#endif
    };

    void FilePreviewTests::SourceLocations()
    {
        VERIFY_ARE_EQUAL(std::wstring{ L"cs" }, Lintel::ExtensionOf(L"file:///mnt/c/src/Program.cs#L194"));
        VERIFY_ARE_EQUAL(std::wstring{ L"tsx" }, Lintel::ExtensionOf(L"file:///src/Component.TSX?view=raw#L10"));
        VERIFY_IS_TRUE(Lintel::GroupContains(L"sourceCode", L"cs"));
        VERIFY_ARE_EQUAL(std::wstring{ L"TypeScript" }, std::wstring{ Lintel::FindFileType(L"C:\\src\\App.tsx").name });
        VERIFY_ARE_EQUAL(std::wstring{ L"C#" }, std::wstring{ Lintel::FindFileType(L"file:///src/Program.cs#L194").name });
        VERIFY_ARE_EQUAL(std::wstring{ L"Markdown" }, std::wstring{ Lintel::FindFileType(L"README.MD").name });
        VERIFY_ARE_EQUAL(std::wstring{ L"Dockerfile" }, std::wstring{ Lintel::FindFileType(L"/src/Dockerfile").name });
        VERIFY_IS_TRUE(Lintel::Contains(Lintel::FindFileType(L"file:///src/Dockerfile#L8").groups, L"sourceCode"));
        VERIFY_ARE_EQUAL(std::wstring{ L"Makefile" }, std::wstring{ Lintel::FindFileType(L"file:///src/Makefile?raw#L4").name });
        VERIFY_ARE_EQUAL(std::wstring{ L"C#" }, std::wstring{ Lintel::FindFileType(L"/src/hash#name.cs").name });
        VERIFY_IS_TRUE(Lintel::ExtensionOf(L"/home/user.name/README").empty());
    }

    void FilePreviewTests::SyntaxTokens()
    {
        const std::wstring code{ L"public class Program { // note\n string value = \"hello\"; int n = 42; }" };
        const auto tokens = MarkdownPreview::Highlight(code, L"cs");
        std::wstring rebuilt;
        bool keyword = false, comment = false, number = false, literal = false;
        for (const auto& token : tokens)
        {
            rebuilt.append(code, token.start, token.length);
            keyword |= token.kind == MarkdownPreview::TokenKind::Keyword;
            comment |= token.kind == MarkdownPreview::TokenKind::Comment;
            number |= token.kind == MarkdownPreview::TokenKind::Number;
            literal |= token.kind == MarkdownPreview::TokenKind::String;
        }
        VERIFY_ARE_EQUAL(code, rebuilt);
        VERIFY_IS_TRUE(keyword && comment && number && literal);
        const auto unknown = MarkdownPreview::Highlight(code, L"unknown-language");
        VERIFY_ARE_EQUAL(size_t{ 1 }, unknown.size());
        VERIFY_IS_TRUE(unknown[0].kind == MarkdownPreview::TokenKind::Plain);
        const auto yaml = MarkdownPreview::Highlight(L"title: 'hello'\n", L"yml");
        VERIFY_IS_TRUE(yaml[0].kind == MarkdownPreview::TokenKind::Key);
    }

    void FilePreviewTests::SyntaxTokenWinRTText()
    {
        const std::wstring source{ L"public class Program { string text = \"hello\"; int count = 42; }" };
        const auto tokens = MarkdownPreview::Highlight(source, L"cs");
        VERIFY_IS_TRUE(tokens.size() > 1);
        VERIFY_IS_TRUE(source[tokens.front().start + tokens.front().length] != L'\0');
        std::wstring rebuilt;
        // Exercise the same WinRT parameter conversion as Run.Text, without a
        // XAML host. An unterminated view aborts here before a setter can run.
        const auto setText = [&](const winrt::param::hstring& value) {
            const winrt::hstring& text = value;
            VERIFY_IS_TRUE(text.c_str()[text.size()] == L'\0');
            rebuilt.append(text.c_str(), text.size());
        };
        for (const auto& token : tokens)
        {
            setText(MarkdownPreview::TokenText(source, token));
        }
        VERIFY_ARE_EQUAL(source, rebuilt);
        setText(MarkdownPreview::TokenText(source, { 0, 0, MarkdownPreview::TokenKind::Plain }));
        VERIFY_ARE_EQUAL(source, rebuilt);
    }

    void FilePreviewTests::RichMarkdownBlocks()
    {
        const auto blocks = MarkdownPreview::ParseRichBlocks("Before\n\n| Name | Value |\n| --- | --- |\n| A\\|B | `x|y` |\n\n> [!WARNING]\n> **Careful**\n>\n> Next paragraph\n\n```md\n| Raw | Table |\n| --- | --- |\n```\n");
        const auto table = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) { return block.kind == MarkdownPreview::RichBlock::Kind::Table; });
        VERIFY_IS_TRUE(table != blocks.end());
        VERIFY_ARE_EQUAL(size_t{ 2 }, table->rows.size());
        VERIFY_ARE_EQUAL(std::string{ "A|B" }, table->rows[1][0]);
        VERIFY_ARE_EQUAL(std::string{ "`x|y`" }, table->rows[1][1]);
        const auto alert = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) { return block.kind == MarkdownPreview::RichBlock::Kind::Callout; });
        VERIFY_IS_TRUE(alert != blocks.end());
        VERIFY_ARE_EQUAL(std::string{ "WARNING" }, alert->tone);
        VERIFY_IS_TRUE(alert->text.find("**Careful**\n\nNext paragraph") != std::string::npos);
        VERIFY_IS_TRUE(blocks.back().text.find("| Raw | Table |") != std::string::npos);
        VERIFY_IS_FALSE(MarkdownPreview::TableDivider("| name | value |"));
        VERIFY_IS_TRUE(MarkdownPreview::TableDivider("| :--- | ---: |"));
    }

    void FilePreviewTests::JiraAdfBlocks()
    {
        winrt::init_apartment();
        const auto apartment = wil::scope_exit([] { winrt::uninit_apartment(); });
        const auto node = winrt::Windows::Data::Json::JsonObject::Parse(LR"({"type":"doc","content":[{"type":"panel","attrs":{"panelType":"error"},"content":[{"type":"paragraph","content":[{"type":"text","text":"Stop","marks":[{"type":"strong"}]}]}]},{"type":"table","content":[{"type":"tableRow","content":[{"type":"tableHeader","content":[{"type":"paragraph","content":[{"type":"text","text":"Name"}]}]},{"type":"tableHeader","content":[{"type":"paragraph","content":[{"type":"text","text":"Result"}]}]}]},{"type":"tableRow","content":[{"type":"tableCell","content":[{"type":"paragraph","content":[{"type":"text","text":"A|B"}]}]},{"type":"tableCell","content":[{"type":"paragraph","content":[{"type":"text","text":"422"}]}]}]}]},{"type":"codeBlock","attrs":{"language":"typescript"},"content":[{"type":"text","text":"const n = 1;"}]}]})");
        std::wstring markdown;
        MarkdownPreview::FlattenAdf(node, markdown, 0);
        const auto blocks = MarkdownPreview::ParseRichBlocks(winrt::to_string(markdown));
        const auto alert = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) { return block.kind == MarkdownPreview::RichBlock::Kind::Callout; });
        VERIFY_IS_TRUE(alert != blocks.end());
        VERIFY_ARE_EQUAL(std::string{ "ERROR" }, alert->tone);
        VERIFY_IS_TRUE(alert->text.find("**Stop**") != std::string::npos);
        const auto table = std::find_if(blocks.begin(), blocks.end(), [](const auto& block) { return block.kind == MarkdownPreview::RichBlock::Kind::Table; });
        VERIFY_IS_TRUE(table != blocks.end());
        VERIFY_ARE_EQUAL(std::string{ "A|B" }, table->rows[1][0]);
        VERIFY_ARE_EQUAL(std::string{ "422" }, table->rows[1][1]);
        VERIFY_IS_TRUE(markdown.find(L"```typescript\nconst n = 1;\n```") != std::wstring::npos);
    }

#ifndef FILE_PREVIEW_STANDALONE
    void FilePreviewTests::YamlFrontmatter()
    {
        const auto parsed = MarkdownPreview::ParseFrontmatter("---\r\ntitle: Example\r\ntags: [one, two]\r\nauthor:\r\n  name: Ada\r\nsummary: |\r\n  First line\r\n  Second line\r\n---\r\n# Body\r\n");
        VERIFY_IS_TRUE(parsed.present && parsed.error.empty());
        VERIFY_ARE_EQUAL(std::string{ "# Body\r\n" }, std::string{ parsed.body });
        VERIFY_IS_TRUE(std::any_of(parsed.rows.begin(), parsed.rows.end(), [](const auto& row) { return row.key == "title" && row.value == "Example"; }));
        VERIFY_IS_TRUE(std::any_of(parsed.rows.begin(), parsed.rows.end(), [](const auto& row) { return row.key == "name" && row.value == "Ada"; }));
        VERIFY_IS_TRUE(std::any_of(parsed.rows.begin(), parsed.rows.end(), [](const auto& row) { return row.value == "First line\nSecond line\n"; }));
        VERIFY_IS_FALSE(MarkdownPreview::ParseFrontmatter("# Heading\n---\ntext").present);
        VERIFY_IS_FALSE(MarkdownPreview::ParseFrontmatter("---\ntitle: unclosed\n").present);
        VERIFY_IS_TRUE(!MarkdownPreview::ParseFrontmatter("---\nbad: [\n---\n# Body").error.empty());
        const auto cycle = MarkdownPreview::ParseFrontmatter("---\na: &a\n  self: *a\n---\nBody");
        VERIFY_IS_TRUE(cycle.rows.size() < 20);
        VERIFY_IS_TRUE(MarkdownPreview::ParseFrontmatter("\xef\xbb\xbf---\ntitle: BOM\n...\nBody").present);
    }
#endif

    void FilePreviewTests::TextEncodings()
    {
        VERIFY_ARE_EQUAL(std::wstring{ L"hello\nworld" }, DecodeText("hello\nworld", false).value());
        VERIFY_ARE_EQUAL(std::wstring{ L"\u00e9" }, DecodeText("\xef\xbb\xbf\xc3\xa9", false).value());
        VERIFY_ARE_EQUAL(std::wstring{ L"A" }, DecodeText(std::string{ "\xff\xfe\x41\0", 4 }, false).value());
        VERIFY_ARE_EQUAL(std::wstring{ L"A" }, DecodeText(std::string{ "\xfe\xff\0\x41", 4 }, false).value());
        VERIFY_IS_FALSE(DecodeText(std::string{ "abc\0def", 7 }, false).has_value());
        VERIFY_IS_FALSE(DecodeText("\xff invalid UTF-8", false).has_value());
        VERIFY_IS_FALSE(DecodeText(std::string{ "\xff\xfe\0\xd8", 4 }, false).has_value());
        VERIFY_IS_TRUE(DecodeText("", false).has_value());
    }

    void FilePreviewTests::BoundedUtf8()
    {
        VERIFY_IS_TRUE(DecodeText("\xc3\xa9", true)->starts_with(L"\u00e9\n"));
        VERIFY_IS_TRUE(DecodeText("a\xe2\x82", true)->starts_with(L"a\n[Preview truncated"));
        VERIFY_IS_FALSE(DecodeText("a\xe2\x82", false).has_value());
    }

    void FilePreviewTests::OfficeParagraphs()
    {
        PackageFixture file;
        file.Add(L"/word/document.xml", R"(<w:document xmlns:w="urn:word"><w:body><w:p><w:r><w:t>Hello</w:t></w:r></w:p><w:p><w:r><w:t>World</w:t></w:r></w:p></w:body></w:document>)");
        file.Save();
        const auto result = ReadOfficeSections(file.path, L".docx");
        VERIFY_ARE_EQUAL(size_t{ 1 }, result.size());
        VERIFY_ARE_EQUAL(std::wstring{ L"Hello\nWorld\n" }, result[0].second);
    }

    void FilePreviewTests::OfficeSheets()
    {
        PackageFixture file;
        file.Add(L"/xl/workbook.xml", R"(<workbook xmlns:r="urn:rel"><sheets><sheet name="Totals" r:id="s1"/></sheets></workbook>)");
        {
            winrt::com_ptr<IOpcPartSet> parts;
            winrt::check_hresult(file.package->GetPartSet(parts.put()));
            winrt::com_ptr<IOpcPartUri> uri;
            winrt::check_hresult(file.factory->CreatePartUri(L"/xl/workbook.xml", uri.put()));
            winrt::com_ptr<IOpcPart> part;
            winrt::check_hresult(parts->GetPart(uri.get(), part.put()));
            winrt::com_ptr<IOpcRelationshipSet> rels;
            winrt::check_hresult(part->GetRelationshipSet(rels.put()));
            winrt::com_ptr<IUri> target;
            winrt::check_hresult(CreateUri(L"worksheets/sheet1.xml", Uri_CREATE_ALLOW_RELATIVE, 0, target.put()));
            winrt::com_ptr<IOpcRelationship> relationship;
            winrt::check_hresult(rels->CreateRelationship(L"s1", L"urn:sheet", target.get(), OPC_URI_TARGET_MODE_INTERNAL, relationship.put()));
        }
        file.Add(L"/xl/sharedStrings.xml", R"(<sst><si><t>Revenue</t></si></sst>)");
        file.Add(L"/xl/worksheets/sheet1.xml", R"(<worksheet><sheetData><row r="1"><c r="A1" t="s"><v>0</v></c><c r="C1"><f>1+2</f><v>3</v></c></row><row r="201"><c r="A201"><v>99</v></c></row></sheetData></worksheet>)");
        file.Save();
        const auto result = ReadOfficeSections(file.path, L".xlsx");
        VERIFY_ARE_EQUAL(std::wstring{ L"Totals" }, result[0].first);
        VERIFY_IS_TRUE(result[0].second.starts_with(L"Revenue\t\t3\t\n"));
        VERIFY_IS_TRUE(result[0].second.find(L"Preview limited") != std::wstring::npos);
        VERIFY_IS_TRUE(result[0].second.find(L"99") == std::wstring::npos);
    }

    void FilePreviewTests::RejectDtd()
    {
        PackageFixture file;
        file.Add(L"/word/document.xml", R"(<!DOCTYPE document [<!ENTITY local SYSTEM "file:///C:/Windows/win.ini">]><document><t>&local;</t></document>)");
        file.Save();
        bool rejected = false;
        try { ReadOfficeSections(file.path, L".docx"); }
        catch (const winrt::hresult_error&) { rejected = true; }
        VERIFY_IS_TRUE(rejected);
    }
}

void ControlUnitTests::FilePreviewTests::PreviewFieldPresentation()
{
    using namespace Microsoft::Terminal::PreviewPresentation;
    VERIFY_IS_TRUE(IsCommitGroup(L"github", L"commit"));
    VERIFY_IS_FALSE(IsCommitGroup(L"jira", L"commit"));
    VERIFY_IS_FALSE(IsCommitGroup(L"jira", L"details"));
    VERIFY_IS_FALSE(IsCommitGroup(L"github", L"pullRequest"));
    VERIFY_IS_FALSE(IsCommitGroup(L"github", L""));
    VERIFY_IS_TRUE(IsZeroCount(L"0"));
    VERIFY_IS_FALSE(IsZeroCount(L"10"));
    VERIFY_IS_FALSE(IsZeroCount(L"0%"));
    VERIFY_IS_FALSE(IsZeroCount(L""));
    VERIFY_IS_FALSE(IsZeroCount(L"AT-0"));
}

void ControlUnitTests::FilePreviewTests::SharedPathResolution()
{
    const auto windows = Lintel::PathCandidates(LR"(Z:\home\stevenp\x.png)", true, L"Ubuntu", { L"Debian" });
    VERIFY_ARE_EQUAL(std::wstring{ LR"(Z:\home\stevenp\x.png)" }, windows.front().path);
    const auto source = Lintel::PathCandidates(L"/tmp/shefrd-e05e0c5b-handoff.md", true, L"Ubuntu", { L"Debian" });
    VERIFY_ARE_EQUAL(std::wstring{ LR"(\\wsl.localhost\Ubuntu\tmp\shefrd-e05e0c5b-handoff.md)" }, source.front().path);
    const auto unknown = Lintel::PathCandidates(L"/tmp/a.md", true, {}, { L"Ubuntu", L"Debian", L"Ubuntu" });
    VERIFY_ARE_EQUAL(size_t{ 2 }, unknown.size());
    VERIFY_IS_FALSE(Lintel::SelectPathCandidate(unknown, { true, true }).has_value());
    VERIFY_IS_FALSE(Lintel::SelectPathCandidate(unknown, { false, false }).has_value());
    VERIFY_ARE_EQUAL(std::wstring{ L"Debian" }, Lintel::SelectPathCandidate(unknown, { false, true })->distro);
    const auto mount = Lintel::PathCandidates(L"/mnt/c/Users/steve/a.md", true, L"Ubuntu");
    VERIFY_ARE_EQUAL(std::wstring{ LR"(C:\Users\steve\a.md)" }, mount.front().path);
    const auto literal = Lintel::PathCandidates(L"/tmp/a%23b#c.md", false, {});
    VERIFY_ARE_EQUAL(std::wstring{ L"/tmp/a%23b#c.md" }, literal.front().path);

    // "~/..." is the home of whoever printed it, never the distribution's root.
    VERIFY_IS_TRUE(Lintel::ClassifyPath(L"~/.claude/x.md") == Lintel::PathKind::Home);
    const std::vector<Lintel::PathHome> homes{ { L"", LR"(C:\Users\steve)" }, { L"Ubuntu", L"/home/stevenp/" } };
    const auto wslHome = Lintel::PathCandidates(L"~/.claude/plans/notes-tui-handoff-prompt.md", true, L"Ubuntu", { L"Debian" }, homes);
    VERIFY_ARE_EQUAL(size_t{ 1 }, wslHome.size());
    VERIFY_ARE_EQUAL(std::wstring{ LR"(\\wsl.localhost\Ubuntu\home\stevenp\.claude\plans\notes-tui-handoff-prompt.md)" }, wslHome.front().path);
    VERIFY_ARE_EQUAL(std::wstring{ L"Ubuntu" }, wslHome.front().distro);
    // Home not known yet: nothing, rather than a guess or another distribution's home.
    VERIFY_IS_TRUE(Lintel::PathCandidates(L"~/.bashrc", true, L"Debian", { L"Ubuntu" }, homes).empty());
    // A non-WSL shell's "~" is the Windows profile.
    const auto windowsHome = Lintel::PathCandidates(L"~/.claude/settings.json", true, {}, { L"Ubuntu" }, homes);
    VERIFY_ARE_EQUAL(std::wstring{ LR"(C:\Users\steve\.claude\settings.json)" }, windowsHome.front().path);
    const auto rootHome = Lintel::PathCandidates(L"~/x", true, L"Debian", {}, { { L"Debian", L"/" } });
    VERIFY_ARE_EQUAL(std::wstring{ LR"(\\wsl.localhost\Debian\x)" }, rootHome.front().path);
}

#ifdef FILE_PREVIEW_STANDALONE
int main()
{
    try
    {
        ControlUnitTests::FilePreviewTests tests;
        tests.TextEncodings();
        tests.BoundedUtf8();
        tests.OfficeParagraphs();
        tests.OfficeSheets();
        tests.RejectDtd();
        tests.SourceLocations();
        tests.SyntaxTokens();
        std::cerr << "SyntaxTokenWinRTText\n";
        tests.SyntaxTokenWinRTText();
        std::cerr << "RichMarkdownBlocks\n";
        tests.RichMarkdownBlocks();
        std::cerr << "JiraAdfBlocks\n";
        tests.JiraAdfBlocks();
        tests.SharedPathResolution();
        tests.PreviewFieldPresentation();
        std::cout << "All 12 native file preview tests passed.\n";
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        std::cerr << "WinRT error " << std::hex << error.code().value << ": " << winrt::to_string(error.message()) << '\n';
        return 1;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
