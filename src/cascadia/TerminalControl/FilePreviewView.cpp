// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "HyperlinkPreview.h"
#include <cmath>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include "../inc/LintelFileTypes.g.h"
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

namespace winrt::Microsoft::Terminal::Control::implementation
{
    namespace
    {
        using namespace Windows::UI::Xaml;
        namespace Controls = Windows::UI::Xaml::Controls;
        using Windows::Data::Pdf::PdfDocument;
        struct FileImageState
        {
            weak_ref<Controls::Image> image;
            weak_ref<Controls::ScrollViewer> scroll;
            weak_ref<Controls::ScrollViewer> viewport;
            weak_ref<Controls::StackPanel> toolbar;
            FrameworkElement::SizeChanged_revoker viewportChanged;
            weak_ref<Controls::Primitives::ToggleButton> fitButton;
            double width = 0;
            double height = 0;
            bool fit = false;
            weak_ref<Controls::TextBlock> status;
            weak_ref<Controls::ComboBox> pages;
            PdfDocument document{ nullptr };
            uint32_t generation = 0;
            bool closed = false;
            bool compact = false;
        };

        void SizeImage(const std::shared_ptr<FileImageState>& state)
        {
            const auto image = state->image.get();
            const auto scroll = state->scroll.get();
            if (!image || !scroll || state->compact || state->width <= 0 || state->height <= 0) return;
            double scale = 1;
            if (state->fit)
            {
                // Fit within the pane's visible body, not only the media scroller's
                // maximum. A short pane must still show the entire image/page.
                const auto width = scroll.ActualWidth() - 16;
                auto height = scroll.MaxHeight() - 16;
                if (const auto viewport = state->viewport.get(); viewport && viewport.ActualHeight() > 0)
                {
                    double chrome = 12;
                    if (const auto toolbar = state->toolbar.get()) chrome += toolbar.ActualHeight();
                    if (const auto status = state->status.get()) chrome += status.ActualHeight();
                    height = std::min(height, viewport.ActualHeight() - chrome - 16);
                }
                if (width <= 0 || height <= 0) return;
                scale = std::min(width / state->width, height / state->height);
            }
            image.Width(state->width * scale);
            image.Height(state->height * scale);
        }

        fire_and_forget RenderPage(std::shared_ptr<FileImageState> state, uint32_t index)
        {
            const auto generation = ++state->generation;
            const apartment_context ui;
            hstring failure;
            try
            {
                const auto document = state->document;
                if (!document) co_return;
                co_await resume_background();
                auto page = document.GetPage(index);
                Windows::Storage::Streams::InMemoryRandomAccessStream stream;
                Windows::Data::Pdf::PdfPageRenderOptions options;
                const auto size = page.Size();
                if (size.Width <= 0 || size.Height <= 0 || size.Height / size.Width > 16)
                    throw hresult_error(E_FAIL, L"Page dimensions exceed the preview limit.");
                const auto width = std::min(state->compact ? 640u : 1600u, static_cast<uint32_t>(std::sqrt(16000000.0 * size.Width / size.Height)));
                options.DestinationWidth(std::max(1u, width));
                co_await page.RenderToStreamAsync(stream, options);
                page.Close();
                co_await ui;
                if (state->closed || state->generation != generation) co_return;
                Media::Imaging::BitmapImage bitmap;
                co_await bitmap.SetSourceAsync(stream);
                if (state->closed || state->generation != generation) co_return;
                state->width = bitmap.PixelWidth();
                state->height = bitmap.PixelHeight();
                SizeImage(state);
                if (const auto image = state->image.get()) image.Source(bitmap);
                if (const auto status = state->status.get()) status.Text(L"Page " + to_hstring(index + 1) + L" of " + to_hstring(document.PageCount()));
            }
            catch (const hresult_error& error)
            {
                failure = error.message();
            }
            catch (...) { failure = L"File could not be rendered."; }
            co_await ui;
            if (!failure.empty() && !state->closed && state->generation == generation)
                if (const auto status = state->status.get()) status.Text(failure);
        }

