// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CodeBlock.h"
#include "MarkdownToXaml.h"
#include "MarkdownBlocks.h"
#include <winrt/Windows.UI.Xaml.Automation.h>

#include <cmark.h>

namespace winrt
{
    namespace MUX = Microsoft::UI::Xaml;
    namespace WUX = winrt::Windows::UI::Xaml;
    using IInspectable = Windows::Foundation::IInspectable;
}
using namespace winrt;

// Bullet points used for unordered lists.
static constexpr std::wstring_view bullets[]{
    L"• ",
    L"◦ ",
    L"▪ " // After this level, we'll keep using this one.
};
static constexpr int WidthOfBulletPoint{ 9 };
static constexpr int IndentWidth{ 3 * WidthOfBulletPoint };

// The vertical rhythm, taken from GitHub's own markdown stylesheet and kept in
// em so it scales with the body text. The model is GitHub's too: every block
// owns only the space BELOW it, and nothing owns space above except a heading,
// which asks for a little more than the block before it already left. That
// matters here more than in HTML, because XAML block margins add where CSS
// margins collapse - two blocks each claiming a gap on the side they share
// produce double the space, which is how this renderer used to get both too
// little air under a heading and odd extra air above one.
namespace Rhythm
{
    // Line height of body text. The font's own default is roughly 1.2 and
    // reads as a wall; 1.5 is GitHub's.
    static constexpr double BodyLineHeight{ 1.5 };
    // Headings are set tighter than body text, as they are on GitHub.
    static constexpr double HeadingLineHeight{ 1.25 };
    // Space under a paragraph, table, code block, callout, or a whole list.
    static constexpr double BlockGap{ 1.0 };
    // Space between the items of a tight list (".25em" on GitHub). A loose
    // list - one with blank lines between items - gets a full BlockGap.
    static constexpr double TightItemGap{ 0.25 };
    // Total space above a heading. The block before it already left BlockGap,
    // so the heading asks only for the difference.
    static constexpr double HeadingTopGap{ 1.5 };
    // Space between an h1/h2 and the rule GitHub draws under it.
    static constexpr double HeadingRulePadding{ 0.3 };
    // A thematic break (---) sits in 1.5em of space on both sides.
    static constexpr double ThematicBreakGap{ 1.5 };
}

// Heading sizes relative to body text, h1 through h6 - GitHub's scale.
static constexpr double HeadingScale[]{ 2.0, 1.5, 1.25, 1.0, 0.875, 0.85 };

static constexpr std::wstring_view CodeFontFamily{ L"Cascadia Mono, Consolas" };

template<typename T>
static std::string_view textFromCmarkString(const T& s) noexcept
{
    return std::string_view{ (char*)s.data, (size_t)s.len };
}
static std::string_view textFromLiteral(cmark_node* node) noexcept
{
    return cmark_node_get_literal(node);
}
static std::string_view textFromUrl(cmark_node* node) noexcept
{
    return cmark_node_get_url(node);
}

typedef wil::unique_any<cmark_node*, decltype(&cmark_node_free), cmark_node_free> unique_node;
typedef wil::unique_any<cmark_iter*, decltype(&cmark_iter_free), cmark_iter_free> unique_iter;

