// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "HyperlinkPreviewService.h"
#include "LinkPreviewPaneContent.h"
#include "../TerminalControl/HyperlinkRules.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include "FilePreviewReader.h"
#include "RemoteFileRead.h"
#include "../../inc/ProcessCaptureImpl.h"
#include "../../inc/LintelPaths.h"
#include "../../inc/LintelHomes.h"
#include "../../types/inc/utils.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <iomanip>
#include "../inc/LintelFileTypes.g.h"

namespace winrt::TerminalApp::implementation
{
    namespace Control = winrt::Microsoft::Terminal::Control;
    using namespace ::Microsoft::Terminal::FilePreview;

    safe_void_coroutine HyperlinkPreviewService::InvokeLinkAction(hstring text, Control::IControlSettings settings, hstring actionId)
    {
        const auto lifetime = get_strong();
        try
        {
            const winrt::apartment_context ui;
            if (!settings || actionId.empty() || actionId == L"none") co_return;
            const auto isFile = Lintel::ClassifyPath(std::wstring_view{ text }) != Lintel::PathKind::None || std::wstring_view{ text }.starts_with(L"file://");
            const auto effective = Control::implementation::ResolveHyperlinkRules(settings, std::wstring_view{ text }, isFile);
            if (actionId == L"primary") actionId = effective.primaryAction;
            if (actionId == L"alternative") actionId = effective.alternativeAction;
            if (actionId.empty() || actionId == L"none") co_return;
            if (actionId == L"open")
            {
                const auto bound = ResolveOpenAction(text, effective.integration);
                if (!bound.empty() && bound != L"open") actionId = bound;
            }
            hstring target = ResolveLink(text, effective.integration);
            if (target.empty() && !isFile && (actionId == L"open" || actionId == L"copyLink"))
            {
                if (const auto preview = co_await GetPreviewAsync(text, effective.integration)) target = preview.ResolvedUri();
                co_await ui;
            }
            if (target.empty()) target = text;
            hstring path;
            if (isFile)
            {
                const auto distro = ::Microsoft::Console::Utils::WslDistroForCommandline(settings.Commandline(), settings.PathTranslationStyle() == Control::PathTranslationStyle::WSL);
                const auto plain = std::wstring_view{ text }.starts_with(L"file://") ? ::Microsoft::Console::Utils::ResolveFileUriTarget(std::wstring_view{ text }, distro) : std::wstring{ text };
                co_await resume_background();
                // Off the UI thread, so a "~/..." path may read the distribution's home.
                const auto candidates = Lintel::PathCandidates(plain, true, distro, ::Microsoft::Console::Utils::RegisteredWslDistros(), Mullion::PathHomesFor(distro, true));
                std::vector<bool> exists;
                for (const auto& candidate : candidates) { std::error_code error; exists.push_back(std::filesystem::exists(candidate.path, error)); }
                const auto selected = Lintel::SelectPathCandidate(candidates, exists);
                co_await ui;
                if (!selected) co_return;
                path = selected->path;
                target = ::Microsoft::Console::Utils::FilePathToUri(selected->path);
            }
            if (actionId == L"copyLink" || actionId == L"copyPath")
            {
                // Copy link copies a path the way it is written, never as the file:// URI
                // Open is handed -- the same rule as TermControl::_hyperlinkCopyLinkText.
                const auto writtenAsPath = Lintel::ClassifyPath(std::wstring_view{ text }) != Lintel::PathKind::None;
                const auto linkText = !isFile ? target : (writtenAsPath ? text : path);
                Windows::ApplicationModel::DataTransfer::DataPackage data;
                data.SetText(actionId == L"copyPath" ? path : linkText);
                Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
            }
            else if (actionId == L"reveal" && !path.empty())
            {
                const auto args = std::wstring{ L"/select,\"" } + std::wstring{ path } + L"\"";
                ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            else if (LinkAction) LinkAction(actionId == L"showInPane" ? text : target, actionId, effective.integration, path);
        } CATCH_LOG();
    }

    Windows::UI::Xaml::FrameworkElement HyperlinkPreviewService::CreatePreviewView(const hstring& text, const Control::IControlSettings& settings, int32_t depth)
    {
        namespace C = Windows::UI::Xaml::Controls;
        const auto isFile = Lintel::ClassifyPath(std::wstring_view{ text }) != Lintel::PathKind::None || std::wstring_view{ text }.starts_with(L"file://");
        const auto effective = Control::implementation::ResolveHyperlinkRules(settings, std::wstring_view{ text }, isFile);
        C::StackPanel host;
        host.Spacing(6);
        host.Padding(Windows::UI::Xaml::Thickness{ 10 });
        C::HyperlinkButton source;
        source.Content(box_value(text));
        const Control::IHyperlinkPreviewProvider provider = *this;
        source.Click([provider, text, settings](auto&&, auto&&) { provider.InvokeLinkAction(text, settings, L"open"); });
        host.Children().Append(source);
        C::StackPanel buttons;
        buttons.Orientation(C::Orientation::Horizontal); buttons.Spacing(6);
        const auto addButton = [&](std::wstring_view label, const hstring& id) {
            C::Button button; button.Content(box_value(hstring{ label }));
            button.Click([provider, text, settings, id](auto&&, auto&&) { provider.InvokeLinkAction(text, settings, id); });
            buttons.Children().Append(button);
        };
        // The same split the link tooltip makes (TermControl::_applyHyperlinkBuiltInButtons):
        // a button the rule switched off still works, so it goes in a menu at the end of
        // the row, and Copy link and Copy path are one action when they would copy the
        // same text -- a path written the Windows way, or a file:// link.
        C::MenuFlyout more;
        const auto offer = [&](bool available, bool shown, std::wstring_view label, const hstring& id) {
            if (!available) return;
            if (shown)
            {
                addButton(label, id);
                return;
            }
            if (!effective.actionsEnabled) return;
            C::MenuFlyoutItem item;
            item.Text(hstring{ label });
            item.Click([provider, text, settings, id](auto&&, auto&&) { provider.InvokeLinkAction(text, settings, id); });
            more.Items().Append(item);
        };
        const auto kind = Lintel::ClassifyPath(std::wstring_view{ text });
        const auto copiesAgree = isFile && (kind == Lintel::PathKind::Windows || kind == Lintel::PathKind::Unc || std::wstring_view{ text }.starts_with(L"file://"));
        const auto keepCopyLink = !copiesAgree || (effective.showCopyLink && !effective.showCopyPath);
        offer(true, effective.showOpen, L"Open", L"open");
        offer(keepCopyLink, effective.showCopyLink, L"Copy link", L"copyLink");
        offer(isFile && !(copiesAgree && keepCopyLink), effective.showCopyPath, L"Copy path", L"copyPath");
        offer(isFile, effective.showReveal, L"Show in Explorer", L"reveal");
        offer(true, effective.showInPane, L"Show in pane", L"showInPane");
        for (const auto& action : effective.customActions) addButton(std::wstring_view{ action.Name() }, action.ActionId());
        if (more.Items().Size() > 0)
        {
            C::FontIcon glyph;
            glyph.Glyph(L"\uE712");
            glyph.FontSize(14);
            C::Button button;
            button.Content(glyph);
            button.VerticalAlignment(Windows::UI::Xaml::VerticalAlignment::Stretch);
            button.Flyout(more);
            C::ToolTipService::SetToolTip(button, box_value(L"More actions"));
            Windows::UI::Xaml::Automation::AutomationProperties::SetName(button, L"More actions");
            buttons.Children().Append(button);
        }
        if (effective.showPreview && effective.integration != L"none" && effective.integrationDisplayMode != Control::HyperlinkIntegrationDisplayMode::None)
        {
            const auto pane = winrt::make_self<LinkPreviewPaneContent>();
            pane->SetPreviewProvider(*this);
            pane->SetLinkSettings(settings, true, depth);
            std::wstring path;
            if (isFile)
            {
                const auto distro = ::Microsoft::Console::Utils::WslDistroForCommandline(settings.Commandline(), settings.PathTranslationStyle() == Control::PathTranslationStyle::WSL);
                if (std::wstring_view{ text }.starts_with(L"file://")) path = ::Microsoft::Console::Utils::ResolveFileUriTarget(std::wstring_view{ text }, distro);
                else
                {
                    const auto candidates = Lintel::PathCandidates(std::wstring_view{ text }, true, distro, {}, Mullion::PathHomesFor(distro, false));
                    path = candidates.empty() ? std::wstring{ text } : candidates.front().path;
                }
            }
            pane->ShowLink(text, effective.integration, winrt::hstring{ path });
            pane->GetRoot().MaxHeight(settings.HyperlinkTooltipMaxHeight() > 0 ? std::max(100.0, static_cast<double>(settings.HyperlinkTooltipMaxHeight()) - 100) : std::numeric_limits<double>::infinity());
            host.Children().Append(pane->GetRoot());
        }
        host.Children().Append(buttons);
        if (effective.showRule)
        {
            C::TextBlock label;
            label.Text(effective.ruleIndex >= 0 ? hstring{ L"Matched by " + std::wstring{ effective.ruleName } } : hstring{ L"No rule matched" });
            label.Opacity(0.7);
            host.Children().Append(label);
        }
        C::ScrollViewer viewport;
        viewport.VerticalScrollBarVisibility(C::ScrollBarVisibility::Auto);
        viewport.HorizontalScrollBarVisibility(C::ScrollBarVisibility::Disabled);
        viewport.HorizontalScrollMode(C::ScrollMode::Disabled);
        viewport.Content(host);
        return viewport;
    }

    // A folder is as reachable as a file, and what someone hovering one wants to know is
    // what is in it: folders first, then files, each alphabetically, the way Explorer
    // lists them. Capped, because this runs on hover and a hover must not walk
    // node_modules; a folder past the cap says so instead of pretending to be complete.
    template<typename Cancelled>
    static void FillFolderPreview(const Control::HyperlinkPreview& preview, const std::filesystem::path& path, const Cancelled& cancelled)
    {
        static constexpr size_t MaxEntries = 500;
        static constexpr auto FolderGlyph = L"\uE8B7";

        std::vector<std::wstring> folders;
        std::vector<std::wstring> files;
        auto truncated = false;
        std::error_code error;
        for (std::filesystem::directory_iterator it{ path, std::filesystem::directory_options::skip_permission_denied, error }, end; !error && it != end; it.increment(error))
        {
            if (cancelled()) throw hresult_canceled();
            if (folders.size() + files.size() >= MaxEntries)
            {
                truncated = true;
                break;
            }
            std::error_code typeError;
            auto name = it->path().filename().wstring();
            if (it->is_directory(typeError))
            {
                folders.push_back(std::move(name) + L"\\");
            }
            else
            {
                files.push_back(std::move(name));
            }
        }
        if (error && folders.empty() && files.empty()) throw hresult_error(E_ACCESSDENIED, L"This folder could not be read.");

        const auto byName = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(files.begin(), files.end(), byName);

        // "C:\" has no file name of its own, and neither has a path with a trailing slash.
        auto name = path.filename().wstring();
        if (name.empty()) name = path.parent_path().filename().wstring();
        if (name.empty()) name = path.wstring();

        const auto count = [](size_t n, std::wstring_view one, std::wstring_view many) {
            return std::to_wstring(n) + L" " + std::wstring{ n == 1 ? one : many };
        };
        std::wstring summary;
        if (truncated) summary = L"More than " + std::to_wstring(MaxEntries) + L" items";
        else if (folders.empty() && files.empty()) summary = L"Empty";
        else if (folders.empty()) summary = count(files.size(), L"file", L"files");
        else if (files.empty()) summary = count(folders.size(), L"folder", L"folders");
        else summary = count(folders.size(), L"folder", L"folders") + L", " + count(files.size(), L"file", L"files");

        preview.IntegrationName(L"Folder");
        preview.IntegrationIcon(FolderGlyph);
        auto fields = single_threaded_vector<Control::HyperlinkPreviewField>();
        Control::HyperlinkPreviewField title;
        title.Kind(Control::HyperlinkPreviewFieldKind::Title);
        title.Value(name);
        fields.Append(title);
        Control::HyperlinkPreviewField metadata;
        metadata.Value(L"Folder \u00B7 " + summary);
        metadata.IconUri(FolderGlyph);
        fields.Append(metadata);
        preview.Fields(fields);

        if (folders.empty() && files.empty()) return;
        std::wstring listing;
        for (const auto* group : { &folders, &files })
        {
            for (const auto& entry : *group)
            {
                if (!listing.empty()) listing += L'\n';
                listing += entry;
            }
        }
        Control::HyperlinkPreviewTab section;
        section.Label(L"Contents");
        section.Body(listing);
        preview.FileSections(single_threaded_vector<Control::HyperlinkPreviewTab>({ section }));
        preview.FileKind(L"folder");
    }

    Windows::Foundation::IAsyncOperation<Control::HyperlinkPreview> HyperlinkPreviewService::GetFilePreviewAsync(hstring resolvedFilePath)
    {
        const auto cancellation = co_await get_cancellation_token();
        co_await resume_background();
        if (cancellation()) throw hresult_canceled();
        check_hresult(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const auto apartment = wil::scope_exit([] { CoUninitialize(); });
        Control::HyperlinkPreview preview;
        preview.IntegrationName(L"File");
        preview.IntegrationIcon(L"\uE8A5");
        preview.FilePath(resolvedFilePath);
        preview.SourceText(resolvedFilePath);
        try
        {
            // A preview runs on hover, so it never opens an SMB connection to another machine.
            if (::Microsoft::Console::Utils::IsRemoteUncPath(std::wstring_view{ resolvedFilePath }))
            {
                preview.FilePath(L"");
                throw hresult_error(E_ACCESSDENIED, L"Network paths are not previewed.");
            }
            if (Lintel::ClassifyPath(std::wstring_view{ resolvedFilePath }) == Lintel::PathKind::Posix)
            {
                const auto candidates = Lintel::PathCandidates(std::wstring_view{ resolvedFilePath }, true, {}, ::Microsoft::Console::Utils::RegisteredWslDistros());
                std::vector<bool> exists;
                for (const auto& candidate : candidates)
                {
                    if (cancellation()) throw hresult_canceled();
                    std::error_code error;
                    exists.push_back(std::filesystem::exists(candidate.path, error));
                }
                const auto selected = Lintel::SelectPathCandidate(candidates, exists);
                if (!selected)
                {
                    preview.FilePath(L"");
                    const auto count = std::count(exists.begin(), exists.end(), true);
                    throw hresult_error(E_FAIL, count > 1 ? L"Path exists in multiple WSL distributions. Use a path naming the distribution." : L"Path was not found in any registered WSL distribution.");
                }
                resolvedFilePath = selected->path;
                preview.FilePath(resolvedFilePath);
            }
            const std::filesystem::path path{ resolvedFilePath.c_str() };
            if (std::error_code kindError; std::filesystem::is_directory(path, kindError))
            {
                FillFolderPreview(preview, path, [&] { return cancellation(); });
                if (cancellation()) throw hresult_canceled();
                co_return preview;
            }
            if (!std::filesystem::is_regular_file(path)) throw hresult_error(E_FAIL, L"This path is not a file or a folder that can be read.");
            const auto size = std::filesystem::file_size(path);
            auto fields = single_threaded_vector<Control::HyperlinkPreviewField>();
            Control::HyperlinkPreviewField title;
            title.Kind(Control::HyperlinkPreviewFieldKind::Title);
            title.Value(path.filename().wstring());
            fields.Append(title);
            Control::HyperlinkPreviewField metadata;
            const auto& fileType = Lintel::FindFileType(path.wstring());
            preview.IntegrationName(fileType.name);
            preview.IntegrationIcon(fileType.icon);
            std::wostringstream sizeLabel;
            if (size < 1024) sizeLabel << size << (size == 1 ? L" byte" : L" bytes");
            else if (size < 1024 * 1024) sizeLabel << std::fixed << std::setprecision(1) << static_cast<double>(size) / 1024 << L" KiB";
            else sizeLabel << std::fixed << std::setprecision(1) << static_cast<double>(size) / (1024 * 1024) << L" MiB";
            metadata.Value(std::wstring{ fileType.name } + L" · " + sizeLabel.str());
            metadata.IconUri(fileType.icon);
            fields.Append(metadata);
            preview.Fields(fields);
            std::wstring ext = path.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (size > 50 * 1024 * 1024) throw hresult_error(E_FAIL, L"File exceeds the 50 MiB preview limit.");
            if (ext == L".pdf") preview.FileKind(L"pdf");
            else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" || ext == L".bmp" || ext == L".tif" || ext == L".tiff") preview.FileKind(L"image");
            else if (ext == L".docx" || ext == L".xlsx" || ext == L".pptx")
            {
                preview.FileKind(ext == L".xlsx" ? L"sheet" : L"document");
                auto sections = single_threaded_vector<Control::HyperlinkPreviewTab>();
                for (const auto& [label, body] : ReadOfficeSections(path.wstring(), ext))
                {
                    Control::HyperlinkPreviewTab section;
                    section.Label(label);
                    section.Body(body);
                    sections.Append(section);
                }
                preview.FileSections(sections);
            }
            else
            {
                std::ifstream file{ path, std::ios::binary };
                if (!file) throw hresult_error(E_ACCESSDENIED, L"File could not be opened for preview.");
                std::string bytes(std::min(static_cast<size_t>(size), TextLimit), '\0');
                file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (file.bad()) throw hresult_error(E_FAIL, L"File could not be read completely.");
                bytes.resize(static_cast<size_t>(file.gcount()));
                auto decoded = DecodeText(bytes, size > TextLimit);
                if (!decoded) throw hresult_error(E_FAIL, L"No built-in preview for this binary format or text encoding.");
                const auto& text = *decoded;
                Control::HyperlinkPreviewTab section;
                section.Label(L"Text");
                section.Body(text);
                preview.FileSections(single_threaded_vector<Control::HyperlinkPreviewTab>({ section }));
                preview.FileKind(fileType.language == L"markdown" ? L"markdown" : L"text");
            }
        }
        catch (const hresult_error& error) { preview.Error(error.message()); }
        catch (...) { preview.Error(L"File could not be read. It may be missing, inaccessible, or damaged."); }
        if (cancellation()) throw hresult_canceled();
        co_return preview;
    }

    // Long enough for a cold wsl.exe and a round trip to a LAN machine; a hover that waits longer is not a hover.
    static constexpr unsigned long RemoteReadTimeoutMs = 5000;

    // The file lives on another machine, so nothing here touches the filesystem: shefrd in
    // WSL reads it over the SSH connection it already holds. Text files only, under
    // the same 1 MiB limit as a local file. FilePath stays empty, so Markdown has no base to
    // resolve a relative image against and nothing reaches for a local or UNC copy.
    Windows::Foundation::IAsyncOperation<Control::HyperlinkPreview> HyperlinkPreviewService::GetRemoteFilePreviewAsync(hstring remoteFileUri, hstring distro)
    {
        const auto cancellation = co_await get_cancellation_token();
        co_await resume_background();
        if (cancellation()) throw hresult_canceled();
        Control::HyperlinkPreview preview;
        preview.IntegrationName(L"File");
        preview.IntegrationIcon(L"\uE8A5");
        preview.SourceText(remoteFileUri);
        const auto parts = ::Microsoft::Console::Utils::SplitRemoteFileUri(std::wstring_view{ remoteFileUri });
        if (!parts) co_return Control::HyperlinkPreview{ nullptr };
        const auto host = parts->first;
        const auto posixPath = parts->second;
        const auto name = posixPath.substr(posixPath.find_last_of(L'/') + 1);
        const auto& fileType = Lintel::FindFileType(posixPath);
        preview.IntegrationName(fileType.name);
        preview.IntegrationIcon(fileType.icon);
        auto fields = single_threaded_vector<Control::HyperlinkPreviewField>();
        Control::HyperlinkPreviewField title;
        title.Kind(Control::HyperlinkPreviewFieldKind::Title);
        title.Value(name);
        fields.Append(title);
        Control::HyperlinkPreviewField metadata;
        metadata.IconUri(fileType.icon);
        const auto describe = [&](std::wstring_view detail) {
            metadata.Value(std::wstring{ fileType.name } + L" \u00B7 on " + host + L" \u00B7 " + std::wstring{ detail });
        };
        // Said plainly: these are facts about the file. Everything else -- no reader, no
        // connection, a refusal, a timeout, a reply that is not one -- is quietly unavailable.
        static constexpr auto unavailable = L"Preview unavailable";
        // The same split as a local file: these formats need a file on disk to render.
        auto ext = std::filesystem::path{ posixPath }.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        static constexpr std::wstring_view needsDisk[]{ L".pdf", L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff", L".docx", L".xlsx", L".pptx" };
        if (std::find(std::begin(needsDisk), std::end(needsDisk), ext) != std::end(needsDisk))
        {
            describe(unavailable);
        }
        else
        {
            std::wstring commandLine{ L"wsl.exe" };
            if (!distro.empty()) commandLine += L" -d " + ::Microsoft::Console::Utils::QuoteCommandLineArgument(distro);
            const auto script = fmt::format(LR"(exec shefrd machine read "$1" "$2" --max-bytes {} 2>/dev/null)", TextLimit);
            for (const auto& argument : { std::wstring{ L"--exec" }, std::wstring{ L"sh" }, std::wstring{ L"-lc" }, script, std::wstring{ L"sh" }, host, posixPath })
            {
                commandLine += L' ' + ::Microsoft::Console::Utils::QuoteCommandLineArgument(argument);
            }
            const auto capture = ::TerminalUtils::CaptureProcessEx(std::move(commandLine), {}, RemoteReadTimeoutMs, TextLimit + 64 * 1024);
            if (cancellation()) throw hresult_canceled();
            auto reply = ParseRemoteReadReply(capture.Output, TextLimit);
            switch (reply.status)
            {
            case RemoteReadStatus::Ok:
            {
                auto decoded = DecodeText(std::move(reply.bytes), reply.truncated);
                if (!decoded)
                {
                    describe(L"No built-in preview for this binary format or text encoding");
                    break;
                }
                std::wostringstream sizeLabel;
                if (reply.size < 1024) sizeLabel << reply.size << (reply.size == 1 ? L" byte" : L" bytes");
                else if (reply.size < 1024 * 1024) sizeLabel << std::fixed << std::setprecision(1) << static_cast<double>(reply.size) / 1024 << L" KiB";
                else sizeLabel << std::fixed << std::setprecision(1) << static_cast<double>(reply.size) / (1024 * 1024) << L" MiB";
                describe(sizeLabel.str());
                Control::HyperlinkPreviewTab section;
                section.Label(L"Text");
                section.Body(*decoded);
                preview.FileSections(single_threaded_vector<Control::HyperlinkPreviewTab>({ section }));
                preview.FileKind(fileType.language == L"markdown" ? L"markdown" : L"text");
                break;
            }
            case RemoteReadStatus::NotFound:
                describe(L"Not found");
                break;
            case RemoteReadStatus::NotRegular:
                describe(L"Not a regular file");
                break;
            case RemoteReadStatus::TooLarge:
                describe(L"Too large to preview");
                break;
            default:
                describe(unavailable);
                break;
            }
        }
        fields.Append(metadata);
        preview.Fields(fields);
        co_return preview;
    }
}