        fire_and_forget LoadImage(std::shared_ptr<FileImageState> state, hstring path, bool pdf)
        {
            const auto generation = ++state->generation;
            const apartment_context ui;
            hstring failure;
            try
            {
                co_await resume_background();
                const auto file = co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
                const auto stream = co_await file.OpenReadAsync();
                if (stream.Size() > 50 * 1024 * 1024) throw hresult_error(E_FAIL, L"File exceeds the 50 MiB preview limit.");
                if (pdf)
                {
                    auto document = co_await PdfDocument::LoadFromStreamAsync(stream);
                    co_await ui;
                    if (state->closed || state->generation != generation) co_return;
                    state->document = document;
                    if (const auto pages = state->pages.get(); pages && !state->compact)
                    {
                        // Populate on demand instead of constructing one item per page of a huge PDF.
                        pages.Items().Clear();
                        const auto count = std::min(document.PageCount(), 10000u);
                        for (uint32_t i = 0; i < count; ++i) pages.Items().Append(box_value(L"Page " + to_hstring(i + 1)));
                        pages.SelectedIndex(0);
                    }
                    else RenderPage(state, 0);
                }
                else
                {
                    const auto decoder = co_await Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(stream);
                    const auto width = decoder.PixelWidth();
                    const auto height = decoder.PixelHeight();
                    if (static_cast<uint64_t>(width) * height > 16000000)
                        throw hresult_error(E_FAIL, L"Image exceeds the 16 megapixel preview limit.");
                    co_await ui;
                    if (state->closed || state->generation != generation) co_return;
                    Media::Imaging::BitmapImage bitmap;
                    bitmap.DecodePixelWidth(state->compact ? 640 : std::min(width, 4096u));
                    stream.Seek(0);
                    co_await bitmap.SetSourceAsync(stream);
                    if (state->closed || state->generation != generation) co_return;
                    state->width = width;
                    state->height = height;
                    SizeImage(state);
                    if (const auto status = state->status.get()) status.Text(to_hstring(width) + L" × " + to_hstring(height));
                    if (const auto image = state->image.get()) image.Source(bitmap);
                }
            }
            catch (const hresult_error& error)
            {
                failure = error.message();
            }
            catch (...) { failure = L"File could not be rendered."; }
            co_await ui;
            if (!failure.empty() && !state->closed && state->generation == generation)
                if (const auto status = state->status.get()) status.Text(failure);
        }
    }

