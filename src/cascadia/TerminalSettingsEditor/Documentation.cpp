// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Documentation.h"
#include "Documentation.g.cpp"

// For Breadcrumb, which declares no constructor in the IDL and therefore has no
// activation factory: the only way to make one is winrt::make<> on the
// implementation type, which lives here.
#include "MainPage.h"
#include "NavConstants.h"

#include <til/io.h>
// The editor's pch has no reason to carry Documents: nothing else in it walks a
// RichTextBlock's inlines. The markdown renderer hands one back, so this page does.
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Microsoft.Terminal.UI.Markdown.h>

namespace winrt
{
    namespace WUX = Windows::UI::Xaml;
    namespace MTUM = Microsoft::Terminal::UI::Markdown;
}

using namespace winrt::Windows::UI::Xaml::Navigation;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    // Any https URI under this host is a link to another topic rather than to the
    // web. It is handed to the renderer as the base URL, so a plain relative link
    // such as [extensions](extensions.md) resolves to https://<host>/extensions.md
    // and comes back out of Hyperlink::NavigateUri intact, which is what lets the
    // walk below find it again.
    //
    // A real https URI rather than a scheme of our own, because the renderer builds
    // the link inside a try/catch: a URI Windows.Foundation.Uri refuses to parse
    // leaves the Hyperlink with no NavigateUri at all, and then there is nothing to
    // match on. The host is under .invalid (RFC 2606), which can never resolve, so a
    // link this page somehow fails to rewrite is inert rather than pointing at a real
    // site.
    static constexpr std::wstring_view DocsLinkHost{ L"docs.windows-terminal.invalid" };
    static constexpr std::wstring_view DocsBaseUrl{ L"https://docs.windows-terminal.invalid/" };

    // Docs\<id>.md, beside the binaries. GetModuleFileNameW on this DLL rather than
    // the package API: it is right packaged or not, and needs nothing async.
    static std::filesystem::path _topicPath(const std::wstring_view id)
    {
        std::filesystem::path modulePath{ wil::GetModuleFileNameW<std::wstring>(wil::GetModuleInstanceHandle()) };
        modulePath.remove_filename();
        modulePath /= L"Docs";
        modulePath /= std::wstring{ id } + L".md";
        return modulePath;
    }

    // A topic id names a file, so it is spelled out in full rather than sanitized:
    // lowercase letters, digits and hyphens, nothing else. No dots and no slashes
    // means no id can climb out of the Docs directory.
    static bool _isTopicId(const std::wstring_view id) noexcept
    {
        if (id.empty() || id.size() > 64)
        {
            return false;
        }
        return std::all_of(id.begin(), id.end(), [](const wchar_t ch) noexcept {
            return (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') || ch == L'-';
        });
    }

    // The topic a rendered link points at, or an empty string if the link goes
    // somewhere on the actual internet and should be left alone.
    static hstring _topicFromUri(const Windows::Foundation::Uri& uri)
    {
        if (!uri || uri.Host() != DocsLinkHost)
        {
            return {};
        }

        std::wstring path{ uri.Path() };
        while (!path.empty() && path.front() == L'/')
        {
            path.erase(path.begin());
        }
        if (path.ends_with(L".md"))
        {
            path.resize(path.size() - 3);
        }
        return _isTopicId(path) ? hstring{ path } : hstring{};
    }

    using LinkVisitor = std::function<void(const WUX::Documents::Hyperlink&)>;

    static void _visitElement(const WUX::UIElement& element, const LinkVisitor& visit);
    static void _visitInlines(const WUX::Documents::InlineCollection& inlines, const LinkVisitor& visit);

    static void _visitRichTextBlock(const WUX::Controls::RichTextBlock& block, const LinkVisitor& visit)
    {
        for (const auto& child : block.Blocks())
        {
            if (const auto& paragraph = child.try_as<WUX::Documents::Paragraph>())
            {
                _visitInlines(paragraph.Inlines(), visit);
            }
        }
    }

    static void _visitInlines(const WUX::Documents::InlineCollection& inlines, const LinkVisitor& visit)
    {
        if (!inlines)
        {
            return;
        }
        for (const auto& item : inlines)
        {
            // Hyperlink derives from Span, so it has to be tested first.
            if (const auto& link = item.try_as<WUX::Documents::Hyperlink>())
            {
                visit(link);
                _visitInlines(link.Inlines(), visit);
            }
            else if (const auto& span = item.try_as<WUX::Documents::Span>())
            {
                _visitInlines(span.Inlines(), visit);
            }
            else if (const auto& container = item.try_as<WUX::Documents::InlineUIContainer>())
            {
                // Tables, callouts and code blocks arrive as elements hosted inline,
                // and each table cell is a RichTextBlock of its own, so a link in a
                // table is two levels down from here rather than one.
                _visitElement(container.Child(), visit);
            }
        }
    }

    static void _visitElement(const WUX::UIElement& element, const LinkVisitor& visit)
    {
        if (!element)
        {
            return;
        }
        if (const auto& block = element.try_as<WUX::Controls::RichTextBlock>())
        {
            _visitRichTextBlock(block, visit);
        }
        else if (const auto& panel = element.try_as<WUX::Controls::Panel>())
        {
            for (const auto& child : panel.Children())
            {
                _visitElement(child, visit);
            }
        }
        else if (const auto& border = element.try_as<WUX::Controls::Border>())
        {
            _visitElement(border.Child(), visit);
        }
    }

    Documentation::Documentation()
    {
        InitializeComponent();
    }

    void Documentation::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _weakWindowRoot = args.WindowRoot();

        // A search result and each of the three "Learn more" links arrive the same
        // way: as an element name. A topic card's name opens the topic rather than
        // scrolling to the card, because the text is what was being looked for.
        // Anything else falls through to the usual bring-into-view.
        const auto elementToFocus = args.ElementToFocus();
        hstring topic{};
        if (!elementToFocus.empty())
        {
            if (const auto& named = FindName(elementToFocus).try_as<WUX::FrameworkElement>())
            {
                if (const auto& tag = named.Tag().try_as<hstring>())
                {
                    topic = *tag;
                }
            }
        }

        if (!topic.empty())
        {
            // Deferred to the next dispatcher tick, and that is load-bearing rather
            // than tidiness: MainPage navigates the frame first and appends the
            // "Documentation" crumb afterwards, so we are running *before* the trail
            // has its first entry. Opening the topic here would push our crumb in
            // front of the page's own and the trail would read backwards.
            if (const auto queue = Windows::System::DispatcherQueue::GetForCurrentThread())
            {
                queue.TryEnqueue([weakThis{ get_weak() }, topic]() {
                    if (const auto self{ weakThis.get() })
                    {
                        self->OpenTopic(topic);
                    }
                });
            }
        }
        else
        {
            BringIntoViewWhenLoaded(elementToFocus);
        }

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("documentation", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    void Documentation::TopicCard_Click(const Windows::Foundation::IInspectable& sender,
                                       const WUX::RoutedEventArgs& /*e*/)
    {
        if (const auto& card = sender.try_as<WUX::FrameworkElement>())
        {
            if (const auto& tag = card.Tag().try_as<hstring>())
            {
                OpenTopic(*tag);
            }
        }
    }

    void Documentation::OpenTopic(const hstring& id)
    {
        if (!_isTopicId(id))
        {
            return;
        }

        const auto content = TopicContent();
        content.Children().Clear();

        const auto path = _topicPath(id);
        const auto markdown = til::io::read_file_as_utf8_string_if_exists(path);

        if (markdown.empty())
        {
            // A topic whose file did not reach the package is the one failure mode
            // here that produces no build error at all, so say what is missing and
            // where it was looked for rather than drawing an empty card.
            WUX::Controls::TextBlock message;
            message.Text(RS_(L"Documentation_Missing/Text"));
            message.TextWrapping(WUX::TextWrapping::WrapWholeWords);
            content.Children().Append(message);

            WUX::Controls::TextBlock location;
            location.Text(hstring{ path.wstring() });
            location.FontFamily(WUX::Media::FontFamily{ L"Cascadia Mono, Consolas" });
            location.FontSize(12);
            location.IsTextSelectionEnabled(true);
            location.TextWrapping(WUX::TextWrapping::Wrap);
            location.Margin(WUX::ThicknessHelper::FromLengths(0, 8, 0, 0));
            content.Children().Append(location);
        }
        else
        {
            auto rendered = MTUM::Builder::Convert(winrt::to_hstring(markdown), hstring{ DocsBaseUrl });

            // Links between topics stay inside the app. NavigateUri has to be
            // cleared as well as handled: a Hyperlink that has one never raises
            // Click, it hands the URI to the shell instead.
            _visitRichTextBlock(rendered, [weakThis{ get_weak() }](const WUX::Documents::Hyperlink& link) {
                const auto topic = _topicFromUri(link.NavigateUri());
                if (topic.empty())
                {
                    return;
                }
                link.NavigateUri(nullptr);
                // No ToolTipService::SetToolTip here, and that is load-bearing. This
                // walk runs before the RichTextBlock is appended to the page, and a
                // Hyperlink off the live tree has no owner for XAML to register the
                // tooltip with: CHyperlink::UnregisterToolTip dereferenced null
                // (read at 0x2a8) and took the whole Terminal down the moment any
                // topic containing a link to another topic was opened. Captured
                // under cdb, not inferred. The link text already names the topic.
                link.Click([weakThis, topic](auto&&, auto&&) {
                    if (const auto self{ weakThis.get() })
                    {
                        self->OpenTopic(topic);
                    }
                });
            });

            content.Children().Append(rendered);
        }

        TopicList().Visibility(WUX::Visibility::Collapsed);
        TopicBody().Visibility(WUX::Visibility::Visible);

        // The crumb's label is the card's own header, found by the Tag that named the
        // file, so the trail and the topic list cannot disagree about a topic's name
        // and a translated header translates the crumb with it.
        hstring label{ id };
        for (const auto& child : TopicList().Children())
        {
            const auto& element = child.try_as<WUX::FrameworkElement>();
            if (!element)
            {
                continue;
            }
            const auto& tag = element.Tag().try_as<hstring>();
            if (!tag || *tag != id)
            {
                continue;
            }
            if (const auto& card = element.try_as<Editor::SettingsCard>())
            {
                if (const auto& header = card.Header().try_as<hstring>())
                {
                    label = *header;
                }
            }
            break;
        }

        _showCrumb(label);
        _scrollToTop();
    }

    void Documentation::_showCrumb(const hstring& label)
    {
        const auto windowRoot = _weakWindowRoot.get();
        if (!windowRoot)
        {
            return;
        }
        const auto mainPage = windowRoot.try_as<Editor::MainPage>();
        if (!mainPage)
        {
            return;
        }
        const auto crumbs = mainPage.Breadcrumbs();
        if (!crumbs)
        {
            return;
        }

        const auto crumb = winrt::make<Breadcrumb>(box_value(hstring{ documentationTag }), label, BreadcrumbSubPage::None);
        if (_pushedCrumb && crumbs.Size() > 1)
        {
            // Following a link from one topic to another replaces our crumb rather
            // than nesting: there is no hierarchy here, only sibling topics.
            crumbs.SetAt(crumbs.Size() - 1, crumb);
        }
        else
        {
            crumbs.Append(crumb);
            _pushedCrumb = true;
        }
    }

    void Documentation::_scrollToTop()
    {
        // The page does not own its scroll viewer, MainPage does, so walk up to
        // whichever one is hosting us. Opening a topic from a card near the bottom of
        // the list would otherwise start the reader halfway down the text.
        WUX::DependencyObject current = TopicBody();
        for (auto depth = 0; current && depth < 16; ++depth)
        {
            if (const auto& viewer = current.try_as<WUX::Controls::ScrollViewer>())
            {
                viewer.ChangeView(nullptr, 0.0, nullptr);
                return;
            }
            current = WUX::Media::VisualTreeHelper::GetParent(current);
        }
    }
}