// Function Description:
// - Entrypoint to convert a string of markdown into a XAML RichTextBlock.
// Arguments:
// - markdownText: the markdown content to render
// - baseUrl: the current URI of the content. This will allow for relative links
//   to be appropriately resolved.
// Return Value:
// - a RichTextBlock with the rendered markdown in it.
WUX::Controls::RichTextBlock MarkdownToXaml::Convert(std::string_view markdownText, const winrt::hstring& baseUrl, size_t depth)
{
    MarkdownToXaml data{ baseUrl };
    if (depth > 8)
    {
        data._NewRun().Text(winrt::to_hstring(markdownText));
        return data._root;
    }
    for (const auto& block : MarkdownPreview::ParseRichBlocks(markdownText))
    {
        if (block.kind == MarkdownPreview::RichBlock::Kind::Table)
        {
            WUX::Controls::Grid table;
            const auto columns = std::min<size_t>(32, block.rows.front().size());
            for (size_t col = 0; col < columns; ++col)
            {
                WUX::Controls::ColumnDefinition column;
                column.Width(WUX::GridLength{ 1, WUX::GridUnitType::Star });
                table.ColumnDefinitions().Append(column);
            }
            for (size_t row = 0; row < block.rows.size(); ++row)
            {
                WUX::Controls::RowDefinition definition;
                definition.Height(WUX::GridLength{ 0, WUX::GridUnitType::Auto });
                table.RowDefinitions().Append(definition);
                for (size_t col = 0; col < columns; ++col)
                {
                    auto text = col < block.rows[row].size() ? block.rows[row][col] : std::string{};
                    for (size_t pos = 0; (pos = text.find("<br>", pos)) != text.npos; pos += 3) text.replace(pos, 4, "  \n");
                    auto body = Convert(text, baseUrl, depth + 1);
                    body.TextWrapping(WUX::TextWrapping::Wrap);
                    if (!row) body.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
                    WUX::Controls::Border cell;
                    // GitHub's cell padding, 6px by 13px: roomier across than
                    // down, because columns need separating more than rows do.
                    cell.Padding(WUX::Thickness{ 13, 6, 13, 6 });
                    cell.BorderThickness(WUX::Thickness{ 0.5, 0.5, 0.5, 0.5 });
                    cell.BorderBrush(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 70, 128, 128, 128 } });
                    // Header row shaded, then every second body row striped,
                    // so an eye can follow a wide row across without a ruler.
                    if (!row) cell.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 35, 128, 128, 128 } });
                    else if (row % 2 == 0) cell.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 14, 128, 128, 128 } });
                    cell.Child(body);
                    WUX::Controls::Grid::SetRow(cell, static_cast<int32_t>(row));
                    WUX::Controls::Grid::SetColumn(cell, static_cast<int32_t>(col));
                    table.Children().Append(cell);
                }
            }
            data._AppendBlock(table);
        }
        else if (block.kind == MarkdownPreview::RichBlock::Kind::Callout)
        {
            const bool warning = block.tone == "WARNING" || block.tone == "IMPORTANT";
            const bool error = block.tone == "ERROR" || block.tone == "CAUTION";
            const bool success = block.tone == "SUCCESS" || block.tone == "TIP";
            const winrt::Windows::UI::Color color = error ? winrt::Windows::UI::Color{ 40, 220, 65, 45 } : warning ? winrt::Windows::UI::Color{ 40, 220, 175, 0 } : success ? winrt::Windows::UI::Color{ 40, 40, 170, 95 } : winrt::Windows::UI::Color{ 40, 65, 130, 230 };
            WUX::Controls::Grid grid;
            WUX::Controls::ColumnDefinition iconColumn;
            iconColumn.Width(WUX::GridLength{ 28, WUX::GridUnitType::Pixel });
            grid.ColumnDefinitions().Append(iconColumn);
            WUX::Controls::ColumnDefinition bodyColumn;
            bodyColumn.Width(WUX::GridLength{ 1, WUX::GridUnitType::Star });
            grid.ColumnDefinitions().Append(bodyColumn);
            WUX::Controls::TextBlock icon;
            icon.Text(error ? L"⊗" : warning ? L"⚠" : success ? L"✓" : L"ⓘ");
            icon.FontSize(18);
            WUX::Automation::AutomationProperties::SetName(icon, winrt::to_hstring(block.tone));
            grid.Children().Append(icon);
            const auto body = Convert(block.text, baseUrl, depth + 1);
            WUX::Controls::Grid::SetColumn(body, 1);
            grid.Children().Append(body);
            WUX::Controls::Border card;
            card.Padding(WUX::Thickness{ 12, 10, 12, 10 });
            card.CornerRadius(WUX::CornerRadius{ 4, 4, 4, 4 });
            card.Background(WUX::Media::SolidColorBrush{ color });
            card.Child(grid);
            data._AppendBlock(card);
        }
        else
        {
            data._EndParagraph();
            unique_node doc{ cmark_parse_document(block.text.data(), block.text.size(), CMARK_OPT_DEFAULT) };
            unique_iter iter{ cmark_iter_new(doc.get()) };
            cmark_event_type ev_type;
            while ((ev_type = cmark_iter_next(iter.get())) != CMARK_EVENT_DONE)
                data._RenderNode(cmark_iter_get_node(iter.get()), ev_type);
        }
    }

    data._TrimOuterMargins();
    return data._root;
}

