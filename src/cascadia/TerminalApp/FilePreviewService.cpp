// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "HyperlinkPreviewService.h"
#include "LinkPreviewPaneContent.h"
#include "../TerminalControl/HyperlinkRules.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include "FilePreviewReader.h"
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
                Windows::ApplicationModel::DataTransfer::DataPackage data;
                data.SetText(actionId == L"copyPath" ? path : target);
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
        if (effective.showOpen) addButton(L"Open", L"open");
        if (effective.showCopyLink) addButton(L"Copy link", L"copyLink");
        if (isFile && effective.showCopyPath) addButton(L"Copy path", L"copyPath");
        if (isFile && effective.showReveal) addButton(L"Show in Explorer", L"reveal");
        if (effective.showInPane) addButton(L"Show in pane", L"showInPane");
        for (const auto& action : effective.customActions) addButton(std::wstring_view{ action.Name() }, action.ActionId());
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
            if (!std::filesystem::is_regular_file(path)) throw hresult_error(E_FAIL, L"This path is not an accessible regular file.");
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
}