    Windows::UI::Xaml::FrameworkElement HyperlinkPreviewHelpers::CreateFileView(const Control::HyperlinkPreview& preview, bool compact)
    {
        using namespace Windows::UI::Xaml;
        namespace Controls = Windows::UI::Xaml::Controls;
        if (!preview || preview.FileKind().empty() || !preview.Error().empty()) return nullptr;
        Controls::StackPanel root;
        root.Spacing(6);
        Controls::ScrollViewer scroll;
        scroll.MaxHeight(compact ? 560 : 800);
        scroll.HorizontalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
        scroll.VerticalScrollBarVisibility(Controls::ScrollBarVisibility::Auto);
        if (preview.FileKind() == L"image" || preview.FileKind() == L"pdf")
        {
            auto state = std::make_shared<FileImageState>();
            state->compact = compact;
            state->scroll = make_weak(scroll);
            scroll.SizeChanged([state](auto&&, auto&&) { if (!state->closed && state->fit) SizeImage(state); });
            Controls::TextBlock status;
            status.Text(L"Loading preview…");
            status.TextWrapping(TextWrapping::Wrap);
            state->status = make_weak(status);
            if (!compact && preview.FileKind() == L"pdf")
            {
                Controls::ComboBox pages;
                state->pages = make_weak(pages);
                pages.SelectionChanged([state](const Windows::Foundation::IInspectable& sender, const Controls::SelectionChangedEventArgs&) {
                    const auto index = sender.as<Controls::ComboBox>().SelectedIndex();
                    if (!state->closed && index >= 0) RenderPage(state, static_cast<uint32_t>(index));
                });
                root.Children().Append(pages);
            }
            Controls::Image image;
            image.Stretch(Media::Stretch::Uniform);
            state->image = make_weak(image);
            scroll.Content(image);
            if (!compact)
            {
                scroll.ZoomMode(Controls::ZoomMode::Enabled);
                scroll.MinZoomFactor(0.25f);
                scroll.MaxZoomFactor(4.0f);
                Controls::StackPanel zoom;
                state->toolbar = make_weak(zoom);
                zoom.Orientation(Controls::Orientation::Horizontal);
                Controls::Primitives::ToggleButton fit;
                fit.Content(box_value(L"Fit"));
                Controls::ToolTipService::SetToolTip(fit, box_value(L"Fit the whole image or page"));
                state->fitButton = make_weak(fit);
                fit.Click([state](auto&&, auto&&) {
                    state->fit = true;
                    if (const auto button = state->fitButton.get()) button.IsChecked(true);
                    SizeImage(state);
                    if (const auto view = state->scroll.get()) view.ChangeView(0.0, 0.0, 1.0f, true);
                });
                zoom.Children().Append(fit);
                for (const auto factor : { 0.5f, 1.0f, 2.0f })
                {
                    Controls::Button button;
                    button.Content(box_value(to_hstring(static_cast<int>(factor * 100)) + L"%"));
                    button.Click([state, factor](auto&&, auto&&) {
                        state->fit = false;
                        if (const auto button = state->fitButton.get()) button.IsChecked(false);
                        SizeImage(state);
                        if (const auto view = state->scroll.get()) view.ChangeView(0.0, 0.0, factor, true);
                    });
                    zoom.Children().Append(button);
                }
                root.Children().Append(zoom);
            }
            root.Children().Append(status);
            root.Unloaded([state](auto&&, auto&&) {
                state->closed = true; ++state->generation; state->document = nullptr;
                state->viewportChanged.revoke();
            });
            root.Loaded([state, path = preview.FilePath(), pdf = preview.FileKind() == L"pdf"](const Windows::Foundation::IInspectable& sender, auto&&) {
                state->closed = false;
                auto parent = Media::VisualTreeHelper::GetParent(sender.as<DependencyObject>());
                while (parent)
                {
                    if (const auto viewport = parent.try_as<Controls::ScrollViewer>())
                    {
                        state->viewport = make_weak(viewport);
                        state->viewportChanged = viewport.SizeChanged(auto_revoke, [weak = std::weak_ptr<FileImageState>{ state }](auto&&, auto&&) {
                            if (const auto current = weak.lock(); current && !current->closed && current->fit) SizeImage(current);
                        });
                        break;
                    }
                    parent = Media::VisualTreeHelper::GetParent(parent);
                }
                LoadImage(state, path, pdf);
            });
        }
        else if (preview.FileKind() == L"markdown")
        {
            const auto sections = preview.FileSections();
            if (!sections || sections.Size() == 0) return nullptr;
            auto source = std::wstring{ sections.GetAt(0).Body() };
            const size_t limit = compact ? 65536 : 262144;
            const bool truncated = source.size() > limit;
            if (truncated) source.resize(limit);
            Controls::StackPanel buttons;
            buttons.Orientation(Controls::Orientation::Horizontal);
            buttons.Spacing(4);
            Controls::Primitives::ToggleButton formatted;
            Controls::Primitives::ToggleButton raw;
            formatted.Content(box_value(L"Formatted"));
            raw.Content(box_value(L"Raw"));
            buttons.Children().Append(formatted);
            buttons.Children().Append(raw);
            const auto show = [view = make_weak(scroll), a = make_weak(formatted), b = make_weak(raw), source = hstring{ source }, path = preview.FilePath()](bool rawMode) {
                if (const auto button = a.get()) button.IsChecked(!rawMode);
                if (const auto button = b.get()) button.IsChecked(rawMode);
                if (const auto viewControl = view.get())
                {
                    viewControl.HorizontalScrollMode(rawMode ? Controls::ScrollMode::Auto : Controls::ScrollMode::Disabled);
                    viewControl.HorizontalScrollBarVisibility(rawMode ? Controls::ScrollBarVisibility::Auto : Controls::ScrollBarVisibility::Disabled);
                    try { viewControl.Content(winrt::Microsoft::Terminal::UI::Markdown::Builder::Preview(source, path, rawMode)); }
                    catch (...) { viewControl.Content(winrt::Microsoft::Terminal::UI::Markdown::Builder::Highlight(source, L"markdown")); }
                    viewControl.ChangeView(0.0, 0.0, nullptr);
                }
            };
            formatted.Click([show](auto&&, auto&&) { show(false); });
            raw.Click([show](auto&&, auto&&) { show(true); });
            root.Children().Append(buttons);
            if (truncated)
            {
                Controls::TextBlock notice;
                notice.Text(compact ? L"Preview shortened. Open in pane for more." : L"Preview shortened. Open the file for the complete document.");
                notice.TextWrapping(TextWrapping::Wrap);
                root.Children().Append(notice);
            }
            show(false);
        }
        else
        {
            const auto sections = preview.FileSections();
            if (!sections || sections.Size() == 0) return nullptr;
            Controls::TextBlock text;
            text.FontFamily(Media::FontFamily{ L"Consolas" });
            text.IsTextSelectionEnabled(true);
            auto show = [weak = make_weak(text), view = make_weak(scroll), sections, compact, sheet = preview.FileKind() == L"sheet", numbered = preview.FileKind() == L"text", language = preview.FileKind() == L"folder" ? std::wstring{} : std::wstring{ Lintel::FindFileType(preview.FilePath()).language }](uint32_t index) {
                std::wstring body{ sections.GetAt(index).Body() };
                std::wstring result;
                size_t start = 0;
                size_t line = 1;
                while (start < body.size())
                {
                    const auto end = body.find(L'\n', start);
                    if (compact && line > 200) { result += L"\n[Open in pane for more]"; break; }
                    if (numbered && !compact) result += std::to_wstring(line) + L"  ";
                    result.append(body, start, end == std::wstring::npos ? body.size() - start : end - start + 1);
                    if (end == std::wstring::npos) break;
                    start = end + 1;
                    ++line;
                }
                if (sheet)
                {
                    Controls::Grid grid;
                    size_t rowStart = 0;
                    int row = 0;
                    while (rowStart < result.size() && row < 202)
                    {
                        const auto rowEnd = result.find(L'\n', rowStart);
                        const auto lineText = result.substr(rowStart, rowEnd == std::wstring::npos ? result.size() - rowStart : rowEnd - rowStart);
                        Controls::RowDefinition definition;
                        definition.Height(GridLength{ 0, GridUnitType::Auto });
                        grid.RowDefinitions().Append(definition);
                        size_t cellStart = 0;
                        int column = 0;
                        while (cellStart < lineText.size() && column < 50)
                        {
                            const auto cellEnd = lineText.find(L'\t', cellStart);
                            if (grid.ColumnDefinitions().Size() <= static_cast<uint32_t>(column))
                            {
                                Controls::ColumnDefinition col;
                                col.Width(GridLength{ 0, GridUnitType::Auto });
                                grid.ColumnDefinitions().Append(col);
                            }
                            Controls::TextBlock cell;
                            cell.Text(lineText.substr(cellStart, cellEnd == std::wstring::npos ? lineText.size() - cellStart : cellEnd - cellStart));
                            cell.Margin(Thickness{ 4, 2, 12, 2 });
                            cell.IsTextSelectionEnabled(true);
                            Controls::Grid::SetRow(cell, row);
                            Controls::Grid::SetColumn(cell, column++);
                            grid.Children().Append(cell);
                            if (cellEnd == std::wstring::npos) break;
                            cellStart = cellEnd + 1;
                        }
                        ++row;
                        if (rowEnd == std::wstring::npos) break;
                        rowStart = rowEnd + 1;
                    }
                    if (const auto control = view.get()) control.Content(grid);
                }
                else if (!language.empty())
                {
                    if (const auto control = view.get()) control.Content(winrt::Microsoft::Terminal::UI::Markdown::Builder::Highlight(result, language));
                }
                else if (const auto control = weak.get()) control.Text(result);
            };
            if (!compact && sections.Size() > 1)
            {
                Controls::ComboBox selector;
                for (const auto& section : sections) selector.Items().Append(box_value(section.Label()));
                selector.SelectionChanged([show](const Windows::Foundation::IInspectable& sender, const Controls::SelectionChangedEventArgs&) {
                    const auto index = sender.as<Controls::ComboBox>().SelectedIndex();
                    if (index >= 0) show(static_cast<uint32_t>(index));
                });
                selector.SelectedIndex(0);
                root.Children().Append(selector);
            }
            scroll.Content(text);
            show(0);
        }
        root.Children().Append(scroll);
        return root;
    }
}