// The gap under an ordinary paragraph, which depends on where it is: inside a
// tight list the items sit close, everywhere else a paragraph gets a full gap.
double MarkdownToXaml::_paragraphBottomGap() const noexcept
{
    if (!_lists.empty() && _lists.back().tight)
    {
        return Rhythm::TightItemGap * _baseFontSize;
    }
    return Rhythm::BlockGap * _baseFontSize;
}

void MarkdownToXaml::_SetLastBlockBottom(double bottom)
{
    const auto blocks = _root.Blocks();
    if (blocks.Size() == 0)
    {
        return;
    }
    const auto last = blocks.GetAt(blocks.Size() - 1);
    auto margin = last.Margin();
    margin.Bottom = bottom;
    last.Margin(margin);
}

// Space belongs between blocks, not around the whole document. Whatever hosts
// this - a documentation page, a pane, a table cell, a hover card - already has
// its own padding, and a heading's top gap or a paragraph's bottom gap at the
// very edge would double it. Table cells and callouts are rendered by a nested
// Convert, so this is also what keeps a one-line cell one line tall.
void MarkdownToXaml::_TrimOuterMargins()
{
    const auto blocks = _root.Blocks();
    if (blocks.Size() == 0)
    {
        return;
    }
    const auto first = blocks.GetAt(0);
    auto top = first.Margin();
    top.Top = 0;
    first.Margin(top);
    _SetLastBlockBottom(0);
}

// A horizontal line across the text column: the rule under an h1/h2, and a
// thematic break (---). A paragraph holding one thin Border, with its line
// height pinned to the rule's thickness - otherwise the paragraph keeps the
// body line height and a 1px rule arrives with 20px of empty line around it.
void MarkdownToXaml::_AppendRule(double thickness, double topGap, double bottomGap)
{
    _EndParagraph();
    WUX::Controls::Border rule;
    rule.Height(thickness);
    rule.Background(WUX::Media::SolidColorBrush{ winrt::Windows::UI::Color{ 60, 128, 128, 128 } });

    WUX::Documents::InlineUIContainer container;
    container.Child(rule);
    auto paragraph = _CurrentParagraph();
    paragraph.Inlines().Append(container);
    paragraph.LineStackingStrategy(WUX::LineStackingStrategy::BlockLineHeight);
    paragraph.LineHeight(thickness);
    paragraph.Margin(WUX::ThicknessHelper::FromLengths(static_cast<double>(IndentWidth) * _indent, topGap, 0, bottomGap));

    const auto resize = [root = winrt::make_weak(_root), child = winrt::make_weak(rule), indent = _indent] {
        try
        {
            const auto owner = root.get();
            const auto content = child.get();
            if (owner && content && owner.ActualWidth() > 0)
                content.Width(std::max(1.0, owner.ActualWidth() - IndentWidth * indent));
        }
        CATCH_LOG();
    };
    _root.SizeChanged([resize](auto&&, auto&&) { resize(); });
    rule.Loaded([resize](auto&&, auto&&) { resize(); });
    _EndParagraph();
}

void MarkdownToXaml::_AppendBlock(const WUX::FrameworkElement& element)
{
    _EndParagraph();
    WUX::Documents::InlineUIContainer container;
    container.Child(element);
    _CurrentParagraph().Inlines().Append(container);
    // Only a gap below, like every other block. The left margin keeps the
    // list indent: overwriting it with zero, as this used to, pulled a code
    // block inside a list item out to the page edge while the resize below
    // still subtracted the indent from its width.
    _CurrentParagraph().Margin(WUX::ThicknessHelper::FromLengths(static_cast<double>(IndentWidth) * _indent, 0, 0, Rhythm::BlockGap * _baseFontSize));
    // Inline controls otherwise measure to their intrinsic width and can escape
    // the pane. Reflow them with the owning rich-text block on every resize.
    const auto resize = [root = winrt::make_weak(_root), child = winrt::make_weak(element), indent = _indent] {
        try
        {
            const auto owner = root.get();
            const auto content = child.get();
            if (owner && content && owner.ActualWidth() > 0)
                content.Width(std::max(1.0, owner.ActualWidth() - IndentWidth * indent));
        }
        CATCH_LOG();
    };
    _root.SizeChanged([resize](auto&&, auto&&) { resize(); });
    element.Loaded([resize](auto&&, auto&&) { resize(); });
    _EndParagraph();
}

MarkdownToXaml::MarkdownToXaml(const winrt::hstring& baseUrl) :
    _baseUri{ baseUrl }
{
    _root.ContextFlyout(winrt::Microsoft::Terminal::UI::TextMenuFlyout{});
    _root.IsTextSelectionEnabled(true);
    _root.TextWrapping(WUX::TextWrapping::WrapWholeWords);
    if (const auto size = _root.FontSize(); size > 0)
    {
        _baseFontSize = size;
    }
}

WUX::Documents::Paragraph MarkdownToXaml::_CurrentParagraph()
{
    if (_lastParagraph == nullptr)
    {
        _lastParagraph = WUX::Documents::Paragraph{};

        // A list item's marker is written as part of the paragraph's text, but
        // the item's text should still line up in a column. So a paragraph that
        // carries a marker hangs its first line out by the marker's width.
        // Only that one: a second paragraph in the same item has no marker and
        // lines up with the first one's text rather than with its bullet.
        if (_pendingMarkerWidth > 0)
        {
            _lastParagraph.TextIndent(-_pendingMarkerWidth);
            _pendingMarkerWidth = 0;
        }

        // MaxHeight stacking makes this a minimum, so a line holding something
        // taller - a heading, an inline image - still grows to fit it.
        _lastParagraph.LineStackingStrategy(WUX::LineStackingStrategy::MaxHeight);
        _lastParagraph.LineHeight(Rhythm::BodyLineHeight * _baseFontSize);
        _lastParagraph.Margin(WUX::ThicknessHelper::FromLengths(static_cast<double>(IndentWidth) * _indent, 0, 0, _paragraphBottomGap()));
        _root.Blocks().Append(_lastParagraph);
    }
    return _lastParagraph;
}
WUX::Documents::Run MarkdownToXaml::_CurrentRun()
{
    if (_lastRun == nullptr)
    {
        _lastRun = WUX::Documents::Run{};
        _CurrentSpan().Inlines().Append(_lastRun);
    }
    return _lastRun;
}
WUX::Documents::Span MarkdownToXaml::_CurrentSpan()
{
    if (_lastSpan == nullptr)
    {
        _lastSpan = WUX::Documents::Span{};
        _CurrentParagraph().Inlines().Append(_lastSpan);
    }
    return _lastSpan;
}
WUX::Documents::Run MarkdownToXaml::_NewRun()
{
    if (_lastRun == nullptr)
    {
        return _CurrentRun();
    }
    else
    {
        auto old{ _lastRun };

        WUX::Documents::Run newRun{};

        newRun.FontFamily(old.FontFamily());
        newRun.FontWeight(old.FontWeight());
        newRun.FontStyle(old.FontStyle());

        _lastRun = newRun;
        _CurrentSpan().Inlines().Append(_lastRun);
    }
    return _lastRun;
}
void MarkdownToXaml::_EndRun() noexcept
{
    _lastRun = nullptr;
}
void MarkdownToXaml::_EndSpan() noexcept
{
    _EndRun();
    _lastSpan = nullptr;
}
void MarkdownToXaml::_EndParagraph() noexcept
{
    _EndSpan();
    _lastParagraph = nullptr;
}

WUX::Controls::TextBlock MarkdownToXaml::_makeDefaultTextBlock()
{
    WUX::Controls::TextBlock b{};
    b.ContextFlyout(winrt::Microsoft::Terminal::UI::TextMenuFlyout{});
    b.IsTextSelectionEnabled(true);
    b.TextWrapping(WUX::TextWrapping::WrapWholeWords);
    return b;
}

void MarkdownToXaml::_RenderNode(cmark_node* node, cmark_event_type ev_type)
{
    const bool entering = (ev_type == CMARK_EVENT_ENTER);

    switch (cmark_node_get_type(node))
    {
    case CMARK_NODE_DOCUMENT:
        break;

    case CMARK_NODE_BLOCK_QUOTE:

        // It's non-trivial to deal with the right-side vertical lines that
        // we're accustomed to seeing for block quotes in markdown content.
        // RichTextBlock doesn't have a good way of adding a border to a
        // paragraph, it would seem.
        //
        // We could add a InlineUIContainer, with a Border in there, then
        // put a new RichTextBlock in there, but I believe text selection
        // wouldn't transit across the border.

        // Instead, we're just going to add a new layer of indenting.

        if (entering)
        {
            _EndParagraph();
            _indent++;
            _blockQuoteDepth++;
        }
        else
        {
            _EndParagraph();
            _indent = std::max(0, _indent - 1);
            _blockQuoteDepth = std::max(0, _blockQuoteDepth - 1);
        }

        break;

    case CMARK_NODE_LIST:
    {
        if (entering)
        {
            _EndParagraph();
            _indent++;
            ListState list;
            list.ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
            list.next = std::max(0, cmark_node_get_list_start(node));
            list.tight = cmark_node_get_list_tight(node) != 0;
            _lists.push_back(list);
        }
        else
        {
            _EndParagraph();
            _indent = std::max(0, _indent - 1);
            if (!_lists.empty())
            {
                _lists.pop_back();
            }
            // The last item of a tight list left only an item-sized gap. The
            // list as a whole is a block and owes the full gap to whatever
            // follows it - unless it is nested, in which case what follows is
            // the next item of the list around it.
            _SetLastBlockBottom(_paragraphBottomGap());
        }
        break;
    }

    case CMARK_NODE_ITEM:
        // A list item, either for an ordered list or an unordered one.
        if (entering)
        {
            _EndParagraph();
            if (!_lists.empty() && _lists.back().ordered)
            {
                // Numbered, as the source asked. The hanging indent grows with
                // the number so "10." lines up as well as "9." does.
                const auto number = _lists.back().next++;
                _pendingMarkerWidth = number >= 10 ? 26.0 : 19.0;
                _NewRun().Text(winrt::hstring{ fmt::format(FMT_COMPILE(L"{}. "), number) });
            }
            else
            {
                _pendingMarkerWidth = WidthOfBulletPoint;
                _NewRun().Text(gsl::at(bullets, std::clamp(static_cast<int>(_lists.size()) - 1, 0, 2)));
            }
        }
        break;

    case CMARK_NODE_HEADING:
    {
        _EndParagraph();

        // At the start of a header, change the font size to match the new
        // level of header we're at. The text will come later, in a
        // CMARK_NODE_TEXT
        const auto level = std::clamp(cmark_node_get_heading_level(node), 1, 6);
        const auto size = HeadingScale[level - 1] * _baseFontSize;
        if (entering)
        {
            auto heading = _CurrentParagraph();
            heading.FontSize(size);
            heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            heading.LineHeight(Rhythm::HeadingLineHeight * size);
            // Above: whatever the previous block did not already leave. Below:
            // the full gap, except an h1/h2, whose gap comes after its rule.
            const auto above = (Rhythm::HeadingTopGap - Rhythm::BlockGap) * _baseFontSize;
            const auto below = level <= 2 ? Rhythm::HeadingRulePadding * size : Rhythm::BlockGap * _baseFontSize;
            heading.Margin(WUX::ThicknessHelper::FromLengths(static_cast<double>(IndentWidth) * _indent, above, 0, below));
        }
        else if (level <= 2)
        {
            // GitHub rules off the two top levels, which is most of what makes
            // a long document scannable: the eye finds sections by the lines.
            _AppendRule(1, 0, Rhythm::BlockGap * _baseFontSize);
        }
        break;
    }

    case CMARK_NODE_CODE_BLOCK:
    {
        _EndParagraph();

        const auto codeHstring{ winrt::to_hstring(cmark_node_get_literal(node)) };
        // The literal for a code node always includes the trailing newline.
        // Trim that off.
        const std::wstring_view codeView{ codeHstring.c_str(), codeHstring.empty() ? 0 : codeHstring.size() - 1 };

        auto codeBlock = winrt::make<winrt::Microsoft::Terminal::UI::Markdown::implementation::CodeBlock>(winrt::hstring{ codeView }, winrt::to_hstring(cmark_node_get_fence_info(node) ? cmark_node_get_fence_info(node) : ""));
        _AppendBlock(codeBlock);
    }
    break;

    case CMARK_NODE_HTML_BLOCK:
        // Raw HTML comes to us in the literal
        //    node->as.literal.data, node->as.literal.len
        // But we don't support raw HTML, so we'll do nothing.
        break;

    case CMARK_NODE_CUSTOM_BLOCK:
        // Not even entirely sure what this is.
        break;

    case CMARK_NODE_THEMATIC_BREAK:
        if (entering)
        {
            // The previous block already left a BlockGap; top it up to the
            // break's own gap, and leave the same below.
            _AppendRule(2,
                        (Rhythm::ThematicBreakGap - Rhythm::BlockGap) * _baseFontSize,
                        Rhythm::ThematicBreakGap * _baseFontSize);
        }
        break;

    case CMARK_NODE_PARAGRAPH:
    {
        // A paragraph normally starts a block of its own. The exception is the
        // first paragraph of a list item, which has to continue the paragraph
        // the item already opened with its marker - ending it would leave the
        // bullet alone on a line with the text below it, which is what a
        // "loose" list (blank lines between items) used to render as.
        cmark_node* parent = cmark_node_parent(node);
        const bool firstInItem = entering &&
                                 parent != nullptr &&
                                 cmark_node_get_type(parent) == CMARK_NODE_ITEM &&
                                 cmark_node_previous(node) == nullptr;
        if (!firstInItem)
        {
            _EndParagraph();
        }
        break;
    }
    case CMARK_NODE_TEXT:
    {
        const auto text{ winrt::to_hstring(textFromLiteral(node)) };

        if (_lastImage)
        {
            // The tooltip for an image comes in as a CMARK_NODE_TEXT, so set that here.
            WUX::Controls::ToolTipService::SetToolTip(_lastImage, box_value(text));
        }
        else
        {
            // Otherwise, just add the text to the current paragraph
            _NewRun().Text(text);
        }
    }

    break;

    case CMARK_NODE_LINEBREAK:
        _EndSpan();
        _CurrentParagraph().Inlines().Append(WUX::Documents::LineBreak());
        break;

    case CMARK_NODE_SOFTBREAK:
        // I'm fairly confident this is what happens when you've just got
        // two lines only separated by a single \r\n in a MD doc. E.g. when
        // you want a paragraph to wrap at 80 columns in code, but wrap in
        // the rendered document.
        //
        // In the HTML implementation, what happens here depends on the options:
        // * CMARK_OPT_HARDBREAKS: add a full line break
        // * CMARK_OPT_NOBREAKS: Just add a space
        // * otherwise, just add a '\n'
        //
        // We're not really messing with options here, so lets just add a
        // space. That seems to keep the current line going, but allow for
        // word breaking.

        _NewRun().Text(L" ");
        break;

    case CMARK_NODE_CODE:
    {
        const auto text{ winrt::to_hstring(textFromLiteral(node)) };
        const auto& codeRun{ _NewRun() };

        codeRun.FontFamily(WUX::Media::FontFamily{ CodeFontFamily });
        // A Span can't have a border or a background, so we can't give
        // it the whole treatment that a <code> span gets in HTML.
        codeRun.Text(text);

        _NewRun().FontFamily(_root.FontFamily());
    }
    break;

    case CMARK_NODE_HTML_INLINE:
        // Same as above - no raw HTML support here.
        break;

    case CMARK_NODE_CUSTOM_INLINE:
        // Same as above - not even entirely sure what this is.
        break;

    case CMARK_NODE_STRONG:
        _NewRun().FontWeight(entering ?
                                 winrt::Windows::UI::Text::FontWeights::Bold() :
                                 winrt::Windows::UI::Text::FontWeights::Normal());
        break;

    case CMARK_NODE_EMPH:
        _NewRun().FontStyle(entering ?
                                winrt::Windows::UI::Text::FontStyle::Italic :
                                winrt::Windows::UI::Text::FontStyle::Normal);
        break;

    case CMARK_NODE_LINK:

        if (entering)
        {
            const auto urlHstring{ to_hstring(textFromUrl(node)) };
            WUX::Documents::Hyperlink a{};

            // Set the tooltip to display the URL
            try
            {
                // This block is from TermControl.cpp, where we sanitize the
                // tooltips for URLs. That has a much more comprehensive
                // comment.

                const winrt::Windows::Foundation::Uri uri{ _baseUri, urlHstring };

                a.NavigateUri(uri);

                auto tooltipText = urlHstring;
                const auto unicode = uri.AbsoluteUri();
                const auto punycode = uri.AbsoluteCanonicalUri();
                if (punycode != unicode)
                {
                    tooltipText = winrt::hstring{ punycode + L"\n" + unicode };
                }
                WUX::Controls::ToolTipService::SetToolTip(a, box_value(tooltipText));
            }
            catch (...)
            {
            }

            _CurrentParagraph().Inlines().Append(a);
            _lastSpan = a;

            // Similar to the header element, the actual text of the link
            // will later come through as a CMARK_NODE_TEXT
        }
        else
        {
            _EndSpan();
        }
        break;

    case CMARK_NODE_IMAGE:
        if (entering)
        {
            const auto urlHstring{ to_hstring(textFromUrl(node)) };

            try
            {
                winrt::Windows::Foundation::Uri uri{ _baseUri, urlHstring };

                WUX::Media::Imaging::BitmapImage bitmapImage;
                bitmapImage.UriSource(uri);

                WUX::Controls::Image img{};
                img.Source(bitmapImage);

                WUX::Documents::InlineUIContainer imageBlock{};
                imageBlock.Child(img);

                _CurrentParagraph().Inlines().Append(imageBlock);
                _lastImage = img;
            }
            catch (...)
            {
            }
        }
        else
        {
            _EndSpan();
            _lastImage = nullptr;
        }
        break;

        // These elements are in cmark-gfm, which we'd love to move to in the
        // future, but isn't yet available in vcpkg.

        // case CMARK_NODE_FOOTNOTE_DEFINITION:
        //     // Not supported currently
        //     break;
        //
        // case CMARK_NODE_FOOTNOTE_REFERENCE:
        //     // Not supported currently
        //     break;

    default:
        assert(false);
        break;
    }
}
